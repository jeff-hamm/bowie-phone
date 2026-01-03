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
    if (isDialTonePlaying())
    {
        Logger.debugln("🔇 Stopping dial tone - digit detected");
        stopAudio();
    }

    // Special case: '*' key completes the sequence (excluding the '*')
    if (digit == '*')
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
    if (sequenceIndex < MAX_SEQUENCE_LENGTH)
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
    if (sequenceIndex >= MAX_SEQUENCE_LENGTH - 1)
    {
        Logger.debugln("Sequence complete: buffer full");
        return true;
    }

    return false;
}

/**
 * @brief Check for new DTMF digits and manage sequence collection
 * @return true when a complete sequence is ready to process (matches a known pattern)
 */
static bool checkForDTMFSequence()
{
    static unsigned long lastAnalysisTime = 0;
    unsigned long currentTime = millis();

    // Analyze DTMF data every 50ms
    if (currentTime - lastAnalysisTime > 50)
    {
        char detectedChar = analyzeDTMF();
        if (detectedChar != 0)
        {
            Logger.debugf("DTMF digit detected: %c\n", detectedChar);
            lastAnalysisTime = currentTime;
            return addDigitToSequence(detectedChar);
        }
        lastAnalysisTime = currentTime;
    }

    return false; // No complete sequence yet
}

const char* readDTMFSequence()
{
    // Check for complete DTMF sequences from real audio or simulated input
    bool ready = checkForDTMFSequence() || sequenceReady;
    
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

    // Unknown sequence handling:
    // - Log for analysis
    // - Check for patterns
    // - Provide user feedback
    // - Suggest alternatives
}

// ============================================================================
// PHONE NUMBER PROCESSING FUNCTIONS
// ============================================================================