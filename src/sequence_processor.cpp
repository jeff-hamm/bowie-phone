#include "sequence_processor.h"
#include "audio_file_manager.h"
#include "audio_key_registry.h"
#include "extended_audio_player.h"
#include "notifications.h"
#include "phone_service.h"
#include "config.h"
#include "logging.h"

// ============================================================================
// DTMF SEQUENCE STATE
// ============================================================================
extern ExtendedAudioPlayer& audioPlayer;  // Defined in main.ino
static char dtmfSequence[MAX_SEQUENCE_LENGTH + 1]; // +1 for null terminator
static int sequenceIndex = 0;
static unsigned long lastDigitTime = 0;
static bool sequenceReady = false;  // Flag set when sequence is ready to process
static bool sequenceLocked = false; // Lock input after a sequence plays until hang-up
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
    // Notify DTMF digit detected (pulses green LED)
    notify(NotificationType::DTMFDetected, digit);
    
    // Stop dial tone when first digit is pressed
    if (audioPlayer.isAudioKeyPlaying("dialtone"))
    {
        Logger.debugln("🔇 Stopping dial tone - digit detected");
        audioPlayer.stop();
    }

    // Special case: '*' or '#' key completes the sequence (excluded from the key string)
    if (digit == '*' || digit == '#')
    {
        if (sequenceIndex > 0)
        {
            Logger.printf("⭐ '%c' pressed - completing sequence '%s'\n", digit, dtmfSequence);
            return true; // Process sequence without adding the terminator
        }
        // If terminator is first character, ignore it
        return false;
    }

    // Add digit to sequence
    if (sequenceIndex < maxSequenceLength)
    {
        // Notify that we're reading/processing a sequence (red LED on)
        notify(NotificationType::ReadingSequence, true);
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
            if (getAudioKeyRegistry().hasKey(substring))
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
 * @param skipFFT Unused (kept for API compatibility; FFT pipeline removed)
 * @return true when a complete sequence is ready to process (matches a known pattern)
 * 
 * Note: All DTMF detection is now handled by the Goertzel task which
 * feeds digits via addDtmfDigit() / getGoertzelKey() from the main loop.
 * This function is retained for simulated-input paths only.
 */
static bool checkForDTMFSequence(bool skipFFT = false)
{
    (void)skipFFT;  // FFT pipeline removed — parameter kept for compat
    return false;   // Digits arrive via addDtmfDigit() now
}

bool readDTMFSequence(bool skipFFT)
{
    // Check for complete DTMF sequences from real audio or simulated input
    bool ready = checkForDTMFSequence(skipFFT) || sequenceReady;
    
    if (ready && sequenceIndex > 0)
    {
        sequenceReady = false;  // Clear the flag
        notify(NotificationType::ReadingSequence, false);  // Clear reading LED
        
        
        // Process the complete sequence - this handles playback internally
        bool audioStarted = processNumberSequence(dtmfSequence);
        // Reset sequence buffer for next sequence
        sequenceIndex = 0;
        dtmfSequence[0] = '\0';

        // Lock input until hang-up if audio started
        if (audioStarted) {
            sequenceLocked = true;
        }
        
        return audioStarted;
    }
    
    return false; // No complete sequence yet
}

void resetDTMFSequence()
{
    sequenceIndex = 0;
    dtmfSequence[0] = '\0';
    sequenceReady = false;
    sequenceLocked = false;
    notify(NotificationType::ReadingSequence, false);  // Clear reading LED
    Logger.debugln("🔄 DTMF sequence reset");
}

bool isSequenceLocked() {
    return sequenceLocked;
}

void addDtmfDigit(char digit)
{
    Logger.debugf("🔧 [DEBUG] Simulating DTMF digit: %c\n", digit);
    
    // Validate digit
    if (!((digit >= '0' && digit <= '9') || digit == '*' || digit == '#'))
    {
        Logger.printf("⚠️ Invalid DTMF digit: %c\n", digit);
        return;
    }
    // Ignore digits when phone is on-hook
    if (!Phone.isOffHook())
    {
        Logger.debugf("[DEBUG] Ignoring digit '%c' - phone is on hook\n", digit);
        return;
    }
    // If a sequence is already waiting to be processed, ignore new digits
    if (sequenceReady)
    {
        Logger.debugf("🔧 [DEBUG] Ignoring digit '%c' - sequence already ready\n", digit);
        return;
    }

    // If input is locked (audio playing from a matched sequence), ignore
    if (sequenceLocked)
    {
        return;
    }
    
    // Use the same logic as real DTMF detection
    if (addDigitToSequence(digit)) {
        sequenceReady = true;  // Mark sequence as ready for processing
        Logger.debugln("🔧 [DEBUG] Sequence ready for processing");
    }
}

const char* getSequence()
{
    return dtmfSequence;
}

bool isReadingSequence()
{
    return sequenceIndex > 0;
}

bool isSequenceReady()
{
    return sequenceIndex > 0 && sequenceReady;
}

unsigned long getLastDigitTime()
{
    return lastDigitTime;
}

// ============================================================================
// MAIN PROCESSING FUNCTIONS
// ============================================================================

bool processNumberSequence(const char *sequence)
{
    Logger.printf("=== Processing DTMF Sequence: '%s' (length: %d) ===\n",
                  sequence, strlen(sequence));

    bool audioStarted = false;
    
    if (isSpecialCommand(sequence))
    {
        processSpecialCommand(sequence);
    }
    else if (getAudioKeyRegistry().hasKey(sequence))
    {
#if ENABLE_PLAYLIST_FEATURES
        // Play the playlist for this audio key (includes ringback, audio, click)
        audioStarted = audioPlayer.playPlaylist(sequence);
        if (!audioStarted) {
            // Fallback to direct audio key playback if no playlist
            audioStarted = audioPlayer.playAudioKey(sequence);
        }
#else
        audioStarted = audioPlayer.playAudioKey(sequence);
#endif
    }
    else {
        processUnknownSequence(sequence);
        audioStarted = true;  // wrong_number audio locks input until hangup
    }

    Logger.debugln("=== Sequence Processing Complete ===");
    return audioStarted;
}

void processUnknownSequence(const char *sequence)
{
    Logger.printf("❓ UNKNOWN SEQUENCE: %s\n", sequence);
    Logger.debugln("💡 This sequence doesn't match any known patterns");
    audioPlayer.playAudioKey("wrong_number");
    // Unknown sequence handling:
    // - Log for analysis
    // - Check for patterns
    // - Provide user feedback
    // - Suggest alternatives
}

// ============================================================================
// PHONE NUMBER PROCESSING FUNCTIONS
// ============================================================================