#include "dtmf_goertzel.h"
#include "logging.h"
#include "config.h"
#include "phone.h"  // For phone-specific DTMF frequencies

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

// Debounce state - track the current key being held
static char goertzelCurrentKey = 0;          // Key currently being detected (held down)
static unsigned long goertzelKeyStartTime = 0;  // When current key detection started
static unsigned long goertzelKeyLastSeen = 0;   // When we last saw the current key

// Reference structure for Goertzel DTMF frequencies
struct GoertzelDTMFRef {
    enum Type { Row, Col };
    Type type;
    int index;
};

// Goertzel callback - called when a frequency is detected above threshold
void onGoertzelFrequency(float frequency, float magnitude, void* ref) {
    if (ref == nullptr) return;
    
    const PhoneConfig& config = getPhoneConfig();
    unsigned long now = millis();
    
    // Check if previous detection timed out (row without column = stale)
    if (goertzelDetectedRow >= 0 && (now - goertzelBlockStartTime) > config.goertzelBlockTimeoutMs) {
        if (isFFTDebugEnabled()) {
            Logger.printf("🎵 Goertzel: row %d timed out (no column within %lums)\n", 
                         goertzelDetectedRow, config.goertzelBlockTimeoutMs);
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
        if (goertzelDetectedRow >= 0 && (now - goertzelBlockStartTime) <= config.goertzelBlockTimeoutMs) {
            goertzelDetectedCol = dtmfRef->index;
            char key = GOERTZEL_DTMF_KEYPAD[goertzelDetectedRow][goertzelDetectedCol];
            
            // Update when we last saw this key
            goertzelKeyLastSeen = now;
            
            if (key == goertzelCurrentKey) {
                // Same key still being held - do nothing, already emitted
            } else if (goertzelCurrentKey == 0) {
                // No key was active - this is a new press
                goertzelCurrentKey = key;
                goertzelKeyStartTime = now;
                goertzelPendingKey = key;  // Emit immediately
                
                if (isFFTDebugEnabled()) {
                    Logger.printf("🎵 Goertzel DTMF NEW: %c (row=%d, col=%d)\n", 
                                 key, goertzelDetectedRow, dtmfRef->index);
                }
            } else {
                // Different key than current - previous must have been released
                // Start tracking new key
                goertzelCurrentKey = key;
                goertzelKeyStartTime = now;
                goertzelPendingKey = key;  // Emit new key
                
                if (isFFTDebugEnabled()) {
                    Logger.printf("🎵 Goertzel DTMF SWITCH: %c (was %c)\n", key, goertzelCurrentKey);
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
// Uses phone-specific frequencies from PhoneConfig
void initGoertzelDecoder(GoertzelStream &goertzel, StreamCopy &copier)
{
    const PhoneConfig& config = getPhoneConfig();
    
    // Add DTMF row frequencies from phone config
    goertzel.addFrequency(config.rowFreqs[0], &rowRefs[0]);
    goertzel.addFrequency(config.rowFreqs[1], &rowRefs[1]);
    goertzel.addFrequency(config.rowFreqs[2], &rowRefs[2]);
    goertzel.addFrequency(config.rowFreqs[3], &rowRefs[3]);
    
    // Add DTMF column frequencies from phone config
    // Note: Column 3 (1633 Hz / 'D' key) omitted - rarely used on consumer phones
    goertzel.addFrequency(config.colFreqs[0], &colRefs[0]);
    goertzel.addFrequency(config.colFreqs[1], &colRefs[1]);
    goertzel.addFrequency(config.colFreqs[2], &colRefs[2]);
    // goertzel.addFrequency(config.colFreqs[3], &colRefs[3]);  // Uncomment if 'D' key needed
    
    // Set detection callback
    goertzel.setFrequencyDetectionCallback(onGoertzelFrequency);
    
    // Configure Goertzel parameters
    auto cfg = goertzel.defaultConfig();
    cfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    cfg.threshold = config.fundamentalMagnitudeThreshold;  // Use phone-specific threshold
    cfg.block_size = config.goertzelBlockSize;             // Phone-specific block size
    goertzel.begin(cfg);
    
    // Use phone-specific copier buffer size
    copier.resize(config.goertzelCopierBufferSize);
    
    Logger.printf("🎵 Goertzel DTMF decoder initialized for %s\n", config.name);
    Logger.debugf("   Rows: %.0f, %.0f, %.0f, %.0f Hz\n", 
                  config.rowFreqs[0], config.rowFreqs[1], config.rowFreqs[2], config.rowFreqs[3]);
    Logger.debugf("   Cols: %.0f, %.0f, %.0f Hz (thresh=%.1f, block=%d, release=%lums)\n",
                  config.colFreqs[0], config.colFreqs[1], config.colFreqs[2],
                  cfg.threshold, config.goertzelBlockSize, config.goertzelReleaseMs);
}

// Get pending key from Goertzel decoder
// Handles key release detection - if no signal for goertzelReleaseMs, key is considered released
char getGoertzelKey() {
    const PhoneConfig& config = getPhoneConfig();
    unsigned long now = millis();
    
    // Check if current key should be considered released (no detection for a while)
    if (goertzelCurrentKey != 0 && (now - goertzelKeyLastSeen) > config.goertzelReleaseMs) {
        goertzelCurrentKey = 0;  // Key released, ready for next press
    }
    
    char key = goertzelPendingKey;
    if (key != 0) {
        goertzelPendingKey = 0;
    }
    return key;
}

// Reset Goertzel detection state (clear any stale partial detections)
void resetGoertzelState() {
    goertzelDetectedRow = -1;
    goertzelDetectedCol = -1;
    goertzelPendingKey = 0;
    goertzelBlockStartTime = 0;
    goertzelCurrentKey = 0;
    goertzelKeyStartTime = 0;
    goertzelKeyLastSeen = 0;
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
