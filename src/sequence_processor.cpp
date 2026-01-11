#include "sequence_processor.h"
#include "audio_file_manager.h"
#include "dtmf_decoder.h"
#include "audio_player.h"
#include "config.h"
#include "logging.h"

// ============================================================================
// DTMF SEQUENCE STATE
// ============================================================================

static char dtmfSequence[MAX_SEQUENCE_LENGTH + 1];  // +1 for null terminator
static int sequenceIndex = 0;
static unsigned long lastDigitTime = 0;
static bool sequenceReady = false;  // Flag set when sequence is ready to process
static int maxSequenceLength = MAX_SEQUENCE_LENGTH;  // Runtime configurable max length

// ============================================================================
// CONFIGURATION FUNCTIONS
// ============================================================================

void setMaxSequenceLength(int maxLength)
{
    if (maxLength <= 0 || maxLength > MAX_SEQUENCE_LENGTH)
    {
        // 0 or invalid values reset to full MAX_SEQUENCE_LENGTH
        maxSequenceLength = MAX_SEQUENCE_LENGTH;
        Logger.printf("📏 Max sequence length reset to %d\n", maxSequenceLength);
    }
    else
    {
        maxSequenceLength = maxLength;
        Logger.printf("📏 Max sequence length set to %d\n", maxSequenceLength);
    }
}

int getMaxSequenceLength()
{
    return maxSequenceLength;
}

// ============================================================================
// DTMF SEQUENCE READING
// ============================================================================

/**
 * @brief Add a DTMF digit to the sequence buffer (internal helper)
 * @param digit The digit to add
 * @return true if a complete sequence is ready
 */
static bool addDigitToSequence(char digit)
{
    // Stop dial tone when first digit is pressed
    if (isAudioKeyPlaying("dialtone"))
    {
        Logger.debugln("🔇 Stopping dial tone - digit detected");
        stopAudio();
    }

    // Special case: '*' key completes the sequence (excluding the '*')
    if (digit == '*' || digit == '#')
    {
        if (sequenceIndex > 0)
        {
            Logger.printf("⭐ '*' pressed - completing sequence '%s' (excluding '*')\n", dtmfSequence);
            return true; // Process sequence without adding '*'
        }
        // If '*' is first character, ignore it
        return false;
    }

    // Add digit to sequence
    if (sequenceIndex < maxSequenceLength)
    {
        dtmfSequence[sequenceIndex] = digit;
        sequenceIndex++;
        dtmfSequence[sequenceIndex] = '\0'; // Null terminate
        lastDigitTime = millis();
        Logger.printf("📞 Current sequence: '%s'\n", dtmfSequence);
        
        // Check all substrings of the current sequence for matches
        // e.g., "9911" should find "911" (check suffixes: "9911", "911", "11", "1")
        for (int start = 0; start < sequenceIndex; start++)
        {
            const char* substring = &dtmfSequence[start];
            if (hasAudioKey(substring))
            {
                Logger.debugf("✅ Found matching substring '%s' in sequence '%s'\n", substring, dtmfSequence);
                // Move the matched portion to the beginning for processing
                memmove(dtmfSequence, substring, strlen(substring) + 1);
                sequenceIndex = strlen(dtmfSequence);
                return true; // Process this match
            }
        }
    }

    // Only complete if buffer is full (no timeout - wait for hang up or match)
    if (sequenceIndex >= maxSequenceLength - 1)
    {
        Logger.debugln("Sequence complete: buffer full");
        return true;
    }

    return false;
}

/**
 * @brief Check for new DTMF digits and manage sequence collection
 * @param skipFFT If true, skip FFT processing (use when Goertzel is active)
 * @return true when a complete sequence is ready to process (matches a known pattern)
 * 
 * Note: The FFT callback (fftResult) fires when each FFT window completes
 * (~46ms at 2048 samples @ 44100Hz). That callback captures data which
 * processFFTFrame() analyzes. analyzeDTMF() returns any confirmed button.
 */
static bool checkForDTMFSequence(bool skipFFT = false)
{
    // Process any pending FFT frame (deferred from callback for efficiency)
    // Skip when using Goertzel (dial tone active) to avoid dial tone harmonic interference
    if (!skipFFT) {
        processFFTFrame();
    }
    
    char detectedChar = analyzeDTMF();
    if (detectedChar != 0)
    {
        Logger.debugf("DTMF digit detected: %c\n", detectedChar);
        return addDigitToSequence(detectedChar);
    }

    return false; // No complete sequence yet
}

const char* readDTMFSequence(bool skipFFT)
{
    // Check for complete DTMF sequences from real audio or simulated input
    bool ready = checkForDTMFSequence(skipFFT) || sequenceReady;
    
    if (ready && sequenceIndex > 0)
    {
        sequenceReady = false;  // Clear the flag
        
        // Process the complete sequence and check for audio playback
        const char* audioPath = processNumberSequence(dtmfSequence);
        
        // Reset sequence buffer for next sequence
        sequenceIndex = 0;
        dtmfSequence[0] = '\0';
        
        // Return audio path (may be nullptr, URL, or SD path)
        // Caller doesn't need to check SD.exists() - playAudioPath handles both modes
        return audioPath;
    }
    
    return nullptr; // No complete sequence yet
}

void resetDTMFSequence()
{
    sequenceIndex = 0;
    dtmfSequence[0] = '\0';
    Logger.debugln("🔄 DTMF sequence reset");
}

void simulateDTMFDigit(char digit)
{
    Logger.debugf("🔧 [DEBUG] Simulating DTMF digit: %c\n", digit);
    
    // Validate digit
    if (!((digit >= '0' && digit <= '9') || digit == '*' || digit == '#'))
    {
        Logger.printf("⚠️ Invalid DTMF digit: %c\n", digit);
        return;
    }
    
    // Use the same logic as real DTMF detection
    if (addDigitToSequence(digit)) {
        sequenceReady = true;  // Mark sequence as ready for processing
        Logger.debugln("🔧 [DEBUG] Sequence ready for processing");
    }
}

// ============================================================================
// MAIN PROCESSING FUNCTIONS
// ============================================================================

const char* processNumberSequence(const char *sequence)
{
    Logger.printf("=== Processing DTMF Sequence: '%s' (length: %d) ===\n",
                  sequence, strlen(sequence));

    // Check sequence type and route to appropriate handler
    const char* audioPath = nullptr;
    
    if (isSpecialCommand(sequence))
    {
        processSpecialCommand(sequence);
    }
    else if (hasAudioKey(sequence))
    {
        audioPath = processAudioKey(sequence);
    }
    else
    {
        processUnknownSequence(sequence);
    }

    Logger.debugln("=== Sequence Processing Complete ===");
    return audioPath;
}

void processUnknownSequence(const char *sequence)
{
    Logger.printf("❓ UNKNOWN SEQUENCE: %s\n", sequence);
    Logger.debugln("💡 This sequence doesn't match any known patterns");
    playAudioKey("wrong_number");
    // Unknown sequence handling:
    // - Log for analysis
    // - Check for patterns
    // - Provide user feedback
    // - Suggest alternatives
}

// ============================================================================
// PHONE NUMBER PROCESSING FUNCTIONS
// ============================================================================