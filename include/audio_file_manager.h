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

// Forward declarations
namespace audio_tools {
    class AudioSource;
}
using namespace audio_tools;

// ============================================================================
// CONSTANTS AND CONFIGURATION
// ============================================================================

#ifndef AUDIO_JSON_FILE
#define AUDIO_JSON_FILE "/audio_files.json"
#endif
#ifndef CACHE_TIMESTAMP_FILE
#define CACHE_TIMESTAMP_FILE "/audio_cache_time.txt"
#endif
#ifndef CACHE_ETAG_FILE
#define CACHE_ETAG_FILE "/audio_cache_etag.txt"
#endif
#ifndef CACHE_CHECK_INTERVAL_MS
#define CACHE_CHECK_INTERVAL_MS 300000  ///< Lightweight cache check interval (5 minutes)
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
    const char *data;        ///< File path or URL
    const char *ext;         ///< File extension (e.g., "wav", "mp3") - from server metadata
    unsigned long ringDuration; ///< Ring duration in ms (how long ringback plays before audio)
    unsigned long gap;          ///< Gap duration in ms between audio files in a playlist
    unsigned long duration;          ///< Gap duration in ms between audio files in a playlist
};

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Initialize the audio file manager
 *
 * Initializes SD card using pins from config.h (SD_CS_PIN, SD_CLK_PIN, etc.).
 * Loads cached audio files from SD card if available.
 *
 * @return AudioSource pointer if SD card available and initialized, nullptr otherwise
 */
AudioSource *initializeAudioFileManager();

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
 * 
 * @param maxRetries Maximum number of retry attempts (default: 3)
 * @param retryDelayMs Delay between retries in milliseconds (default: 2000)
 */
bool downloadAudio(int maxRetries = 3, unsigned long retryDelayMs = 2000);


/**
 * @brief Check if an audio key has a playlist (with ringback pattern)
 * @param key Audio key to look up
 * @return Non-zero if playlist exists for this key, 0 otherwise
 * @deprecated Ring duration is now managed by playlists. Check AudioPlaylistRegistry::hasPlaylist() directly.
 */
unsigned long getAudioKeyRingDuration(const char *key);

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

/**
 * @brief Pre-cache DNS resolutions before VPN tunnel starts
 * 
 * Resolves hostnames used by audio file manager and caches the IP addresses.
 * Call this BEFORE starting WireGuard/VPN, as the tunnel may break public DNS.
 * Subsequent HTTP requests will use cached IPs if DNS fails.
 */
void preCacheDNS();

/**
 * @brief Register a single audio file with the AudioKeyRegistry and create its playlist
 * 
 * This function is idempotent - calling it multiple times with the same AudioFile
 * will produce the same result. If the key is already registered, it will be
 * updated with the new values.
 * 
 * @param file Pointer to the AudioFile to register
 * @return true if registration successful, false if skipped or failed
 */
bool registerAudioFile(const AudioFile* file);

#endif // AUDIO_FILE_MANAGER_H
