#pragma once

#ifdef TEST_MODE

#include <Arduino.h>
#include "dtmf_goertzel.h"

// Feed one PCM block into Goertzel and evaluate a detection cycle.
void processGoertzelSamplesForTest(GoertzelStream &goertzel, const int16_t* samples, size_t sampleCount);

#endif // TEST_MODE
