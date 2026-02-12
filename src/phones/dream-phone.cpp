/**
 * Dream Phone Configuration
 * 
 * This phone has different DTMF characteristics than Bowie Phone:
 * - Detected frequencies are shifted from standard DTMF
 * - Row 0 (697Hz) detected at ~642Hz (-55Hz)
 * - Column frequencies shifted +20-50Hz
 * - Dial tone (350+440Hz) harmonics interfere with DTMF detection
 * 
 * Detection strategy:
 * 1. Higher magnitude thresholds to reject dial tone harmonics
 * 2. Tighter frequency tolerance to avoid false matches
 * 3. Dial tone rejection in FFT processing
 * 4. More samples required before key-down confirmation
 */

#include "phone.h"

// Only compile this if PHONE is set to DREAM_PHONE
#if defined(PHONE) && PHONE == DREAM_PHONE

// Standard DTMF keypad (shared)
const char DTMF_KEYPAD[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// Dream Phone summed frequency table
// These map to COLUMNS only (not individual buttons)
// Adjusted for this phone's shifted frequencies
// Row frequencies are ~10Hz high, Col frequencies are ~23Hz high
// So summed = (row + col) will be ~33Hz higher than standard
static const PhoneSummedFreqEntry DREAM_SUMMED_FREQ_TABLE[] = {
    // Column 0: row_avg(~835) + col0(1232) = ~2067, but intermodulation varies
    // Standard would be ~2240, this phone likely ~2270-2290
    {2280.0, '1'},  // represents column 0
    
    // Column 1: row_avg(~835) + col1(1358) = ~2193
    // Standard would be ~2455, this phone likely ~2490-2510
    {2500.0, '2'},  // represents column 1
    
    // Column 2: row_avg(~835) + col2(1500) = ~2335
    // Standard would be ~2713, this phone likely ~2750-2770
    {2760.0, '3'},  // represents column 2
};

static const int DREAM_SUMMED_FREQ_TABLE_SIZE = 
    sizeof(DREAM_SUMMED_FREQ_TABLE) / sizeof(PhoneSummedFreqEntry);

// Dream Phone configuration
static const PhoneConfig DREAM_PHONE_CONFIG = {
    // Identification
    .name = "Dream Phone",
    .description = "ESP32-A1S AudioKit with SLIC - shifted frequencies, dial tone interference",
    
    // Frequency scaling
    .freqScale = 1.0f,
    
    // Detection thresholds
    // Try lower threshold with standard frequencies
    .fundamentalMagnitudeThreshold = 20.0f,  // Lower threshold for standard DTMF
    .summedMagnitudeThreshold = 100.0f,      // Strong signals (200-500 observed)
    .freqTolerance = 50.0f,                  // Standard tolerance
    .summedFreqTolerance = 70.0f,            // Hz tolerance for summed frequency
    
    // Detection timing
    .detectionCooldown = 300,                // 300ms between detections
    .gapThreshold = 150,                     // 150ms silence = new button press
    .requiredConsecutive = 4,                // Increased from 3 - more samples for reliability
    
    // Goertzel-specific timing
    .goertzelBlockTimeoutMs = 5,             // Row+Col must arrive within 5ms
    .goertzelReleaseMs = 80,                 // Key released after 80ms silence
    .goertzelBlockSize = 512,                // ~11.6ms blocks @ 44100Hz
    .goertzelCopierBufferSize = 512,         // StreamCopy buffer size
    
    // Detection mode
    .useSummedFreqDetection = true,          // Use summed frequencies for COLUMN
    .useFundamentalDetection = true,         // Also try fundamentals when strong enough
    .summedTriggersRowCheck = true,          // When summed detected, verify with row freq
    
    // Summed frequency table (maps to columns only)
    .summedFreqTable = DREAM_SUMMED_FREQ_TABLE,
    .summedFreqTableSize = DREAM_SUMMED_FREQ_TABLE_SIZE,
    
    // Row frequencies - TRY STANDARD DTMF first
    // Standard: 697, 770, 852, 941 Hz
    // Previous calibration was: 705, 779, 867, 950 (shifted +8-15Hz)
    // Goertzel is more precise - try standard first
    .rowFreqs = {697.0f, 770.0f, 852.0f, 941.0f},
    
    // Column frequencies - TRY STANDARD DTMF first
    // Standard: 1209, 1336, 1477, 1633 Hz
    // Previous calibration was: 1232, 1358, 1500, 1658 (shifted +22-25Hz)
    .colFreqs = {1209.0f, 1336.0f, 1477.0f, 1633.0f},
};

const PhoneConfig& getPhoneConfig() {
    return DREAM_PHONE_CONFIG;
}

// Helper: Decode button from summed frequency
char decodeFromSummedFreq(float freq) {
    const PhoneConfig& config = DREAM_PHONE_CONFIG;
    
    for (int i = 0; i < config.summedFreqTableSize; i++) {
        float diff = freq - config.summedFreqTable[i].freq;
        if (diff < 0) diff = -diff;
        if (diff <= config.summedFreqTolerance) {
            return config.summedFreqTable[i].button;
        }
    }
    return 0; // No match
}

// Helper: Find closest frequency in array
int findClosestFreq(float freq, const float* freqArray, int arraySize, float tolerance) {
    int bestMatch = -1;
    float bestDiff = tolerance + 1;
    
    for (int i = 0; i < arraySize; i++) {
        float diff = freq - freqArray[i];
        if (diff < 0) diff = -diff;
        if (diff <= tolerance && diff < bestDiff) {
            bestDiff = diff;
            bestMatch = i;
        }
    }
    return bestMatch;
}

// Helper: Decode from row and column frequencies
char decodeFromRowCol(float rowFreq, float colFreq) {
    const PhoneConfig& config = DREAM_PHONE_CONFIG;
    
    int row = findClosestFreq(rowFreq, config.rowFreqs, 4, config.freqTolerance);
    int col = findClosestFreq(colFreq, config.colFreqs, 4, config.freqTolerance);
    
    if (row >= 0 && col >= 0) {
        return DTMF_KEYPAD[row][col];
    }
    return 0;
}

#endif // PHONE == DREAM_PHONE
