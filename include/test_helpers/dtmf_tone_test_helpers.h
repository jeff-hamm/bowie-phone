#pragma once

#ifdef TEST_MODE

#include <Arduino.h>
#include <math.h>

// Generate a dual-tone PCM block for synthetic DTMF tests.
inline void generateDualToneBlockForTest(int16_t* output,
                                         size_t sampleCount,
                                         float sampleRate,
                                         float lowFreq,
                                         float highFreq,
                                         float amplitude = 12000.0f,
                                         float phaseOffset = 0.0f) {
    if (!output || sampleCount == 0 || sampleRate <= 0.0f) {
        return;
    }

    const float twoPi = 6.28318530718f;
    for (size_t i = 0; i < sampleCount; i++) {
        const float t = (phaseOffset + static_cast<float>(i)) / sampleRate;
        const float sample = 0.5f * (sinf(twoPi * lowFreq * t) + sinf(twoPi * highFreq * t));
        output[i] = static_cast<int16_t>(amplitude * sample);
    }
}

inline void generateDualToneBlockWithGainsForTest(int16_t* output,
                                                  size_t sampleCount,
                                                  float sampleRate,
                                                  float lowFreq,
                                                  float highFreq,
                                                  float lowAmplitude,
                                                  float highAmplitude,
                                                  float phaseOffset = 0.0f) {
    if (!output || sampleCount == 0 || sampleRate <= 0.0f) {
        return;
    }

    const float twoPi = 6.28318530718f;
    for (size_t i = 0; i < sampleCount; i++) {
        const float t = (phaseOffset + static_cast<float>(i)) / sampleRate;
        float sample = (lowAmplitude * sinf(twoPi * lowFreq * t)) +
                       (highAmplitude * sinf(twoPi * highFreq * t));
        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;
        output[i] = static_cast<int16_t>(sample);
    }
}

inline void generateSingleToneBlockForTest(int16_t* output,
                                           size_t sampleCount,
                                           float sampleRate,
                                           float freq,
                                           float amplitude = 12000.0f,
                                           float phaseOffset = 0.0f) {
    if (!output || sampleCount == 0 || sampleRate <= 0.0f) {
        return;
    }

    const float twoPi = 6.28318530718f;
    for (size_t i = 0; i < sampleCount; i++) {
        const float t = (phaseOffset + static_cast<float>(i)) / sampleRate;
        output[i] = static_cast<int16_t>(amplitude * sinf(twoPi * freq * t));
    }
}

inline void generateSilenceBlockForTest(int16_t* output, size_t sampleCount) {
    if (!output) {
        return;
    }
    for (size_t i = 0; i < sampleCount; i++) {
        output[i] = 0;
    }
}

#endif // TEST_MODE
