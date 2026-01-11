#include "dtmf_decoder.h"
#include "phone.h"
#include "logging.h"
#include "config.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT
// Compile-time configurable gap threshold (ms)
// Gap = silence between button presses, used for state reset
#ifndef GAP_THRESHOLD_MS
#define GAP_THRESHOLD_MS 150
#endif

// Minimum samples before emitting key-down detection
#ifndef MIN_KEYDOWN_SAMPLES
#define MIN_KEYDOWN_SAMPLES 3
#endif

// Minimum average row magnitude to trust row detection
#ifndef MIN_AVG_ROW_MAG
#define MIN_AVG_ROW_MAG 2.0f
#endif

// Get phone configuration (from phone-specific implementation)
#define PHONE_CONFIG getPhoneConfig()

// Global variables for DTMF detection
FrequencyPeak detectedPeaks[10];
int peakCount = 0;
unsigned long lastDetectionTime = 0;

// Detection state
static char lastDetectedButton = 0;
static unsigned long lastButtonTime = 0;
static int consecutiveDetections = 0;
static char confirmedButton = 0;     // Button that passed consecutive detection threshold
static unsigned long lastSignalTime = 0;  // When we last saw a strong signal (for gap detection)
static bool inGap = true;            // True when we haven't seen signal recently

// Summed-frequency triggered row detection state
static int pendingColumn = -1;       // Column detected from summed frequency
static float pendingRowMagMax = 0;   // Max row magnitude seen during summed detection
static int pendingRow = -1;          // Best row match during summed detection
static unsigned long pendingStartTime = 0;  // When we started looking for row

// Row frequency accumulation state (for averaging across button press duration)
static float rowAccumulators[4] = {0, 0, 0, 0};  // Accumulated magnitude at each row freq
static int rowSampleCount = 0;                    // Number of samples accumulated
static int accumulatedColumn = -1;                // Column from summed freq during accumulation
static bool buttonPressActive = false;            // True while we're accumulating for a button
static bool keyDownEmitted = false;               // True if we already emitted for this press (key-down mode)

// Debug: track max magnitude seen for signal level monitoring
// Runtime toggle for FFT debug output (controlled via special command)
static bool fftDebugEnabled = false;
static float maxMagnitudeSeen = 0;
static unsigned long lastDebugPrintTime = 0;
static unsigned long fftCallCount = 0;
static float lastStrongestFreq = 0;
static float lastStrongestMag = 0;
static unsigned long fftStartTime = 0;  // For tracking FFT rate
static float maxRowMagSeen = 0;
static float maxColMagSeen = 0;
static float maxSummedMagSeen = 0;

// FFT debug mode getter/setter
bool isFFTDebugEnabled() { return fftDebugEnabled; }
void setFFTDebugEnabled(bool enabled) { fftDebugEnabled = enabled; }

// FFT frame buffer for deferred processing
// Callback captures data here, processFFTFrame() handles heavy logic
static FFTFrameData pendingFrame = {0, 0, 0, 0, 0, 0, 0, {0, 0, 0, 0}, false, false};

// Get column index from button character
int getColumnFromButton(char button) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            if (DTMF_KEYPAD[row][col] == button) {
                return col;
            }
        }
    }
    return -1;
}

// Get row index from button character  
int getRowFromButton(char button) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            if (DTMF_KEYPAD[row][col] == button) {
                return row;
            }
        }
    }
    return -1;
}

// Find closest DTMF frequency (uses phone config)
int findClosestDTMFFreq(float freq, const float *freqArray, int arraySize)
{
  const PhoneConfig& config = PHONE_CONFIG;
  for (int i = 0; i < arraySize; i++)
  {
    float diff = freq - freqArray[i];
    if (diff < 0) diff = -diff;
    if (diff <= config.freqTolerance)
    {
      return i;
    }
  }
  return -1;
}

// Decode DTMF from detected frequencies (uses phone config)
char decodeDTMF(float rowFreq, float colFreq)
{
  const PhoneConfig& config = PHONE_CONFIG;
  int row = findClosestFreq(rowFreq, config.rowFreqs, 4, config.freqTolerance);
  int col = findClosestFreq(colFreq, config.colFreqs, 4, config.freqTolerance);

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
    Logger.debugf("analyzeDTMF: returning confirmed button '%c'\n", result);
    return result;
  }

  // Only show debug for meaningful peak counts to reduce spam
  if (peakCount >= 2)
  {
    Logger.debugf("analyzeDTMF: called with %d peaks\n", peakCount);
  }

  if (peakCount < 2)
  {
    peakCount = 0; // Reset for next analysis
    return 0;      // No debug output for empty calls to reduce spam
  }

  const PhoneConfig& config = PHONE_CONFIG;

  // Debug: Print all detected peaks
  Logger.debugf("analyzeDTMF: Detected frequency peaks:\n");
  for (int i = 0; i < peakCount; i++)
  {
    Logger.debugf("  Peak %d: %.1fHz (mag: %.1f)\n",
                i, detectedPeaks[i].frequency, detectedPeaks[i].magnitude);
  }

  // Find the two strongest peaks
  float strongestRowFreq = 0, strongestColFreq = 0;
  float maxRowMagnitude = 0, maxColMagnitude = 0;

  for (int i = 0; i < peakCount; i++)
  {
    float freq = detectedPeaks[i].frequency;
    float mag = detectedPeaks[i].magnitude;

    // Check if it's a row frequency
    if (findClosestFreq(freq, config.rowFreqs, 4, config.freqTolerance) >= 0)
    {
      Logger.debugf("analyzeDTMF: row freq candidate %.1fHz (mag: %.1f)\n", freq, mag);
      if (mag > maxRowMagnitude)
      {
        maxRowMagnitude = mag;
        strongestRowFreq = freq;
        Logger.debugf("analyzeDTMF: new strongest row %.1fHz\n", freq);
      }
    }

    // Check if it's a column frequency
    if (findClosestFreq(freq, config.colFreqs, 4, config.freqTolerance) >= 0)
    {
      Logger.debugf("analyzeDTMF: col freq candidate %.1fHz (mag: %.1f)\n", freq, mag);
      if (mag > maxColMagnitude)
      {
        maxColMagnitude = mag;
        strongestColFreq = freq;
        Logger.debugf("analyzeDTMF: new strongest col %.1fHz\n", freq);
      }
    }
  }

  Logger.debugf("analyzeDTMF: final Row=%.1fHz Col=%.1fHz\n",
              strongestRowFreq, strongestColFreq);

  // Decode DTMF if we have both row and column frequencies
  if (strongestRowFreq > 0 && strongestColFreq > 0)
  {
    char dtmfChar = decodeDTMF(strongestRowFreq, strongestColFreq);

    Logger.debugf("analyzeDTMF: decodeDTMF returned '%c'\n", dtmfChar ? dtmfChar : '0');

    if (dtmfChar != 0)
    {
      unsigned long currentTime = millis();
      if (currentTime - lastDetectionTime > config.detectionCooldown)
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
        Logger.debugf("analyzeDTMF: in cooldown period (%lums remaining)\n", 
               config.detectionCooldown - (currentTime - lastDetectionTime));
      }
    }
    else
    {
      Logger.debugf("analyzeDTMF: invalid DTMF char decoded\n");
    }
  }
  else
  {
    Logger.debugf("analyzeDTMF: missing row (%.1f) or col (%.1f) freq\n",
           strongestRowFreq, strongestColFreq);
  }

  // Reset for next analysis cycle
  peakCount = 0;
  return 0;
}

// FFT result callback - LIGHTWEIGHT version that only captures data
// Heavy processing is deferred to processFFTFrame() called from main loop
void fftResult(AudioFFTBase &fft)
{
  auto result = fft.result();
  
  // Get FFT parameters
  int num_bins = fft.size();
  float bin_width = fft.frequency(1);

  // Check for dial tone presence (350Hz and 440Hz)
  // If dial tone is strong, skip DTMF detection to avoid harmonic interference
  int dial_350_bin = (int)(350.0f / bin_width + 0.5f);
  int dial_440_bin = (int)(440.0f / bin_width + 0.5f);
  float dial_350_mag = (dial_350_bin >= 0 && dial_350_bin < num_bins) ? fft.magnitude(dial_350_bin) : 0;
  float dial_440_mag = (dial_440_bin >= 0 && dial_440_bin < num_bins) ? fft.magnitude(dial_440_bin) : 0;
  bool dialTonePresent = (dial_350_mag > 50.0f && dial_440_mag > 50.0f);

  // Calculate bin ranges for DTMF frequency bands
  int low_min_bin = (int)(650.0f / bin_width);
  int low_max_bin = (int)(1000.0f / bin_width);
  int high_min_bin = (int)(1150.0f / bin_width);
  int high_max_bin = (int)(1700.0f / bin_width);
  
  // Find strongest peak in LOW band (row frequencies)
  float row_mag = 0;
  int row_bin = 0;
  for (int bin = low_min_bin; bin <= low_max_bin && bin < num_bins; bin++)
  {
    float mag = fft.magnitude(bin);
    if (mag > row_mag)
    {
      row_mag = mag;
      row_bin = bin;
    }
  }
  
  // Find strongest peak in HIGH band (column frequencies)
  float col_mag = 0;
  int col_bin = 0;
  for (int bin = high_min_bin; bin <= high_max_bin && bin < num_bins; bin++)
  {
    float mag = fft.magnitude(bin);
    if (mag > col_mag)
    {
      col_mag = mag;
      col_bin = bin;
    }
  }
  
  // Quick parabolic interpolation for frequency accuracy
  float row_freq = fft.frequency(row_bin);
  float col_freq = fft.frequency(col_bin);
  if (row_bin > 0 && row_bin < num_bins - 1 && row_mag > 0)
  {
    float left = fft.magnitude(row_bin - 1);
    float right = fft.magnitude(row_bin + 1);
    float delta = 0.5f * (right - left) / (2.0f * row_mag - left - right);
    row_freq += delta * bin_width;
  }
  if (col_bin > 0 && col_bin < num_bins - 1 && col_mag > 0)
  {
    float left = fft.magnitude(col_bin - 1);
    float right = fft.magnitude(col_bin + 1);
    float delta = 0.5f * (right - left) / (2.0f * col_mag - left - right);
    col_freq += delta * bin_width;
  }
  
  // Capture magnitudes at exact DTMF row frequency bins
  const float rowTargets[4] = {697.0f, 770.0f, 852.0f, 941.0f};
  float rowMags[4];
  for (int r = 0; r < 4; r++)
  {
    int bin = (int)(rowTargets[r] / bin_width + 0.5f);
    rowMags[r] = (bin >= 0 && bin < num_bins) ? fft.magnitude(bin) : 0;
  }
  
  // Store captured data for deferred processing
  pendingFrame.timestamp = millis();
  pendingFrame.summedFreq = result.frequency;
  pendingFrame.summedMag = result.magnitude;
  pendingFrame.rowFreq = row_freq;
  pendingFrame.rowMag = row_mag;
  pendingFrame.colFreq = col_freq;
  pendingFrame.colMag = col_mag;
  for (int r = 0; r < 4; r++) pendingFrame.rowMags[r] = rowMags[r];
  pendingFrame.dialTonePresent = dialTonePresent;  // Flag dial tone to reject DTMF
  pendingFrame.valid = true;

  if (fftDebugEnabled) {
    fftCallCount++;
    if (fftStartTime == 0) fftStartTime = millis();
    if (result.magnitude > lastStrongestMag) { lastStrongestMag = result.magnitude; lastStrongestFreq = result.frequency; }
    if (result.magnitude > maxMagnitudeSeen) maxMagnitudeSeen = result.magnitude;
    if (result.magnitude > maxSummedMagSeen) maxSummedMagSeen = result.magnitude;
    if (row_mag > maxRowMagSeen) maxRowMagSeen = row_mag;
    if (col_mag > maxColMagSeen) maxColMagSeen = col_mag;
  }
}

// Process captured FFT frame - call from main loop (e.g., during analyzeDTMF)
// This performs all the heavy DTMF detection logic
void processFFTFrame()
{
  if (!pendingFrame.valid) return;
  
  // Copy frame data locally and mark as processed
  FFTFrameData frame = pendingFrame;
  pendingFrame.valid = false;
  
  unsigned long now = frame.timestamp;
  
  if (fftDebugEnabled) {
    // Print comprehensive debug info every 2000ms
    static unsigned long lastDebugPrintTime_proc = 0;
    if (now - lastDebugPrintTime_proc > 2000)
    {
      const PhoneConfig& cfg = PHONE_CONFIG;
      unsigned long elapsed = now - fftStartTime;
      float fftRate = (elapsed > 0) ? (fftCallCount * 1000.0f / elapsed) : 0;
      
      Logger.debugf("[%lu] 🎵 FFT: row=%.1fHz (%.2f), col=%.1fHz (%.2f)\n",
               now, frame.rowFreq, frame.rowMag, frame.colFreq, frame.colMag);
      Logger.debugf("[%lu] 📈 Stats: rate=%.1f/s, maxRow=%.2f, maxCol=%.2f, maxSum=%.1f, thresh=%.2f\n",
               now, fftRate, maxRowMagSeen, maxColMagSeen, maxSummedMagSeen, 
               cfg.fundamentalMagnitudeThreshold);
      Logger.debugf("[%lu] 🔧 State: gap=%s, pending=%d/%d, lastBtn=%c, consec=%d\n",
               now, inGap ? "Y" : "N", pendingColumn, pendingRow, 
               lastDetectedButton ? lastDetectedButton : '-', consecutiveDetections);
      Logger.debugf("[%lu] 🎹 RowMags: 697=%.2f, 770=%.2f, 852=%.2f, 941=%.2f\n",
               now, frame.rowMags[0], frame.rowMags[1], frame.rowMags[2], frame.rowMags[3]);
      
      lastDebugPrintTime_proc = now;
      fftStartTime = now;
      fftCallCount = 0;
      lastStrongestMag = 0;
      lastStrongestFreq = 0;
      maxRowMagSeen = 0;
      maxColMagSeen = 0;
      maxSummedMagSeen = 0;
    }
  }

  const PhoneConfig& config = PHONE_CONFIG;
  
  // Gap detection: if no signal for GAP_THRESHOLD_MS, reset state for next button
  if (now - lastSignalTime > GAP_THRESHOLD_MS && !inGap)
  {
    Logger.debugf("[%lu] 🔇 GAP DETECTED: %lums since last signal\n", now, now - lastSignalTime);
    
    if (buttonPressActive && !keyDownEmitted)
    {
      Logger.debugf("[%lu] ⚠️ GAP but keyDown not emitted (samples=%d)\n", now, rowSampleCount);
    }
    
    // Reset all state for next button
    Logger.debugf("[%lu] 🔄 Resetting state for next button\n", now);
    inGap = true;
    lastDetectedButton = 0;
    consecutiveDetections = 0;
    pendingColumn = -1;
    pendingRow = -1;
    pendingRowMagMax = 0;
    buttonPressActive = false;
    keyDownEmitted = false;
    accumulatedColumn = -1;
    rowSampleCount = 0;
    for (int r = 0; r < 4; r++) rowAccumulators[r] = 0;
  }

  // Skip DTMF detection if dial tone is present (harmonics cause false positives)
  // Dial tone (350+440 Hz) produces harmonics at 700Hz (near row 0: 697Hz) and 880Hz (near row 2: 852Hz)
  if (frame.dialTonePresent)
  {
    return;  // Don't process DTMF during dial tone
  }

  // Reject constant line noise frequencies
  // 642Hz is common analog line noise (possibly 60Hz mains hum harmonics)
  // 1088Hz is another noise frequency seen on this phone
  // These are NOT valid DTMF frequencies and should be ignored
  if ((frame.rowFreq >= 630.0f && frame.rowFreq <= 665.0f) ||
      (frame.colFreq >= 1070.0f && frame.colFreq <= 1110.0f))
  {
    // Line noise, not DTMF - skip processing
    return;
  }

  // Strategy 1: Standard DTMF detection (if fundamentals are strong enough)
  if (config.useFundamentalDetection && 
      frame.rowMag > config.fundamentalMagnitudeThreshold && 
      frame.colMag > config.fundamentalMagnitudeThreshold)
  {
    int row = findClosestFreq(frame.rowFreq, config.rowFreqs, 4, config.freqTolerance);
    int col = findClosestFreq(frame.colFreq, config.colFreqs, 4, config.freqTolerance);
    
    Logger.debugf("[%lu] 🎯 FUNDAMENTAL: row=%d (%.1fHz, mag=%.2f), col=%d (%.1fHz, mag=%.2f)\n",
           now, row, frame.rowFreq, frame.rowMag, col, frame.colFreq, frame.colMag);
    
    // Only count as valid signal if BOTH row and col match valid DTMF frequencies
    // This prevents line noise from triggering false "signal detected" events
    if (row >= 0 && col >= 0)
    {
      lastSignalTime = now;
      inGap = false;
      
      // Strong fundamentals - accumulate for key-down detection
      if (buttonPressActive && accumulatedColumn != col)
      {
        Logger.debugf("[%lu] ⚠️ FUND: column changed %d→%d, resetting\n", now, accumulatedColumn, col);
        for (int r = 0; r < 4; r++) rowAccumulators[r] = 0;
        rowSampleCount = 0;
        keyDownEmitted = false;
      }
      
      buttonPressActive = true;
      accumulatedColumn = col;
      
      for (int r = 0; r < 4; r++) rowAccumulators[r] += frame.rowMags[r];
      rowSampleCount++;
      
      // KEY-DOWN DETECTION
      if (!keyDownEmitted && rowSampleCount >= MIN_KEYDOWN_SAMPLES)
      {
        int bestRow = -1;
        float bestAccum = 0.0f;
        for (int r = 0; r < 4; r++)
        {
          if (rowAccumulators[r] > bestAccum) { bestAccum = rowAccumulators[r]; bestRow = r; }
        }
        
        float avgBestMag = bestAccum / rowSampleCount;
        if (bestRow >= 0 && avgBestMag >= MIN_AVG_ROW_MAG)
        {
          char finalButton = DTMF_KEYPAD[bestRow][accumulatedColumn];
          Logger.printf("[%lu] 🎹 KEY-DOWN: %c (col=%d, row=%d, samples=%d, avgMag=%.2f)\n",
                        now, finalButton, accumulatedColumn, bestRow, rowSampleCount, avgBestMag);
          confirmedButton = finalButton;
          keyDownEmitted = true;
        }
      }
      
      static unsigned long lastFundLog = 0;
      if ((now - lastFundLog) > 300)
      {
        lastFundLog = now;
        Logger.debugf("[%lu] 📊 FUND ACCUM: col=%d, samples=%d, 697=%.1f, 770=%.1f, 852=%.1f, 941=%.1f\n",
               now, col, rowSampleCount,
               rowAccumulators[0], rowAccumulators[1], rowAccumulators[2], rowAccumulators[3]);
      }
      return;  // Don't fall through to Strategy 2
    }
  }
  
  // Strategy 2: Summed frequency detection with row accumulation
  if (config.useSummedFreqDetection && frame.summedMag > config.summedMagnitudeThreshold)
  {
    char summedButton = decodeFromSummedFreq(frame.summedFreq);
    if (summedButton != 0)
    {
      lastSignalTime = now;
      inGap = false;
      
      int detectedCol = getColumnFromButton(summedButton);
      Logger.debugf("[%lu] 🔔 SUMMED: %.1fHz (mag=%.1f) → '%c' col=%d\n",
             now, frame.summedFreq, frame.summedMag, summedButton, detectedCol);
      
      if (config.summedTriggersRowCheck && detectedCol >= 0)
      {
        if (buttonPressActive && accumulatedColumn != detectedCol)
        {
          Logger.debugf("[%lu] ⚠️ Column changed %d→%d, resetting accumulators\n",
                 now, accumulatedColumn, detectedCol);
          for (int r = 0; r < 4; r++) rowAccumulators[r] = 0;
          rowSampleCount = 0;
          keyDownEmitted = false;
        }
        
        buttonPressActive = true;
        accumulatedColumn = detectedCol;
        
        for (int r = 0; r < 4; r++) rowAccumulators[r] += frame.rowMags[r];
        rowSampleCount++;
        
        // KEY-DOWN DETECTION
        if (!keyDownEmitted && rowSampleCount >= MIN_KEYDOWN_SAMPLES)
        {
          int bestRow = -1;
          float bestAccum = 0.0f;
          for (int r = 0; r < 4; r++)
          {
            if (rowAccumulators[r] > bestAccum) { bestAccum = rowAccumulators[r]; bestRow = r; }
          }
          
          float avgBestMag = bestAccum / rowSampleCount;
          if (bestRow >= 0 && avgBestMag >= MIN_AVG_ROW_MAG)
          {
            char finalButton = DTMF_KEYPAD[bestRow][accumulatedColumn];
            Logger.printf("[%lu] 🎹 KEY-DOWN: %c (col=%d, row=%d, samples=%d, avgMag=%.2f)\n",
                          now, finalButton, accumulatedColumn, bestRow, rowSampleCount, avgBestMag);
            confirmedButton = finalButton;
            keyDownEmitted = true;
          }
          else if (avgBestMag < MIN_AVG_ROW_MAG && rowSampleCount >= MIN_KEYDOWN_SAMPLES * 2)
          {
            char finalButton = DTMF_KEYPAD[0][accumulatedColumn];
            Logger.printf("[%lu] 🎹 KEY-DOWN (col-only): %c (col=%d, avgMag=%.2f too low)\n",
                          now, finalButton, accumulatedColumn, avgBestMag);
            confirmedButton = finalButton;
            keyDownEmitted = true;
          }
        }
        
        static unsigned long lastAccumLog = 0;
        if ((now - lastAccumLog) > 300)
        {
          lastAccumLog = now;
          Logger.debugf("[%lu] 📊 ACCUM: col=%d, samples=%d, 697=%.1f, 770=%.1f, 852=%.1f, 941=%.1f\n",
                 now, accumulatedColumn, rowSampleCount,
                 rowAccumulators[0], rowAccumulators[1], rowAccumulators[2], rowAccumulators[3]);
        }
      }
      else
      {
        // Use summed frequency directly
        if (!keyDownEmitted)
        {
          Logger.printf("[%lu] 🎹 KEY-DOWN (summed): %c\n", now, summedButton);
          confirmedButton = summedButton;
          keyDownEmitted = true;
        }
        buttonPressActive = true;
        accumulatedColumn = detectedCol >= 0 ? detectedCol : 0;
        rowSampleCount++;
      }
    }
  }
  if (fftDebugEnabled) {
    Logger.debugf("[%lu] 📊 Frame: sumFreq=%.1fHz (%.1f), rowMags=[%.2f,%.2f,%.2f,%.2f]\n",
             now, frame.summedFreq, frame.summedMag, frame.rowMags[0], frame.rowMags[1], frame.rowMags[2], frame.rowMags[3]);
  }
}
