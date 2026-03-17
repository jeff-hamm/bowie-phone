#include "dtmf_goertzel.h"
#include "logging.h"
#include "config.h"
#include "phone.h"
#ifdef TEST_MODE
#include "test_helpers/dtmf_goertzel_test_helpers.h"
#endif

// ============================================================================
// GOERTZEL DTMF DETECTOR — Block-accumulation with debounce
//
// Architecture:
//   GoertzelStream fires a callback for each DTMF frequency above threshold
//   within each block of audio. We accumulate the magnitudes, find the
//   strongest row and column, apply twist+magnitude checks, and require
//   multiple consecutive matching blocks before emitting a digit.
//
// Key parameters (from PhoneConfig):
//   - fundamentalMagnitudeThreshold: GoertzelStream callback threshold
//   - goertzelBlockSize: samples per Goertzel block (~46ms at 2048/44100)
//   - requiredConsecutive: blocks needed to confirm a digit (3 = ~139ms)
//   - goertzelReleaseMs: silence duration to consider key released
//
// Bowie Phone specifics:
//   - High band (cols) is 2-9x stronger than low band (rows)
//   - Max twist ratio set to 12:1 to accommodate this asymmetry
//   - Standard DTMF fundamentals (no summed frequency detection needed)
// ============================================================================

// DTMF keypad lookup
static const char GOERTZEL_DTMF_KEYPAD[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// Maximum twist ratio (high/low magnitude) to accept as valid DTMF
// Bowie Phone has asymmetric band magnitudes: high band 2-9x stronger than low
static const float MAX_TWIST_RATIO = 12.0f;

// Number of consecutive silent blocks before considering key released
static const int RELEASE_BLOCK_COUNT = 4;

// Reference structure for Goertzel DTMF frequencies
struct GoertzelDTMFRef {
    enum Type { Row, Col };
    Type type;
    int index;
};

// ============================================================================
// DETECTION STATE
// ============================================================================

// Per-block magnitude accumulators (written in callback, read in evaluateBlock)
static volatile float blockRowMags[4] = {0, 0, 0, 0};
static volatile float blockColMags[4] = {0, 0, 0, 0};
static volatile bool blockDataReady = false;

// Consecutive detection state
static char candidateDigit = 0;        // Current digit candidate being evaluated
static int consecutiveHits = 0;        // How many consecutive blocks matched candidateDigit
static int consecutiveMisses = 0;      // How many consecutive blocks had no valid detection

// Key emission state (thread-safe: written here, read from main loop)
static volatile char goertzelPendingKey = 0;  // Key waiting to be consumed by getGoertzelKey()
static char emittedKey = 0;                   // Last emitted key (suppress repeat emission)

// Mute flag — when true, evaluateBlock() skips detection entirely.
// Used to suppress false DTMF from ES8388 DAC→ADC internal loopback during playback.
static volatile bool goertzelMuted = false;

// ============================================================================
// GOERTZEL CALLBACK
//
// Called by GoertzelStream for each registered frequency whose magnitude
// exceeds the threshold. All callbacks for one block happen sequentially
// within the same copy() call, so no threading concern here.
// ============================================================================

void onGoertzelFrequency(float frequency, float magnitude, void* ref) {
    if (ref == nullptr) return;
    
    GoertzelDTMFRef* dtmfRef = (GoertzelDTMFRef*)ref;
    
    if (dtmfRef->type == GoertzelDTMFRef::Row) {
        // Keep the strongest row magnitude per block
        if (magnitude > blockRowMags[dtmfRef->index]) {
            blockRowMags[dtmfRef->index] = magnitude;
        }
    } else {
        // Keep the strongest col magnitude per block
        if (magnitude > blockColMags[dtmfRef->index]) {
            blockColMags[dtmfRef->index] = magnitude;
        }
    }
    blockDataReady = true;
}

// ============================================================================
// BLOCK EVALUATION
//
// Called from the Goertzel task after each copy() cycle.
// Looks at accumulated magnitudes and runs the detection state machine.
// ============================================================================

static void evaluateBlock() {
    if (goertzelMuted) {
        // Discard accumulated data — DAC→ADC loopback would cause false detections
        for (int i = 0; i < 4; i++) {
            blockRowMags[i] = 0;
            blockColMags[i] = 0;
        }
        blockDataReady = false;
        return;
    }
    
    const PhoneConfig& config = getPhoneConfig();
    
    if (!blockDataReady) {
        // No frequencies above threshold in this cycle — silence
        consecutiveMisses++;
        
        if (consecutiveMisses >= RELEASE_BLOCK_COUNT) {
            // Key released — reset for next press
            if (emittedKey != 0) {
                Logger.printf("🎵 Goertzel: key '%c' released (silence)\n", emittedKey);
                emittedKey = 0;
            }
            candidateDigit = 0;
            consecutiveHits = 0;
        }
        return;
    }
    
    // Find strongest row and strongest column
    int bestRow = -1;
    float bestRowMag = 0;
    int bestCol = -1;
    float bestColMag = 0;
    
    for (int i = 0; i < 4; i++) {
        if (blockRowMags[i] > bestRowMag) {
            bestRowMag = blockRowMags[i];
            bestRow = i;
        }
        if (blockColMags[i] > bestColMag) {
            bestColMag = blockColMags[i];
            bestCol = i;
        }
    }
    
    // Reset accumulators for next block
    for (int i = 0; i < 4; i++) {
        blockRowMags[i] = 0;
        blockColMags[i] = 0;
    }
    blockDataReady = false;
    
    // Need BOTH a row and a column to be a valid DTMF tone
    if (bestRow < 0 || bestCol < 0) {
        consecutiveMisses++;
        if (consecutiveMisses >= RELEASE_BLOCK_COUNT) {
            if (emittedKey != 0) {
                Logger.printf("🎵 Goertzel: key '%c' released (partial)\n", emittedKey);
                emittedKey = 0;
            }
            candidateDigit = 0;
            consecutiveHits = 0;
        }
        return;
    }
    
    // Magnitude floor — reject weak loopback artifacts from ES8388 DAC→ADC
    // Real DTMF presses produce magnitudes in the hundreds; loopback gives 12-24
    if (bestRowMag < config.minDetectionMagnitude || bestColMag < config.minDetectionMagnitude) {
        consecutiveMisses++;
        return;
    }
    
    // Twist check — reject if band magnitudes are too imbalanced
    float maxMag = (bestRowMag > bestColMag) ? bestRowMag : bestColMag;
    float minMag = (bestRowMag < bestColMag) ? bestRowMag : bestColMag;
    
    if (minMag <= 0 || (maxMag / minMag) > MAX_TWIST_RATIO) {
        // Too imbalanced — likely noise or single-band interference
        consecutiveMisses++;
        return;
    }
    
    // Valid detection — decode the digit
    consecutiveMisses = 0;
    char digit = GOERTZEL_DTMF_KEYPAD[bestRow][bestCol];
    
    // Consecutive detection debouncing
    if (digit == candidateDigit) {
        consecutiveHits++;
    } else {
        // Different digit — start new candidate
        candidateDigit = digit;
        consecutiveHits = 1;
    }
    
    // Emit when we have enough consecutive matching blocks AND it's a new key
    if (consecutiveHits >= config.requiredConsecutive && digit != emittedKey) {
        emittedKey = digit;
        goertzelPendingKey = digit;
        
        Logger.printf("🎵 Goertzel DTMF: '%c' (row=%d/%.0f col=%d/%.0f twist=%.1f hits=%d)\n",
                     digit, bestRow, bestRowMag, bestCol, bestColMag,
                     maxMag / minMag, consecutiveHits);
    }
}

// ============================================================================
// STATIC REFERENCES (must persist for GoertzelStream callbacks)
// ============================================================================

static GoertzelDTMFRef rowRefs[4] = {
    {GoertzelDTMFRef::Row, 0},
    {GoertzelDTMFRef::Row, 1},
    {GoertzelDTMFRef::Row, 2},
    {GoertzelDTMFRef::Row, 3}
};
static GoertzelDTMFRef colRefs[4] = {
    {GoertzelDTMFRef::Col, 0},
    {GoertzelDTMFRef::Col, 1},
    {GoertzelDTMFRef::Col, 2},
    {GoertzelDTMFRef::Col, 3}
};

// ============================================================================
// INITIALIZATION
// ============================================================================

void initGoertzelDecoder(GoertzelStream &goertzel, StreamCopy &copier)
{
    const PhoneConfig& config = getPhoneConfig();
    
    // Register all 4 DTMF row frequencies
    goertzel.addFrequency(config.rowFreqs[0], &rowRefs[0]);
    goertzel.addFrequency(config.rowFreqs[1], &rowRefs[1]);
    goertzel.addFrequency(config.rowFreqs[2], &rowRefs[2]);
    goertzel.addFrequency(config.rowFreqs[3], &rowRefs[3]);
    
    // Register all 4 DTMF column frequencies (including 1633 Hz for 'D' key)
    goertzel.addFrequency(config.colFreqs[0], &colRefs[0]);
    goertzel.addFrequency(config.colFreqs[1], &colRefs[1]);
    goertzel.addFrequency(config.colFreqs[2], &colRefs[2]);
    goertzel.addFrequency(config.colFreqs[3], &colRefs[3]);
    
    // Set detection callback
    goertzel.setFrequencyDetectionCallback(onGoertzelFrequency);
    
    // Configure Goertzel parameters
    auto cfg = goertzel.defaultConfig();
    cfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    cfg.threshold = config.fundamentalMagnitudeThreshold;
    cfg.block_size = config.goertzelBlockSize;
    goertzel.begin(cfg);
    
    // Size copier buffer to match block size for efficient transfer
    copier.resize(config.goertzelCopierBufferSize);
    
    Logger.printf("🎵 Goertzel DTMF decoder initialized for %s\n", config.name);
    Logger.printf("   Rows: %.0f, %.0f, %.0f, %.0f Hz\n", 
                  config.rowFreqs[0], config.rowFreqs[1], config.rowFreqs[2], config.rowFreqs[3]);
    Logger.printf("   Cols: %.0f, %.0f, %.0f, %.0f Hz\n",
                  config.colFreqs[0], config.colFreqs[1], config.colFreqs[2], config.colFreqs[3]);
    Logger.printf("   Block=%d samples (%.1fms), thresh=%.1f, floor=%.1f, consecutive=%d, twist<%.0f\n",
                  config.goertzelBlockSize,
                  config.goertzelBlockSize * 1000.0f / AUDIO_SAMPLE_RATE,
                  config.fundamentalMagnitudeThreshold,
                  config.minDetectionMagnitude,
                  config.requiredConsecutive,
                  MAX_TWIST_RATIO);
}

// ============================================================================
// PUBLIC API
// ============================================================================

char getGoertzelKey() {
    char key = goertzelPendingKey;
    if (key != 0) {
        goertzelPendingKey = 0;
    }
    return key;
}

void processGoertzelBlock() {
    evaluateBlock();
}

void resetGoertzelState() {
    for (int i = 0; i < 4; i++) {
        blockRowMags[i] = 0;
        blockColMags[i] = 0;
    }
    blockDataReady = false;
    candidateDigit = 0;
    consecutiveHits = 0;
    consecutiveMisses = 0;
    goertzelPendingKey = 0;
    emittedKey = 0;
}

#ifdef TEST_MODE
void processGoertzelSamplesForTest(GoertzelStream &goertzel, const int16_t* samples, size_t sampleCount) {
    if (samples == nullptr || sampleCount == 0) {
        return;
    }

    goertzel.write(reinterpret_cast<const uint8_t*>(samples), sampleCount * sizeof(int16_t));
    evaluateBlock();
}
#endif

// ============================================================================
// FREERTOS TASK FOR GOERTZEL PROCESSING
// ============================================================================

static TaskHandle_t goertzelTaskHandle = nullptr;
static StreamCopy* goertzelCopierPtr = nullptr;
static volatile bool goertzelTaskShouldRun = false;
static volatile bool goertzelTaskStarted = false;  // true once task loop begins

// Goertzel task — runs on core 0 to avoid blocking audio on core 1
// Each iteration: copy audio → GoertzelStream fires callbacks → evaluate
void goertzelTaskFunction(void* parameter) {
    StreamCopy* copier = (StreamCopy*)parameter;
    
    // Wait for system to stabilize (WiFi init on core 0 uses significant
    // interrupt stack; launching Goertzel immediately causes stack canary trips)
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    goertzelTaskStarted = true;
    Logger.println("🎵 Goertzel task started on core 0");
    
    while (goertzelTaskShouldRun) {
        // Copy audio data from mic to Goertzel decoder
        // This triggers Goertzel computation and callbacks when a block is ready
        copier->copy();
        
        // Evaluate accumulated block data (if a block completed during copy)
        evaluateBlock();
        
        // Small yield to prevent watchdog issues
        vTaskDelay(1);
    }
    
    Logger.println("🎵 Goertzel task stopped");
    goertzelTaskStarted = false;
    goertzelTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void startGoertzelTask(StreamCopy &copier) {
    if (goertzelTaskHandle != nullptr) {
        Logger.println("⚠️ Goertzel task already running");
        return;
    }
    
    goertzelCopierPtr = &copier;
    goertzelTaskShouldRun = true;
    
    xTaskCreatePinnedToCore(
        goertzelTaskFunction,
        "GoertzelTask",
        16384,
        &copier,
        1,
        &goertzelTaskHandle,
        0  // Core 0
    );
    
    Logger.println("🎵 Goertzel task created on core 0");
}

void stopGoertzelTask() {
    if (goertzelTaskHandle == nullptr) {
        return;
    }
    
    goertzelTaskShouldRun = false;
    
    // Wait up to 4 seconds for task to exit (accounts for 3s startup delay)
    int timeout = 4000;
    while (goertzelTaskHandle != nullptr && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout -= 10;
    }
    
    if (goertzelTaskHandle != nullptr) {
        vTaskDelete(goertzelTaskHandle);
        goertzelTaskHandle = nullptr;
    }
}

bool isGoertzelTaskRunning() {
    return goertzelTaskHandle != nullptr && goertzelTaskShouldRun;
}

void setGoertzelMuted(bool muted) {
    if (goertzelMuted != muted) {
        goertzelMuted = muted;
        Logger.printf("🎵 Goertzel %s\n", muted ? "MUTED (playback active)" : "UNMUTED (listening)");
        if (muted) {
            resetGoertzelState();
        }
    }
}

bool isGoertzelMuted() {
    return goertzelMuted;
}
