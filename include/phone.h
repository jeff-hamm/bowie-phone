#ifndef PHONE_H
#define PHONE_H

#include <Arduino.h>

// Phone type definitions for conditional compilation
// Use -DPHONE=BOWIE_PHONE or -DPHONE=DREAM_PHONE in build flags
#define BOWIE_PHONE 1
#define DREAM_PHONE 2

// Default to BOWIE_PHONE if not specified
#ifndef PHONE
#define PHONE BOWIE_PHONE
#endif

// Phone-specific configuration structure
// Each phone model may have different frequency characteristics due to
// variations in SLIC circuits, analog components, and signal processing

// Summed frequency entry for phones that output intermodulation products
struct PhoneSummedFreqEntry {
    float freq;       // Expected detected frequency (row + col with scaling)
    char button;      // Button character
};

// Phone configuration structure
struct PhoneConfig {
    // Phone identification
    const char* name;
    const char* description;
    
    // Frequency scaling factor (some phones output scaled frequencies)
    float freqScale;
    
    // Detection thresholds
    float fundamentalMagnitudeThreshold;  // Threshold for detecting row/col fundamentals
    float summedMagnitudeThreshold;       // Threshold for detecting summed frequencies
    float freqTolerance;                  // Hz tolerance for frequency matching
    float summedFreqTolerance;            // Hz tolerance for summed frequency matching
    
    // Detection timing
    unsigned long detectionCooldown;      // ms between detections (prevent double-detection)
    unsigned long gapThreshold;           // ms silence = new button press
    int requiredConsecutive;              // Required consecutive detections to confirm
    
    // Detection mode flags
    bool useSummedFreqDetection;          // Use summed frequency as primary/trigger
    bool useFundamentalDetection;         // Use weak fundamental frequencies
    bool summedTriggersRowCheck;          // When summed detected, check fundamentals for row
    
    // Summed frequency table
    const PhoneSummedFreqEntry* summedFreqTable;
    int summedFreqTableSize;
    
    // Row frequency scaling (if phone outputs scaled DTMF fundamentals)
    // Standard DTMF: 697, 770, 852, 941 Hz
    float rowFreqs[4];
    
    // Column frequency scaling
    // Standard DTMF: 1209, 1336, 1477, 1633 Hz  
    float colFreqs[4];
};

// Get the current phone configuration
// This is defined in the phone-specific implementation file
extern const PhoneConfig& getPhoneConfig();

// Standard DTMF keypad (same for all phones)
extern const char DTMF_KEYPAD[4][4];

// Helper functions
char decodeFromSummedFreq(float freq);
int findClosestFreq(float freq, const float* freqArray, int arraySize, float tolerance);
char decodeFromRowCol(float rowFreq, float colFreq);

#endif // PHONE_H
