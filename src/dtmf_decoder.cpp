// ==========================================================================
// dtmf_decoder.cpp  —  STUB  (FFT-based detection removed)
// ==========================================================================
// All DTMF detection is now handled by the Goertzel detector
// (dtmf_goertzel.cpp).  These stubs remain so that code which still
// references the old API (e.g. special_command_processor "fft" toggle)
// continues to compile and link.
// ==========================================================================

#include "dtmf_decoder.h"

// Runtime toggle kept for the "fft"/"fftdebug" serial command
static bool fftDebugEnabled = false;

bool  isFFTDebugEnabled()              { return fftDebugEnabled; }
void  setFFTDebugEnabled(bool enabled) { fftDebugEnabled = enabled; }

// No-op stubs — nothing to analyse without the FFT pipeline
char analyzeDTMF()      { return 0; }
void processFFTFrame()  { /* no-op */ }
void fftResult(AudioFFTBase & /*fft*/) { /* no-op */ }
