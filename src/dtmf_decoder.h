#ifndef DTMF_DECODER_H
#define DTMF_DECODER_H

#include "AudioTools/AudioLibs/AudioRealFFT.h"

// Structure to hold frequency peak information
struct FrequencyPeak {
    float frequency;
    float magnitude;
    bool detected;
};

// DTMF frequency pairs
const float DTMF_FREQS_LOW[] = {697, 770, 852, 941};    // Low group frequencies
const float DTMF_FREQS_HIGH[] = {1209, 1336, 1477, 1633}; // High group frequencies

// DTMF character mapping
const char DTMF_CHARS[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// DTMF detection parameters
const float DTMF_MAGNITUDE_THRESHOLD = 100000.0; // Adjust based on testing
const float DTMF_RATIO_THRESHOLD = 5.0;          // Signal-to-noise ratio
const int DTMF_MIN_FRAMES = 3;                   // Minimum consecutive detections

// Function declarations
char analyzeDTMF();
char decodeDTMF(float rowFreq, float colFreq);
int findClosestDTMFFreq(float freq, const float *freqArray, int arraySize);
void fftResult(AudioFFTBase &fft);

#endif // DTMF_DECODER_H