#ifndef DTMF_GOERTZEL_H
#define DTMF_GOERTZEL_H

#include "AudioTools/CoreAudio/GoerzelStream.h"
#include "AudioTools/CoreAudio/StreamCopy.h"

// Initialize Goertzel-based DTMF decoder
// More efficient than FFT when only detecting specific frequencies
// Goertzel is O(n*k) vs FFT O(n log n), much faster for 8 DTMF frequencies
void initGoertzelDecoder(GoertzelStream &goertzel, StreamCopy &copier);

// Start Goertzel processing on a separate FreeRTOS task (core 0)
// This prevents blocking the main loop (audio runs on core 1)
void startGoertzelTask(StreamCopy &copier);

// Stop the Goertzel task
void stopGoertzelTask();

// Check if Goertzel task is running
bool isGoertzelTaskRunning();

// Get pending key from Goertzel decoder (returns 0 if none)
char getGoertzelKey();

// Reset Goertzel detection state (clear any stale partial detections)
void resetGoertzelState();

#endif // DTMF_GOERTZEL_H
