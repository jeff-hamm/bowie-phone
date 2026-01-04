/**
 * @file audio_file_manager.h
 * @brief Audio File Manager Header
 *
 * This library handles downloading, caching, and processing of audio files
 * from a remote server. Files are cached on SD card for offline use.
 * 
 * @author Bowie Phone Project
 * @date 2025
 */

#ifndef AUDIO_FILE_MANAGER_H
#define AUDIO_FILE_MANAGER_H

// ============================================================================
// INCLUDES
// ============================================================================
#include <Arduino.h>

// ============================================================================
// CONSTANTS AND CONFIGURATION
// ============================================================================

#ifndef AUDIO_JSON_FILE
#define AUDIO_JSON_FILE "/audio_files.json"
#endif
#ifndef CACHE_TIMESTAMP_FILE
#define CACHE_TIMESTAMP_FILE "/audio_cache_time.txt"
#endif
#ifndef CACHE_VALIDITY_HOURS
#define CACHE_VALIDITY_HOURS 24     ///< Cache validity in hours
#endif
#ifndef MAX_KNOWN_SEQUENCES
#define MAX_KNOWN_SEQUENCES 50      ///< Maximum number of known sequences
#endif
#ifndef MAX_HTTP_RESPONSE_SIZE
#define MAX_HTTP_RESPONSE_SIZE 8192 ///< Maximum HTTP response size
#endif
#ifndef AUDIO_FILES_DIR
#define AUDIO_FILES_DIR "/audio"    ///< Directory for cached audio files
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
 * @brief Structure representing an audio file entry
 */
struct AudioFile
{
    const char *audioKey;    ///< Audio key (e.g., "dialtone", "busy", or DTMF sequence like "123")
    const char *description; ///< Human-readable description
    const char *type;        ///< Entry type (e.g., "audio", "service", "shortcut", "url")
    const char *path;        ///< File path or URL
    const char *ext;         ///< File extension (e.g., "wav", "mp3") - from server metadata
};

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Initialize the audio file manager
 * 
 * Loads cached audio files from SD card if available.
 * Call this during setup() AFTER SD card is initialized (for SD_MMC mode).
 * 
 * @param sdCsPin CS pin for SPI SD mode (ignored if mmcSupport is true)
 * @param mmcSupport If true, use SD_MMC interface (already initialized). If false, use SPI SD.
 * @param sdAvailable If true, SD card is mounted and available. If false, runs in memory-only mode.
 */
void initializeAudioFileManager(int sdCsPin = 13, bool mmcSupport = true, bool sdAvailable = false);

/**
 * @brief Download audio file list from remote server
 * @return true if download successful, false otherwise
 * 
 * Makes HTTP GET request to configured URL to download audio file definitions.
 * Only downloads if cache is stale and WiFi is connected.
 * Automatically saves to SD card for caching.
 * 
 * Expected JSON format:
 * {
 *   "<audio_key>": {
 *     "description": "<description>",
 *     "type": "<type>",
 *     "path": "<path or URL>"
 *   }
 * }
 */
bool downloadAudio();

/**
 * @brief Check if an audio key exists in the loaded audio files
 * @param key Audio key to check (e.g., "dialtone", "123")
 * @return true if key exists, false otherwise
 */
bool hasAudioKey(const char *key);

/**
 * @brief Check if any audio key starts with the given prefix
 * @param prefix Prefix to check (e.g., "91" would match "911", "912", etc.)
 * @return true if any key starts with prefix, false otherwise
 */
bool hasAudioKeyWithPrefix(const char *prefix);

/**
 * @brief Process an audio key and get the local file path
 * @param key Audio key to process
 * @return Local file path for audio playback, or nullptr if not available
 * 
 * Looks up the key in the audio files list. For remote URLs,
 * returns the cached local path if available, or queues for download.
 */
const char* processAudioKey(const char *key);

/**
 * @brief List all audio keys to serial output
 * 
 * Useful for debugging and configuration verification.
 */
void listAudioKeys();

/**
 * @brief Get the number of loaded audio files
 * @return Number of audio files currently loaded
 */
int getAudioKeyCount();

/**
 * @brief Clear all audio files from memory and SD card cache
 * 
 * Frees allocated memory and clears cached data.
 */
void clearAudioKeys();

/**
 * @brief Invalidate cache and force re-download on next call
 * 
 * Use this before downloadAudio() to force a fresh download.
 */
void invalidateAudioCache();

// ============================================================================
// DOWNLOAD QUEUE MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * @brief Process next item in audio download queue
 * @return true if item was processed, false if queue empty or error
 * 
 * Call this function periodically in main loop to download audio files
 * in the background. Non-blocking operation with rate limiting.
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

#endif // AUDIO_FILE_MANAGER_H
