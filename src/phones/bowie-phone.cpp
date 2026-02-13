/**
 * Bowie Phone Configuration (Repaired Hardware - Feb 2026)
 * 
 * After SLIC hardware repair, this phone now produces standard DTMF fundamentals:
 *   - Clear row frequencies: 697, 770, 852, 941 Hz
 *   - Clear column frequencies: 1209, 1336, 1477, 1633 Hz
 *   - Both bands present simultaneously during key press
 * 
 * Signal characteristics (from offline CSV analysis at 22050 Hz):
 *   - Row band (low) magnitudes are ~2-5x weaker than column band (high)
 *   - Twist ratio (high/low) ranges from ~2:1 to ~9:1 depending on button
 *   - Noise floor Goertzel magnitude: ~29K (at N=5512)
 *   - Active tone Goertzel magnitude: ~500K-15M (at N=5512)
 *   - Clear separation between noise and signal
 * 
 * Detection strategy:
 *   Standard Goertzel for all 8 DTMF frequencies with:
 *   1. Per-block magnitude accumulation (strongest row + strongest col)
 *   2. Twist ratio check (max 12:1 to accommodate asymmetric bands)  
 *   3. Consecutive-block debouncing (3 blocks = ~139ms at block_size=2048)
 *   4. Key release after 4 consecutive silent blocks (~185ms)
 * 
 * Verified tones from signal analysis:
 *   '#' = 941 Hz + 1477 Hz (twist ~4-9x)
 *   '1' = 697 Hz + 1209 Hz (twist ~2.5x)
 */

#include "phone.h"

// Only compile this if PHONE is not defined or is set to BOWIE_PHONE
#if !defined(PHONE) || PHONE == BOWIE_PHONE

// Standard DTMF keypad (shared)
const char DTMF_KEYPAD[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// Bowie Phone configuration - standard DTMF (repaired hardware)
static const PhoneConfig BOWIE_PHONE_CONFIG = {
    // Identification
    .name = "Bowie Phone",
    .description = "ESP32-A1S AudioKit with SLIC - standard DTMF fundamentals (repaired)",
    
    // Frequency scaling
    .freqScale = 1.0f,
    
    // Detection thresholds
    // GoertzelStream fires callback when magnitude > threshold
    // With normalized magnitudes: noise ~0.1-2.0, real tones ~50-500+
    // Set low enough to catch weak row frequencies, debouncing handles noise
    .fundamentalMagnitudeThreshold = 10.0f,
    .summedMagnitudeThreshold = 0.0f,         // Not used - standard DTMF only
    .freqTolerance = 75.0f,                   // Hz tolerance for freq matching
    .summedFreqTolerance = 0.0f,              // Not used
    
    // Detection timing
    .detectionCooldown = 200,                 // 200ms between distinct digit emissions
    .gapThreshold = 120,                      // 120ms silence = button released
    .requiredConsecutive = 3,                 // 3 consecutive matching blocks to confirm
    
    // Goertzel-specific timing
    .goertzelBlockTimeoutMs = 30,             // Row+Col must be in same block (generous)
    .goertzelReleaseMs = 120,                 // Key released after 120ms silence
    .goertzelBlockSize = 2048,                // ~46ms blocks @ 44100Hz (good SNR, low overhead)
    .goertzelCopierBufferSize = 2048,         // Match block size for efficiency
    
    // Detection mode - standard DTMF fundamentals only
    .useSummedFreqDetection = false,
    .useFundamentalDetection = true,
    .summedTriggersRowCheck = false,
    
    // No summed frequency table needed (repaired hardware has good fundamentals)
    .summedFreqTable = nullptr,
    .summedFreqTableSize = 0,
    
    // Standard DTMF row frequencies
    .rowFreqs = {697.0f, 770.0f, 852.0f, 941.0f},
    
    // Standard DTMF column frequencies
    .colFreqs = {1209.0f, 1336.0f, 1477.0f, 1633.0f},
};

const PhoneConfig& getPhoneConfig() {
    return BOWIE_PHONE_CONFIG;
}

// Helper: Decode COLUMN from summed frequency - NOT USED for repaired hardware
static int getColumnFromSummedFreq(float freq) {
    (void)freq;
    return -1;
}

// Helper: Decode button from summed frequency - NOT USED for repaired hardware
char decodeFromSummedFreq(float freq) {
    (void)freq;
    return 0;
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
    const PhoneConfig& config = BOWIE_PHONE_CONFIG;
    
    int row = findClosestFreq(rowFreq, config.rowFreqs, 4, config.freqTolerance);
    int col = findClosestFreq(colFreq, config.colFreqs, 4, config.freqTolerance);
    
    if (row >= 0 && col >= 0) {
        return DTMF_KEYPAD[row][col];
    }
    return 0;
}

#endif // PHONE == BOWIE_PHONE
