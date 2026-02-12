/**
 * @file known_processor.h
 * @brief Known Sequence Processor Header
 * 
 * This library handles downloading, caching, and processing of known DTMF 
 * sequences from a remote server. Sequences are cached on SD card for offline use.
 * 
 * @author Bowie Phone Project
 * @date 2025
 */

#ifndef KNOWN_PROCESSOR_H
#define KNOWN_PROCESSOR_H

// ============================================================================
// INCLUDES
// ============================================================================
#include <Arduino.h>

// ============================================================================
// CONSTANTS AND CONFIGURATION
// ============================================================================

#ifndef KNOWN_SEQUENCES_FILE
#define KNOWN_SEQUENCES_FILE "/known_sequences.json"
#endif
#ifndef CACHE_TIMESTAMP_FILE
#define CACHE_TIMESTAMP_FILE "/known_cache_time.txt"
#endif
#ifndef CACHE_VALIDITY_HOURS
#define CACHE_VALIDITY_HOURS 24     ///< Cache validity in hours
#endif
#ifndef MAX_AUDIO_FILES
#define MAX_AUDIO_FILES 50      ///< Maximum number of known sequences
#endif
#ifndef MAX_HTTP_RESPONSE_SIZE
#define MAX_HTTP_RESPONSE_SIZE 8192 ///< Maximum HTTP response size
#endif
#ifndef AUDIO_FILES_DIR
#define AUDIO_FILES_DIR "/sdcard/audio"    ///< Directory for cached audio files
#endif
#ifndef MAX_DOWNLOAD_QUEUE
#define MAX_DOWNLOAD_QUEUE 20       ///< Maximum items in download queue
#endif
#ifndef MAX_FILENAME_LENGTH
#define MAX_FILENAME_LENGTH 64      ///< Maximum length for generated filenames
#endif
#ifndef KNOWN_SEQUENCES_URL
#define KNOWN_SEQUENCES_URL "https://raw.githubusercontent.com/jeff-hamm/bowie-phone/main/sample-sequence.json"
#endif
#ifndef USER_AGENT_HEADER
#define USER_AGENT_HEADER "BowiePhone/1.0"
#endif
#ifndef DOWNLOAD_QUEUE_CHECK_INTERVAL_MS
#define DOWNLOAD_QUEUE_CHECK_INTERVAL_MS 1000  ///< Interval between download queue processing (milliseconds)
#endif

// ============================================================================
// STRUCTURES
// ============================================================================

/**
 * @brief Structure representing a known DTMF sequence
 */
struct KnownSequence
{
    const char *sequence;    ///< DTMF sequence (e.g., "123", "*67#")
    const char *description; ///< Human-readable description
    const char *type;        ///< Sequence type (e.g., "phone", "service", "shortcut", "url")
    const char *path;        ///< Additional path/URL information
};

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Initialize the known sequence processor
 * 
 * Loads cached sequences from SD card if available.
 * Call this during setup().
 * 
 * @param useSDMMC If true, use SD_MMC interface (already initialized). If false, use SPI SD.
 */
void initializeKnownProcessor(bool useSDMMC = true);

/**
 * @brief Download known sequences from remote server
 * @return true if download successful, false otherwise
 * 
 * Makes HTTP GET request to configured URL to download sequence definitions.
 * Only downloads if cache is stale and WiFi is connected.
 * Automatically saves to SD card for caching.
 * 
 * Expected JSON format:
 * {
 *   "<DTMF code>": {
 *     "description": "<description>",
 *     "type": "<type>",
 *     "path": "<path>"
 *   }
 * }
 */
bool downloadKnownSequences();

/**
 * @brief Check if a sequence is in the known sequences list
 * @param sequence DTMF sequence to check
 * @return true if sequence is known, false otherwise
 */
bool isKnownSequence(const char *sequence);

/**
 * @brief Process a known DTMF sequence
 * @param sequence Known sequence to process
 * @return File path for audio playback, or nullptr if not an audio sequence
 * 
 * Looks up the sequence in the known sequences list and executes
 * appropriate action based on the sequence type. For audio sequences,
 * returns the local file path if available.
 */
const char* processKnownSequence(const char *sequence);

/**
 * @brief List all known sequences to serial output
 * 
 * Useful for debugging and configuration verification.
 */
void listKnownSequences();

/**
 * @brief Get the number of loaded known sequences
 * @return Number of sequences currently loaded
 */
int getKnownSequenceCount();

/**
 * @brief Clear all known sequences from memory and SD card
 * 
 * Frees allocated memory and clears cached data.
 */
void clearKnownSequences();

// ============================================================================
// DOWNLOAD QUEUE MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * @brief Process next item in audio download queue
 * @return true if item was processed, false if queue empty or error
 * 
 * Call this function periodically in main loop to download audio files
 * in the background. Non-blocking operation.
 */
bool processAudioDownloadQueue();

/**
 * @brief Get number of items remaining in download queue
 * @return Number of items not yet processed
 */
int getDownloadQueueCount();

/**
 * @brief Get total number of items ever added to download queue
 * @return Total queue size (including processed items)
 */
int getTotalDownloadQueueSize();

/**
 * @brief List all items in download queue to serial output
 * 
 * Shows pending, in-progress, and completed downloads.
 */
void listDownloadQueue();

/**
 * @brief Clear all items from download queue
 * 
 * Resets queue to empty state. Does not delete downloaded files.
 */
void clearDownloadQueue();

/**
 * @brief Check if download queue is empty
 * @return true if no items remain to process, false otherwise
 */
bool isDownloadQueueEmpty();

#endif // KNOWN_PROCESSOR_H