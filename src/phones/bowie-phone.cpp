/**
 * Bowie Phone Configuration
 * 
 * This phone exhibits unusual DTMF characteristics:
 * - Weak fundamental DTMF frequencies (0.1-0.2 magnitude)
 * - Strong intermodulation products at the sum of row+col frequencies
 * - The summed frequencies only distinguish COLUMNS, not individual buttons
 * 
 * Detection strategy:
 * 1. Use strong summed frequency to detect which COLUMN was pressed
 * 2. Check weak fundamental row frequencies to determine the ROW
 * 3. Combine column + row to determine the actual button
 * 
 * Observed summed frequencies (all buttons in same column output same freq):
 * - Column 0 (1209Hz): ~2240Hz → buttons 1, 4, 7, *
 * - Column 1 (1336Hz): ~2455Hz → buttons 2, 5, 8, 0
 * - Column 2 (1477Hz): ~2713Hz → buttons 3, 6, 9, #
 * 
 * The summed frequency only tells us the COLUMN. We must check the row
 * frequency (697, 770, 852, 941 Hz) to determine the specific button.
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

// Bowie Phone specific summed frequency table
// These map to COLUMNS only (not individual buttons)
// The row must be determined from the weak fundamental row frequency
// Format: {detected_frequency, representative_button_char}
// The button char is from row 0 of that column (1, 2, 3)
static const PhoneSummedFreqEntry BOWIE_SUMMED_FREQ_TABLE[] = {
    // Column 0 (1209 Hz) - any of 1, 4, 7, * will produce this
    {2240.0, '1'},  // Observed: 2239.5Hz - represents column 0
    
    // Column 1 (1336 Hz) - any of 2, 5, 8, 0 will produce this  
    {2455.0, '2'},  // Observed: 2454.8Hz - represents column 1
    
    // Column 2 (1477 Hz) - any of 3, 6, 9, # will produce this
    {2713.0, '3'},  // Observed: 2713.2Hz - represents column 2
};

static const int BOWIE_SUMMED_FREQ_TABLE_SIZE = 
    sizeof(BOWIE_SUMMED_FREQ_TABLE) / sizeof(PhoneSummedFreqEntry);

// Bowie Phone configuration
static const PhoneConfig BOWIE_PHONE_CONFIG = {
    // Identification
    .name = "Bowie Phone",
    .description = "ESP32-A1S AudioKit with SLIC - column-only summed freq, weak fundamentals",
    
    // Frequency scaling
    .freqScale = 1.0f,  // Not used for column detection
    
    // Detection thresholds
    // From logs: good detections have col magnitudes 50-200, noise is 0.1-2.0
    .fundamentalMagnitudeThreshold = 15.0f,  // Require strong column signal to filter noise
    .summedMagnitudeThreshold = 100.0f,      // Strong signals (200-500 observed)
    .freqTolerance = 75.0f,                  // Hz tolerance for fundamental matching
    .summedFreqTolerance = 60.0f,            // Hz tolerance for summed frequency (3 distinct columns)
    
    // Detection timing
    .detectionCooldown = 300,                // 300ms between detections (prevents repeat on held button)
    .gapThreshold = 150,                     // 150ms silence = new button press (~2 FFT frames)
    .requiredConsecutive = 3,                // Require 3 consecutive matches for solid debounce
    
    // Detection mode
    .useSummedFreqDetection = true,          // Primary: use summed frequencies for COLUMN
    .useFundamentalDetection = true,         // Also try fundamentals when strong enough
    .summedTriggersRowCheck = true,          // When summed detected, MUST verify with row freq
    
    // Summed frequency table (maps to columns only)
    .summedFreqTable = BOWIE_SUMMED_FREQ_TABLE,
    .summedFreqTableSize = BOWIE_SUMMED_FREQ_TABLE_SIZE,
    
    // Row frequencies (standard DTMF - check these for row detection)
    .rowFreqs = {697.0f, 770.0f, 852.0f, 941.0f},
    
    // Column frequencies (standard DTMF)
    .colFreqs = {1209.0f, 1336.0f, 1477.0f, 1633.0f},
};

const PhoneConfig& getPhoneConfig() {
    return BOWIE_PHONE_CONFIG;
}

// Helper: Decode COLUMN from summed frequency (returns column index 0-3, or -1)
static int getColumnFromSummedFreq(float freq) {
    const PhoneConfig& config = BOWIE_PHONE_CONFIG;
    
    for (int i = 0; i < config.summedFreqTableSize; i++) {
        float diff = freq - config.summedFreqTable[i].freq;
        if (diff < 0) diff = -diff;
        if (diff <= config.summedFreqTolerance) {
            // Return column index based on which entry matched
            // Entry 0 = column 0, Entry 1 = column 1, Entry 2 = column 2
            return i;
        }
    }
    return -1; // No match
}

// Helper: Decode button from summed frequency
// This now returns a "representative" button that indicates the column
// The actual button must be determined by also checking the row frequency
char decodeFromSummedFreq(float freq) {
    const PhoneConfig& config = BOWIE_PHONE_CONFIG;
    
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
    const PhoneConfig& config = BOWIE_PHONE_CONFIG;
    
    int row = findClosestFreq(rowFreq, config.rowFreqs, 4, config.freqTolerance);
    int col = findClosestFreq(colFreq, config.colFreqs, 4, config.freqTolerance);
    
    if (row >= 0 && col >= 0) {
        return DTMF_KEYPAD[row][col];
    }
    return 0;
}

#endif // PHONE == BOWIE_PHONE
