/**
 * @file tone_generators.h
 * @brief Audio tone generators for synthesizing dial tones, ringback, etc.
 * 
 * This file contains:
 * - ToneGenerator<N>: Generates N simultaneous sine waves (N = 1–4, compile-time)
 * - DualToneGenerator: Convenience alias for ToneGenerator<2> with two-arg constructor
 * - MultiToneGenerator: Type alias for ToneGenerator<4>
 * - RepeatingToneGenerator: Wraps a generator to add silence gaps
 * 
 * @date 2025
 */

#pragma once

#include <array>
#include <config.h>
#include "AudioTools/CoreAudio/AudioEffects/SoundGenerator.h"

using namespace audio_tools;

// ============================================================================
// TONE GENERATOR (template — 1 to 4 simultaneous tones)
// ============================================================================

/**
 * @brief Generator for N simultaneous sine waves (N fixed at compile time)
 *
 * N must be 1–4. Because N is a template parameter the compiler can fully
 * unroll the inner loops, giving the same performance as hand-written code
 * while sharing a single implementation.
 *
 * Amplitude is automatically divided by N so the combined output never clips.
 *
 * Usage:
 *   ToneGenerator<3> gen(std::array<float, 3>{350.0f, 440.0f, 480.0f});
 */
template<int N>
class ToneGenerator : public SoundGenerator<int16_t> {
    static_assert(N >= 1 && N <= 4, "ToneGenerator: N must be 1–4");
public:
    /**
     * @brief Construct with a fixed-size array of frequencies
     * @param freqs  std::array of N frequencies in Hz
     * @param amplitude Peak amplitude (default: 16000)
     */
    ToneGenerator(std::array<float, N> freqs, float amplitude = 16000.0f)
        : m_amplitude(amplitude), m_sampleRate(AUDIO_SAMPLE_RATE) {
        for (int i = 0; i < N; i++) m_freq[i] = freqs[i];
        recalcPhaseIncrements();
    }

    bool begin(AudioInfo info) override {
        SoundGenerator<int16_t>::begin(info);
        m_sampleRate = info.sample_rate;
        recalcPhaseIncrements();
        for (int i = 0; i < N; i++) m_phase[i] = 0.0f;
        return true;
    }

    void recalcPhaseIncrements() {
        for (int i = 0; i < N; i++)
            m_phaseInc[i] = 2.0f * PI * m_freq[i] / (float)m_sampleRate;
    }

    int16_t readSample() override {
        float sum = 0.0f;
        for (int i = 0; i < N; i++) {
            sum += sinf(m_phase[i]);
            m_phase[i] += m_phaseInc[i];
            if (m_phase[i] >= 2.0f * PI) m_phase[i] -= 2.0f * PI;
        }
        return (int16_t)(sum * (m_amplitude / N));
    }

private:
    float m_freq[N];         ///< Tone frequencies
    float m_phaseInc[N] {};  ///< Phase increments per sample
    float m_phase[N] {};     ///< Current phase accumulators
    float m_amplitude;       ///< Peak amplitude
    int m_sampleRate;        ///< Current sample rate
};

// ============================================================================
// DUAL TONE GENERATOR
// ============================================================================

/**
 * @brief Convenience wrapper for ToneGenerator<2> with the classic two-arg constructor.
 *
 * Commonly used for dial tones (350 Hz + 440 Hz North American standard).
 */
class DualToneGenerator : public ToneGenerator<2> {
public:
    /**
     * @brief Construct a DualToneGenerator
     * @param freq1     First frequency in Hz (default: 350 Hz)
     * @param freq2     Second frequency in Hz (default: 440 Hz)
     * @param amplitude Peak amplitude (default: 16000)
     */
    DualToneGenerator(float freq1 = 350.0f, float freq2 = 440.0f, float amplitude = 16000.0f)
        : ToneGenerator<2>(std::array<float, 2>{freq1, freq2}, amplitude) {}
};

// ============================================================================
// MULTI TONE GENERATOR
// ============================================================================

/// @brief Four-tone generator. Alias for ToneGenerator<4>.
using MultiToneGenerator = ToneGenerator<4>;

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
