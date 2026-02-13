#ifndef DTMF_DECODER_H
#define DTMF_DECODER_H

// Forward declare the AudioFFTBase class so the fftResult stub compiles
// without pulling in the full FFT header.
namespace audio_tools { class AudioFFTBase; }
using audio_tools::AudioFFTBase;

// ── Stub API (FFT pipeline removed — Goertzel handles detection) ──

char analyzeDTMF();          // Always returns 0
void processFFTFrame();      // No-op
void fftResult(AudioFFTBase &fft);  // No-op

// FFT debug toggle (still wired to the "fft" serial command)
bool isFFTDebugEnabled();
void setFFTDebugEnabled(bool enabled);

#endif // DTMF_DECODER_H