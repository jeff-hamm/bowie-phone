#include "dtmf_decoder.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT

// Uncomment to enable debug output
// #define DEBUG

// DTMF frequency definitions
const float DTMF_ROW_FREQS[] = {697.0, 770.0, 852.0, 941.0};
const float DTMF_COL_FREQS[] = {1209.0, 1336.0, 1477.0, 1633.0};
const char DTMF_KEYPAD[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};

// Detection parameters
const float MAGNITUDE_THRESHOLD = 300.0;      // Minimum magnitude for detection (lowered for sensitivity)
const float FREQ_TOLERANCE = 25.0;            // Frequency tolerance in Hz (slightly increased)
const unsigned long DETECTION_COOLDOWN = 200; // 200ms between detections (reduced for responsiveness)

// Global variables for DTMF detection
FrequencyPeak detectedPeaks[10];
int peakCount = 0;
unsigned long lastDetectionTime = 0;

// Find closest DTMF frequency
int findClosestDTMFFreq(float freq, const float *freqArray, int arraySize)
{
  for (int i = 0; i < arraySize; i++)
  {
    if (abs(freq - freqArray[i]) <= FREQ_TOLERANCE)
    {
      return i;
    }
  }
  return -1;
}

// Decode DTMF from detected frequencies
char decodeDTMF(float rowFreq, float colFreq)
{
  int row = findClosestDTMFFreq(rowFreq, DTMF_ROW_FREQS, 4);
  int col = findClosestDTMFFreq(colFreq, DTMF_COL_FREQS, 4);

  if (row >= 0 && col >= 0)
  {
    return DTMF_KEYPAD[row][col];
  }
  return 0; // No valid DTMF detected
}

// Analyze collected peaks and decode DTMF
char analyzeDTMF()
{
#ifdef DEBUG
  // Only show debug for meaningful peak counts to reduce spam
  if (peakCount >= 2)
  {
    Serial.printf("DEBUG: analyzeDTMF() called with %d peaks\n", peakCount);
  }
#endif

  if (peakCount < 2)
  {
    peakCount = 0; // Reset for next analysis
    return 0;      // No debug output for empty calls to reduce spam
  }

#ifdef DEBUG
  // Debug: Print all detected peaks
  Serial.printf("DEBUG: Detected frequency peaks:\n");
  for (int i = 0; i < peakCount; i++)
  {
    Serial.printf("  Peak %d: %.1fHz (mag: %.1f)\n",
                  i, detectedPeaks[i].frequency, detectedPeaks[i].magnitude);
  }
#endif

  // Find the two strongest peaks
  float strongestRowFreq = 0, strongestColFreq = 0;
  float maxRowMagnitude = 0, maxColMagnitude = 0;

  for (int i = 0; i < peakCount; i++)
  {
    float freq = detectedPeaks[i].frequency;
    float mag = detectedPeaks[i].magnitude;

    // Check if it's a row frequency
    if (findClosestDTMFFreq(freq, DTMF_ROW_FREQS, 4) >= 0)
    {
#ifdef DEBUG
      Serial.printf("DEBUG: Found row frequency candidate: %.1fHz (mag: %.1f)\n", freq, mag);
#endif
      if (mag > maxRowMagnitude)
      {
        maxRowMagnitude = mag;
        strongestRowFreq = freq;
#ifdef DEBUG
        Serial.printf("DEBUG: New strongest row frequency: %.1fHz\n", freq);
#endif
      }
    }

    // Check if it's a column frequency
    if (findClosestDTMFFreq(freq, DTMF_COL_FREQS, 4) >= 0)
    {
#ifdef DEBUG
      Serial.printf("DEBUG: Found column frequency candidate: %.1fHz (mag: %.1f)\n", freq, mag);
#endif
      if (mag > maxColMagnitude)
      {
        maxColMagnitude = mag;
        strongestColFreq = freq;
#ifdef DEBUG
        Serial.printf("DEBUG: New strongest column frequency: %.1fHz\n", freq);
#endif
      }
    }
  }

#ifdef DEBUG
  Serial.printf("DEBUG: Final strongest frequencies - Row: %.1fHz, Column: %.1fHz\n",
                strongestRowFreq, strongestColFreq);
#endif

  // Decode DTMF if we have both row and column frequencies
  if (strongestRowFreq > 0 && strongestColFreq > 0)
  {
    char dtmfChar = decodeDTMF(strongestRowFreq, strongestColFreq);

#ifdef DEBUG
    Serial.printf("DEBUG: decodeDTMF returned: %c\n", dtmfChar ? dtmfChar : '0');
#endif

    if (dtmfChar != 0)
    {
      unsigned long currentTime = millis();
      if (currentTime - lastDetectionTime > DETECTION_COOLDOWN)
      {
        Serial.printf("DTMF Detected: %c (Row: %.1fHz, Col: %.1fHz)\n",
                      dtmfChar, strongestRowFreq, strongestColFreq);
        lastDetectionTime = currentTime;

        // Reset for next analysis cycle
        peakCount = 0;
        return dtmfChar;
      }
      else
      {
#ifdef DEBUG
        Serial.printf("DEBUG: DTMF detection in cooldown period\n");
#endif
      }
    }
    else
    {
#ifdef DEBUG
      Serial.printf("DEBUG: Invalid DTMF character decoded\n");
#endif
    }
  }
  else
  {
#ifdef DEBUG
    Serial.printf("DEBUG: Missing row or column frequency for DTMF\n");
#endif
  }

  // Reset for next analysis cycle
  peakCount = 0;
  return 0;
}

// FFT result callback - collect frequency peaks and analyze periodically
void fftResult(AudioFFTBase &fft)
{
  auto result = fft.result();
  static unsigned long lastAnalysisTime = 0;

  // Only consider frequencies with sufficient magnitude
  if (result.magnitude > MAGNITUDE_THRESHOLD)
  {
    // Check if this is potentially a DTMF frequency
    bool isDTMFCandidate = false;

    // Check against row frequencies
    for (int i = 0; i < 4; i++)
    {
      if (abs(result.frequency - DTMF_ROW_FREQS[i]) <= FREQ_TOLERANCE)
      {
        isDTMFCandidate = true;
        break;
      }
    }

    // Check against column frequencies if not already a candidate
    if (!isDTMFCandidate)
    {
      for (int i = 0; i < 4; i++)
      {
        if (abs(result.frequency - DTMF_COL_FREQS[i]) <= FREQ_TOLERANCE)
        {
          isDTMFCandidate = true;
          break;
        }
      }
    }

    // Store DTMF candidate frequencies
    if (isDTMFCandidate && peakCount < 10)
    {
      detectedPeaks[peakCount].frequency = result.frequency;
      detectedPeaks[peakCount].magnitude = result.magnitude;
      detectedPeaks[peakCount].detected = true;
      peakCount++;

#ifdef DEBUG
      Serial.printf("DEBUG: Added DTMF candidate: %.1fHz (mag: %.1f), total peaks: %d\n",
                    result.frequency, result.magnitude, peakCount);
#endif
    }
  }

  // FFT callback now only collects frequency data
  // Analysis will be done in the main loop
}