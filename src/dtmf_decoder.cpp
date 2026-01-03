#include "dtmf_decoder.h"
#include "logging.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT

// Uncomment to enable debug output
// #define DEBUG

// Uncomment to see ALL FFT values (not just DTMF candidates)
#define DEBUG_FFT_ALL

// Standard DTMF frequency definitions (for reference)
const float DTMF_ROW_FREQS[] = {697.0, 770.0, 852.0, 941.0};
const float DTMF_COL_FREQS[] = {1209.0, 1336.0, 1477.0, 1633.0};
const char DTMF_KEYPAD[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};

// OBSERVED summed frequencies from actual phone (with ~1.17x scaling factor)
// These are empirically measured - the phone seems to output at different frequencies
// Standard sum would be: 1=1906, 2=2033, 3=2174, 4=1979, etc.
// Observed sum is ~1.17x higher
const float OBSERVED_FREQ_SCALE = 1.17;  // Scaling factor observed

// Summed frequencies lookup (detected_freq -> button)
// Format: {min_freq, max_freq, button_char}
struct SummedFreqEntry {
    float freq;       // Expected detected frequency
    char button;      // Button character
};

// Based on observations: 1=2234, 2=2447, 3=2700 (with some tolerance)
// UPDATED: Button 9 appears at ~2707, need to differentiate from button 3
// Let's recalculate with better scaling estimates
const SummedFreqEntry SUMMED_FREQ_TABLE[] = {
    {2234.0, '1'},  // 697+1209=1906 * ~1.17 = 2230 (CONFIRMED)
    {2380.0, '2'},  // 697+1336=2033 * ~1.17 = 2379
    {2540.0, '3'},  // 697+1477=2174 * ~1.17 = 2544
    {2317.0, '4'},  // 770+1209=1979 * ~1.17 = 2315
    {2467.0, '5'},  // 770+1336=2106 * ~1.17 = 2464
    {2627.0, '6'},  // 770+1477=2247 * ~1.17 = 2629
    {2411.0, '7'},  // 852+1209=2061 * ~1.17 = 2411
    {2561.0, '8'},  // 852+1336=2188 * ~1.17 = 2560
    {2720.0, '9'},  // 852+1477=2329 * ~1.17 = 2725 (observed ~2707)
    {2516.0, '*'},  // 941+1209=2150 * ~1.17 = 2516
    {2666.0, '0'},  // 941+1336=2277 * ~1.17 = 2664
    {2830.0, '#'},  // 941+1477=2418 * ~1.17 = 2829
};
const int SUMMED_FREQ_TABLE_SIZE = sizeof(SUMMED_FREQ_TABLE) / sizeof(SummedFreqEntry);
const float SUMMED_FREQ_TOLERANCE = 40.0;  // Hz tolerance for matching (reduced to differentiate adjacent buttons)

// Detection parameters
const float MAGNITUDE_THRESHOLD = 20.0;       // Minimum magnitude for detection (lowered significantly)
const float FREQ_TOLERANCE = 25.0;            // Frequency tolerance in Hz (slightly increased)
const unsigned long DETECTION_COOLDOWN = 200; // 200ms between detections (reduced for responsiveness)

// Global variables for DTMF detection
FrequencyPeak detectedPeaks[10];
int peakCount = 0;
unsigned long lastDetectionTime = 0;

// Summed frequency detection state
static char lastDetectedButton = 0;
static unsigned long lastButtonTime = 0;
static int consecutiveDetections = 0;
const int REQUIRED_CONSECUTIVE = 1;  // Require 2 consecutive detections to confirm
static char confirmedButton = 0;     // Button that passed consecutive detection threshold

// Debug: track max magnitude seen for signal level monitoring
#ifdef DEBUG_FFT_ALL
static float maxMagnitudeSeen = 0;
static unsigned long lastDebugPrintTime = 0;
static unsigned long fftCallCount = 0;
static float lastStrongestFreq = 0;
static float lastStrongestMag = 0;
#endif

// Decode button from summed frequency (new method for this phone)
char decodeFromSummedFreq(float freq)
{
  for (int i = 0; i < SUMMED_FREQ_TABLE_SIZE; i++)
  {
    float diff = freq - SUMMED_FREQ_TABLE[i].freq;
    if (diff < 0) diff = -diff;  // Manual abs for float
    if (diff <= SUMMED_FREQ_TOLERANCE)
    {
      return SUMMED_FREQ_TABLE[i].button;
    }
  }
  return 0; // No match
}

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
  // First check if we have a confirmed button from summed frequency detection
  if (confirmedButton != 0)
  {
    char result = confirmedButton;
    confirmedButton = 0;  // Clear it so it's only returned once
    return result;
  }

#ifdef DEBUG
  // Only show debug for meaningful peak counts to reduce spam
  if (peakCount >= 2)
  {
    Logger.debugf("DEBUG: analyzeDTMF() called with %d peaks\n", peakCount);
  }
#endif

  if (peakCount < 2)
  {
    peakCount = 0; // Reset for next analysis
    return 0;      // No debug output for empty calls to reduce spam
  }

#ifdef DEBUG
  // Debug: Print all detected peaks
  Logger.debugf("DEBUG: Detected frequency peaks:\n");
  for (int i = 0; i < peakCount; i++)
  {
    Logger.debugf("  Peak %d: %.1fHz (mag: %.1f)\n",
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
      Logger.debugf("DEBUG: Found row frequency candidate: %.1fHz (mag: %.1f)\n", freq, mag);
#endif
      if (mag > maxRowMagnitude)
      {
        maxRowMagnitude = mag;
        strongestRowFreq = freq;
#ifdef DEBUG
        Logger.debugf("DEBUG: New strongest row frequency: %.1fHz\n", freq);
#endif
      }
    }

    // Check if it's a column frequency
    if (findClosestDTMFFreq(freq, DTMF_COL_FREQS, 4) >= 0)
    {
#ifdef DEBUG
      Logger.debugf("DEBUG: Found column frequency candidate: %.1fHz (mag: %.1f)\n", freq, mag);
#endif
      if (mag > maxColMagnitude)
      {
        maxColMagnitude = mag;
        strongestColFreq = freq;
#ifdef DEBUG
        Logger.debugf("DEBUG: New strongest column frequency: %.1fHz\n", freq);
#endif
      }
    }
  }

#ifdef DEBUG
  Logger.debugf("DEBUG: Final strongest frequencies - Row: %.1fHz, Column: %.1fHz\n",
                strongestRowFreq, strongestColFreq);
#endif

  // Decode DTMF if we have both row and column frequencies
  if (strongestRowFreq > 0 && strongestColFreq > 0)
  {
    char dtmfChar = decodeDTMF(strongestRowFreq, strongestColFreq);

#ifdef DEBUG
    Logger.debugf("DEBUG: decodeDTMF returned: %c\n", dtmfChar ? dtmfChar : '0');
#endif

    if (dtmfChar != 0)
    {
      unsigned long currentTime = millis();
      if (currentTime - lastDetectionTime > DETECTION_COOLDOWN)
      {
        Logger.printf("DTMF Detected: %c (Row: %.1fHz, Col: %.1fHz)\n",
                      dtmfChar, strongestRowFreq, strongestColFreq);
        lastDetectionTime = currentTime;

        // Reset for next analysis cycle
        peakCount = 0;
        return dtmfChar;
      }
      else
      {
#ifdef DEBUG
        Logger.debugf("DEBUG: DTMF detection in cooldown period\n");
#endif
      }
    }
    else
    {
#ifdef DEBUG
      Logger.debugf("DEBUG: Invalid DTMF character decoded\n");
#endif
    }
  }
  else
  {
#ifdef DEBUG
    Logger.debugf("DEBUG: Missing row or column frequency for DTMF\n");
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
  
  // Get FFT parameters for frequency calculation
  float sample_rate = 44100.0;  // Must match main.ino
  int fft_length = 8192;        // Must match main.ino
  float bin_width = sample_rate / fft_length;  // ~5.4 Hz per bin
  int num_bins = fft_length / 2;  // Nyquist limit

  // Find the top peaks in the DTMF frequency range (600-1700 Hz)
  int min_bin = (int)(600.0 / bin_width);
  int max_bin = (int)(1700.0 / bin_width);
  
  // Find the two strongest peaks
  float peak1_mag = 0, peak2_mag = 0;
  int peak1_bin = 0, peak2_bin = 0;
  
  for (int bin = min_bin; bin <= max_bin && bin < num_bins; bin++)
  {
    float mag = fft.magnitude(bin);
    if (mag > peak1_mag)
    {
      // Shift peak1 to peak2
      peak2_mag = peak1_mag;
      peak2_bin = peak1_bin;
      // New peak1
      peak1_mag = mag;
      peak1_bin = bin;
    }
    else if (mag > peak2_mag && abs(bin - peak1_bin) > 10)  // Must be at least ~54 Hz apart
    {
      peak2_mag = mag;
      peak2_bin = bin;
    }
  }
  
  float peak1_freq = peak1_bin * bin_width;
  float peak2_freq = peak2_bin * bin_width;

#ifdef DEBUG_FFT_ALL
  fftCallCount++;
  
  // Track the strongest signal we see
  if (result.magnitude > lastStrongestMag)
  {
    lastStrongestMag = result.magnitude;
    lastStrongestFreq = result.frequency;
  }
  if (result.magnitude > maxMagnitudeSeen)
  {
    maxMagnitudeSeen = result.magnitude;
  }
  
  // Print debug info every 2000ms
  unsigned long now = millis();
  if (now - lastDebugPrintTime > 2000)
  {
    Logger.debugf("🎵 FFT: peak1=%.1fHz (%.1f), peak2=%.1fHz (%.1f), threshold=%.1f\n",
                  peak1_freq, peak1_mag, peak2_freq, peak2_mag, MAGNITUDE_THRESHOLD);
    lastDebugPrintTime = now;
    fftCallCount = 0;
    lastStrongestMag = 0;
    lastStrongestFreq = 0;
  }
#endif

  // Check if we have two strong peaks (dual-tone DTMF)
  if (peak1_mag > MAGNITUDE_THRESHOLD && peak2_mag > MAGNITUDE_THRESHOLD * 0.5)
  {
    // Ensure peak1 is the lower frequency (row) and peak2 is higher (column)
    float rowFreq, colFreq;
    if (peak1_freq < peak2_freq)
    {
      rowFreq = peak1_freq;
      colFreq = peak2_freq;
    }
    else
    {
      rowFreq = peak2_freq;
      colFreq = peak1_freq;
    }
    
    // Try standard DTMF decoding first
    int row = findClosestDTMFFreq(rowFreq, DTMF_ROW_FREQS, 4);
    int col = findClosestDTMFFreq(colFreq, DTMF_COL_FREQS, 4);
    
    if (row >= 0 && col >= 0)
    {
      char dtmfChar = DTMF_KEYPAD[row][col];
      unsigned long now = millis();
      
      // Check consecutive detections
      if (dtmfChar == lastDetectedButton && (now - lastButtonTime) < 500)
      {
        consecutiveDetections++;
        Logger.printf("🔢 DTMF %c: detection #%d (row=%.1fHz, col=%.1fHz)\n", 
                      dtmfChar, consecutiveDetections, rowFreq, colFreq);
      }
      else if (dtmfChar != lastDetectedButton)
      {
        lastDetectedButton = dtmfChar;
        consecutiveDetections = 1;
        Logger.printf("🔢 DTMF %c: detection #1 (NEW) (row=%.1fHz, col=%.1fHz)\n", 
                      dtmfChar, rowFreq, colFreq);
      }
      lastButtonTime = now;
      
      if (consecutiveDetections >= REQUIRED_CONSECUTIVE && (now - lastDetectionTime) > DETECTION_COOLDOWN)
      {
        Logger.printf("🎹 DTMF DETECTED: %c (row=%.1fHz, col=%.1fHz)\n", 
                      dtmfChar, rowFreq, colFreq);
        lastDetectionTime = now;
        consecutiveDetections = 0;
        confirmedButton = dtmfChar;
      }
      return;  // Standard DTMF worked!
    }
  }
  
  // Fallback: Try summed frequency detection (for non-standard phones)
  if (result.magnitude > 500.0)  // Higher threshold for summed freq detection
  {
    char summedButton = decodeFromSummedFreq(result.frequency);
    if (summedButton != 0)
    {
      unsigned long now = millis();
      
      if (summedButton == lastDetectedButton && (now - lastButtonTime) < 500)
      {
        consecutiveDetections++;
        Logger.printf("🔢 SUM Button %c: detection #%d (freq=%.1fHz, mag=%.1f)\n", 
                      summedButton, consecutiveDetections, result.frequency, result.magnitude);
      }
      else if (summedButton != lastDetectedButton)
      {
        lastDetectedButton = summedButton;
        consecutiveDetections = 1;
        Logger.printf("🔢 SUM Button %c: detection #1 (NEW) (freq=%.1fHz, mag=%.1f)\n", 
                      summedButton, result.frequency, result.magnitude);
      }
      lastButtonTime = now;
      
      if (consecutiveDetections >= REQUIRED_CONSECUTIVE && (now - lastDetectionTime) > DETECTION_COOLDOWN)
      {
        Logger.printf("🎹 SUM DETECTED: %c (freq=%.1fHz, mag=%.1f)\n", 
                      summedButton, result.frequency, result.magnitude);
        lastDetectionTime = now;
        consecutiveDetections = 0;
        confirmedButton = summedButton;
      }
    }
  }

#ifdef DEBUG_FFT_ALL
  // Print any strong signal
  if (result.magnitude > MAGNITUDE_THRESHOLD)
  {
    Logger.debugf("📊 Signal: %.1fHz (mag=%.1f)\n", result.frequency, result.magnitude);
  }
#endif
}