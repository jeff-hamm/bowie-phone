#include "dtmf_goertzel.h"
#include "logging.h"
#include "config.h"

// Forward declaration for FFT debug mode (from dtmf_decoder.cpp)
bool isFFTDebugEnabled();

// DTMF keypad lookup for Goertzel decoder
static const char GOERTZEL_DTMF_KEYPAD[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// Goertzel DTMF detection state
static int goertzelDetectedRow = -1;
static int goertzelDetectedCol = -1;
static char goertzelPendingKey = 0;
static unsigned long goertzelBlockStartTime = 0;
static const unsigned long GOERTZEL_BLOCK_TIMEOUT_MS = 5;  // Row+Col must arrive within same block (~5ms)

// Gap detection state for debouncing
static char goertzelLastEmittedKey = 0;      // Last key that was emitted
static unsigned long goertzelLastSignalTime = 0;  // When we last saw a valid DTMF signal
static const unsigned long GOERTZEL_GAP_MS = 100;  // Require 100ms gap before new key can be emitted

// Reference structure for Goertzel DTMF frequencies
struct GoertzelDTMFRef {
    enum Type { Row, Col };
    Type type;
    int index;
};

// Goertzel callback - called when a frequency is detected above threshold
void onGoertzelFrequency(float frequency, float magnitude, void* ref) {
    if (ref == nullptr) return;
    
    unsigned long now = millis();
    
    // Check if previous detection timed out (row without column = stale)
    if (goertzelDetectedRow >= 0 && (now - goertzelBlockStartTime) > GOERTZEL_BLOCK_TIMEOUT_MS) {
        if (isFFTDebugEnabled()) {
            Logger.printf("🎵 Goertzel: row %d timed out (no column within %lums)\n", 
                         goertzelDetectedRow, GOERTZEL_BLOCK_TIMEOUT_MS);
        }
        goertzelDetectedRow = -1;
    }
    
    GoertzelDTMFRef* dtmfRef = (GoertzelDTMFRef*)ref;
    
    if (dtmfRef->type == GoertzelDTMFRef::Row) {
        goertzelDetectedRow = dtmfRef->index;
        goertzelBlockStartTime = now;  // Start timing window
        
        if (isFFTDebugEnabled()) {
            Logger.printf("🎵 Goertzel: row %d (%.0f Hz, mag=%.1f)\n", 
                         dtmfRef->index, frequency, magnitude);
        }
    } else {
        // Column detected - check if we have a valid row within timeout
        if (goertzelDetectedRow >= 0 && (now - goertzelBlockStartTime) <= GOERTZEL_BLOCK_TIMEOUT_MS) {
            goertzelDetectedCol = dtmfRef->index;
            char key = GOERTZEL_DTMF_KEYPAD[goertzelDetectedRow][goertzelDetectedCol];
            
            // Update signal time (we're seeing valid DTMF)
            goertzelLastSignalTime = now;
            
            // Only emit key if:
            // 1. It's a different key than last emitted, OR
            // 2. There was a gap since the last key was emitted
            bool canEmit = (key != goertzelLastEmittedKey) || 
                          (goertzelPendingKey == 0 && goertzelLastEmittedKey == 0);
            
            if (canEmit && goertzelPendingKey == 0) {
                goertzelPendingKey = key;
                
                if (isFFTDebugEnabled()) {
                    Logger.printf("🎵 Goertzel DTMF: %c (col=%d, %.0f Hz, mag=%.1f)\n", 
                                 key, dtmfRef->index, frequency, magnitude);
                }
            }
            
            // Reset for next detection
            goertzelDetectedRow = -1;
            goertzelDetectedCol = -1;
        } else if (isFFTDebugEnabled()) {
            Logger.printf("🎵 Goertzel: col %d orphaned (no recent row)\n", dtmfRef->index);
        }
    }
}

// Static references for Goertzel (must persist)
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

// Initialize Goertzel-based DTMF decoder
// More efficient than FFT when only detecting specific frequencies
void initGoertzelDecoder(GoertzelStream &goertzel, StreamCopy &copier)
{
    // Add DTMF row frequencies (697, 770, 852, 941 Hz)
    goertzel.addFrequency(697.0f, &rowRefs[0]);
    goertzel.addFrequency(770.0f, &rowRefs[1]);
    goertzel.addFrequency(852.0f, &rowRefs[2]);
    goertzel.addFrequency(941.0f, &rowRefs[3]);
    
    // Add DTMF column frequencies (1209, 1336, 1477 Hz)
    // Note: 1633 Hz (column D) omitted - rarely used on consumer phones
    goertzel.addFrequency(1209.0f, &colRefs[0]);
    goertzel.addFrequency(1336.0f, &colRefs[1]);
    goertzel.addFrequency(1477.0f, &colRefs[2]);
    // goertzel.addFrequency(1633.0f, &colRefs[3]);  // Uncomment if 'D' key needed
    
    // Set detection callback
    goertzel.setFrequencyDetectionCallback(onGoertzelFrequency);
    
    // Configure Goertzel parameters
    auto cfg = goertzel.defaultConfig();
    cfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    cfg.threshold = 30.0f;      // High threshold to reject dial tone harmonics (350/440Hz)
    cfg.block_size = 512;       // ~11.6ms blocks @ 44100Hz - faster response
    goertzel.begin(cfg);
    
    // Use smaller copier buffer for faster response (512 = ~2.9ms @ 44.1kHz stereo)
    copier.resize(512);
    
    Logger.println("🎵 Goertzel DTMF decoder initialized (7 frequencies, block=512, buf=512, thresh=30)");
}

// Get pending key from Goertzel decoder
// Includes gap detection - same key won't be returned until a gap is detected
char getGoertzelKey() {
    unsigned long now = millis();
    
    // Check for gap - if no signal for GOERTZEL_GAP_MS, allow same key to be detected again
    if (goertzelLastEmittedKey != 0 && (now - goertzelLastSignalTime) > GOERTZEL_GAP_MS) {
        // Gap detected - reset last emitted key so same key can be detected again
        goertzelLastEmittedKey = 0;
    }
    
    char key = goertzelPendingKey;
    if (key != 0) {
        goertzelPendingKey = 0;
        goertzelLastEmittedKey = key;  // Track what we emitted
    }
    return key;
}

// Reset Goertzel detection state (clear any stale partial detections)
void resetGoertzelState() {
    goertzelDetectedRow = -1;
    goertzelDetectedCol = -1;
    goertzelPendingKey = 0;
    goertzelBlockStartTime = 0;
    goertzelLastEmittedKey = 0;
    goertzelLastSignalTime = 0;
}

// ============================================================================
// FREERTOS TASK FOR GOERTZEL PROCESSING
// ============================================================================

static TaskHandle_t goertzelTaskHandle = nullptr;
static StreamCopy* goertzelCopierPtr = nullptr;
static volatile bool goertzelTaskShouldRun = false;

// Goertzel task - runs on core 0 to avoid blocking audio on core 1
void goertzelTaskFunction(void* parameter) {
    StreamCopy* copier = (StreamCopy*)parameter;
    
    Logger.println("🎵 Goertzel task started on core 0");
    
    while (goertzelTaskShouldRun) {
        // Copy audio data from mic to Goertzel decoder
        // This blocks briefly while reading mic data
        copier->copy();
        
        // Small yield to prevent watchdog issues
        vTaskDelay(1);  // 1ms delay between copies
    }
    
    Logger.println("🎵 Goertzel task stopped");
    goertzelTaskHandle = nullptr;
    vTaskDelete(NULL);
}

// Start Goertzel processing on a separate FreeRTOS task (core 0)
void startGoertzelTask(StreamCopy &copier) {
    if (goertzelTaskHandle != nullptr) {
        Logger.println("⚠️ Goertzel task already running");
        return;
    }
    
    goertzelCopierPtr = &copier;
    goertzelTaskShouldRun = true;
    
    // Create task on core 0 (audio typically runs on core 1)
    // Priority 1 (low) - audio should have higher priority
    // Stack size 4096 bytes should be sufficient
    xTaskCreatePinnedToCore(
        goertzelTaskFunction,    // Task function
        "GoertzelTask",          // Task name
        4096,                    // Stack size
        &copier,                 // Parameter (copier pointer)
        1,                       // Priority (low)
        &goertzelTaskHandle,     // Task handle
        0                        // Core 0 (audio on core 1)
    );
    
    Logger.println("🎵 Goertzel task created on core 0");
}

// Stop the Goertzel task
void stopGoertzelTask() {
    if (goertzelTaskHandle == nullptr) {
        return;
    }
    
    goertzelTaskShouldRun = false;
    
    // Wait for task to finish (max 100ms)
    int timeout = 100;
    while (goertzelTaskHandle != nullptr && timeout > 0) {
        vTaskDelay(1);
        timeout--;
    }
    
    if (goertzelTaskHandle != nullptr) {
        // Force delete if still running
        vTaskDelete(goertzelTaskHandle);
        goertzelTaskHandle = nullptr;
    }
}

// Check if Goertzel task is running
bool isGoertzelTaskRunning() {
    return goertzelTaskHandle != nullptr && goertzelTaskShouldRun;
}
