/**
 * @file tone_generators.h
 * @brief Audio tone generators for synthesizing dial tones, ringback, etc.
 * 
 * This file contains:
 * - DualToneGenerator: Generates two simultaneous sine waves
 * - RepeatingToneGenerator: Wraps a generator to add silence gaps
 * 
 * @date 2025
 */

#pragma once

#include <config.h>
#include "AudioTools/CoreAudio/AudioEffects/SoundGenerator.h"

using namespace audio_tools;

// ============================================================================
// DUAL TONE GENERATOR
// ============================================================================

/**
 * @brief Generator for dual-tone audio signals
 * 
 * Generates two simultaneous sine waves with independent frequencies and
 * combines them into a single output stream. Commonly used for creating
 * dial tones (350 Hz + 440 Hz North American standard).
 */
class DualToneGenerator : public SoundGenerator<int16_t> {
public:
    /**
     * @brief Construct a new DualToneGenerator
     * @param freq1 First frequency in Hz (default: 350 Hz)
     * @param freq2 Second frequency in Hz (default: 440 Hz)
     * @param amplitude Peak amplitude (default: 16000)
     */
    DualToneGenerator(float freq1 = 350.0f, float freq2 = 440.0f, float amplitude = 16000.0f) 
        : m_freq1(freq1), m_freq2(freq2), m_amplitude(amplitude) {
        m_sampleRate = AUDIO_SAMPLE_RATE;
        recalcPhaseIncrements();
    }
    
    /**
     * @brief Initialize the generator with audio format info
     * @param info Audio format configuration
     * @return true if initialization succeeded
     */
    bool begin(AudioInfo info) override {
        SoundGenerator<int16_t>::begin(info);
        m_sampleRate = info.sample_rate;
        recalcPhaseIncrements();
        m_phase1 = 0.0f;
        m_phase2 = 0.0f;
        return true;
    }
        
    /**
     * @brief Recalculate phase increments based on current sample rate
     */
    void recalcPhaseIncrements() {
        // Phase increment per sample (radians)
        m_phaseInc1 = 2.0f * PI * m_freq1 / (float)m_sampleRate;
        m_phaseInc2 = 2.0f * PI * m_freq2 / (float)m_sampleRate;
    }
    
    /**
     * @brief Generate next audio sample
     * @return 16-bit audio sample combining both tones
     */
    int16_t readSample() override {
        // Generate two sine waves and add them together
        float sample1 = sinf(m_phase1) * m_amplitude * 0.5f;
        float sample2 = sinf(m_phase2) * m_amplitude * 0.5f;
        
        // Advance phases
        m_phase1 += m_phaseInc1;
        m_phase2 += m_phaseInc2;
        
        // Wrap phases to avoid floating point overflow (wrap at 2*PI)
        if (m_phase1 >= 2.0f * PI) m_phase1 -= 2.0f * PI;
        if (m_phase2 >= 2.0f * PI) m_phase2 -= 2.0f * PI;
        
        return (int16_t)(sample1 + sample2);
    }
    
private:
    float m_freq1, m_freq2;      ///< Frequencies of the two tones
    float m_amplitude;            ///< Peak amplitude
    int m_sampleRate;             ///< Current sample rate
    float m_phaseInc1 = 0.0f;     ///< Phase increment for tone 1
    float m_phaseInc2 = 0.0f;     ///< Phase increment for tone 2
    float m_phase1 = 0.0f;        ///< Current phase for tone 1
    float m_phase2 = 0.0f;        ///< Current phase for tone 2
};

// ============================================================================
// REPEATING TONE GENERATOR
// ============================================================================

/**
 * @brief Generator for repeating tone patterns with silence gaps
 * 
 * Wraps another tone generator and adds silence periods to create
 * repeating cadences. Useful for ringback tones, busy signals, etc.
 */
template<typename T>
class RepeatingToneGenerator : public SoundGenerator<int16_t> {
public:
    /**
     * @brief Construct a new RepeatingToneGenerator
     * @param generator Reference to the tone generator to wrap
     * @param toneMs Duration of tone in milliseconds
     * @param silenceMs Duration of silence in milliseconds
     */
    RepeatingToneGenerator(SoundGenerator<T>& generator, unsigned long toneMs, unsigned long silenceMs)
        : m_generator(generator), m_toneDurationMs(toneMs), m_silenceDurationMs(silenceMs) {
        m_sampleRate = AUDIO_SAMPLE_RATE;
        recalcSampleCounts();
    }
    
    /**
     * @brief Initialize the generator with audio format info
     * @param info Audio format configuration
     * @return true if initialization succeeded
     */
    bool begin(AudioInfo info) override {
        SoundGenerator<int16_t>::begin(info);
        m_sampleRate = info.sample_rate;
        m_generator.begin(info);
        recalcSampleCounts();
        reset();
        return true;
    }
    
    /**
     * @brief Reset the pattern to beginning (start with tone)
     */
    void reset() {
        m_sampleCounter = 0;
        m_inTonePeriod = true;
    }
    
    /**
     * @brief Recalculate sample counts based on current sample rate
     */
    void recalcSampleCounts() {
        m_toneSamples = (m_toneDurationMs * m_sampleRate) / 1000;
        m_silenceSamples = (m_silenceDurationMs * m_sampleRate) / 1000;
    }
    
    /**
     * @brief Generate next audio sample
     * @return 16-bit audio sample (tone or silence)
     */
    int16_t readSample() override {
        int16_t sample = 0;
        
        if (m_inTonePeriod) {
            // Output tone from wrapped generator
            sample = m_generator.readSample();
            m_sampleCounter++;
            
            // Check if tone period is complete
            if (m_sampleCounter >= m_toneSamples) {
                m_inTonePeriod = false;
                m_sampleCounter = 0;
            }
        } else {
            // Output silence (zero)
            sample = 0;
            m_sampleCounter++;
            
            // Check if silence period is complete
            if (m_sampleCounter >= m_silenceSamples) {
                m_inTonePeriod = true;
                m_sampleCounter = 0;
            }
        }
        
        return sample;
    }
    
private:
    SoundGenerator<T>& m_generator;      ///< Wrapped tone generator
    unsigned long m_toneDurationMs;       ///< Tone duration in milliseconds
    unsigned long m_silenceDurationMs;    ///< Silence duration in milliseconds
    int m_sampleRate;                     ///< Current sample rate
    unsigned long m_toneSamples;          ///< Number of samples for tone period
    unsigned long m_silenceSamples;       ///< Number of samples for silence period
    unsigned long m_sampleCounter;        ///< Current sample position in period
    bool m_inTonePeriod;                  ///< True if in tone period, false if in silence
};
