/**
 * @file audio_file_manager.cpp
 * @brief Audio File Manager Implementation
 * 
 * This file implements remote audio file downloading, caching, and processing
 * functionality for audio files retrieved from a remote server.
 * 
 * @author Bowie Phone Project
 * @date 2025
 */

#include "audio_file_manager.h"
#include "extended_audio_player.h"
#include "audio_key_registry.h"
#include "audio_playlist_registry.h"
#include "file_utils.h"
#include "logging.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SD_MMC.h>
#include <FS.h>
#include <SPI.h>
#include <set>
#if SD_USE_MMC
  #include "AudioTools/Disk/AudioSourceSDMMC.h"
#else
  #include "AudioTools/Disk/AudioSourceSD.h"
#endif

// Helper macros for SD vs SD_MMC abstraction
// Controlled by SD_USE_MMC in config.h (compile-time, not runtime)
#if SD_USE_MMC
  #define SD_CARD     ((fs::FS&)SD_MMC)
  #define SD_EXISTS(path)       SD_MMC.exists(path)
  #define SD_OPEN(path, mode)   SD_MMC.open(path, mode)
  #define SD_MKDIR(path)        SD_MMC.mkdir(path)
  #define SD_REMOVE(path)       SD_MMC.remove(path)
#else
  #define SD_CARD     ((fs::FS&)SD)
  #define SD_EXISTS(path)       SD.exists(path)
  #define SD_OPEN(path, mode)   SD.open(path, mode)
  #define SD_MKDIR(path)        SD.mkdir(path)
  #define SD_REMOVE(path)       SD.remove(path)
#endif

// ============================================================================
// STRUCTURES
// ============================================================================

/**
 * @brief Structure for audio download queue items
 */
struct AudioDownloadItem
{
    char url[256];          ///< Original URL to download
    char localPath[128];    ///< Local SD card path for the file
    char description[64];   ///< Description for logging
    char ext[8];            ///< File extension (e.g., "wav", "mp3")
    bool inProgress;        ///< Whether download is currently in progress
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

//static AudioFile audioFiles[MAX_AUDIO_FILES];
//static int audioFileCount = 0;
static unsigned long lastCacheTime = 0;
static unsigned long lastCacheCheck = 0;  // Last lightweight cache check time
static char cachedEtag[64] = {0};         // Cached ETag/lastModified for quick validation
static bool sdCardAvailable = false;  // True if SD card is mounted and accessible
static bool sdCardInitFailed = false; // True if SD init was attempted and failed (don't retry)
static bool spiInitialized = false;   // True after SPI.begin() has been called

// DNS cache for use after WireGuard breaks public DNS
static IPAddress cachedGitHubIP;
static bool dnsPreCached = false;

// Download queue management
static AudioDownloadItem downloadQueue[MAX_DOWNLOAD_QUEUE];
static int downloadQueueCount = 0;
static int downloadQueueIndex = 0; // Current processing index

// Registry references (initialized on first use)
static AudioKeyRegistry& keyRegistry = getAudioKeyRegistry();
static AudioPlaylistRegistry& playlistRegistry = getAudioPlaylistRegistry();

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Initialize SD card if not already done
 * 
 * Handles both first-time SPI setup and SD card mounting.
 * Uses pin definitions from config.h (SD_CS_PIN, SD_CLK_PIN, etc.)
 * Retries up to 3 times on first init, won't retry after permanent failure.
 * 
 * @return true if SD card is ready, false otherwise
 */
static bool initializeSDCard()
{
    if (sdCardAvailable)
    {
        return true;
    }
    
    // Don't retry if we already know it failed
    if (sdCardInitFailed)
    {
        return false;
    }
    
    Logger.println("🔧 Initializing SD card...");
    
#if SD_USE_MMC
    // Using SD_MMC mode (SDMMC 1-bit interface)
    // Initialize SD_MMC if not already done
    Logger.println("🔧 Initializing SD_MMC (1-bit mode)...");
    if (!SD_MMC.begin("/sdcard", true))  // true = 1-bit mode
    {
        Logger.println("❌ SD_MMC.begin() failed");
        sdCardInitFailed = true;
        return false;
    }
    
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE)
    {
        Logger.println("❌ No SD_MMC card detected");
        sdCardInitFailed = true;
        return false;
    }
    
    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    Logger.printf("✅ SD_MMC initialized (1-bit mode, %lluMB)\n", cardSize);
    sdCardAvailable = true;
    return true;
#else
    // Using SD mode (SPI interface)
    // Initialize SPI bus once with pins from config.h
    if (!spiInitialized)
    {
        SPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
        spiInitialized = true;
    }
    
    // Try up to 3 times with increasing delays
    for (int attempt = 1; attempt <= 3; attempt++)
    {
        Logger.printf("🔧 SD SPI initialization attempt %d/3...\n", attempt);
        delay(attempt * 300);
        
        if (SD.begin(SD_CS_PIN, SPI))
        {
            uint8_t cardType = SD.cardType();
            if (cardType != CARD_NONE)
            {
                uint64_t cardSize = SD.cardSize() / (1024 * 1024);
                Logger.printf("✅ SD card initialized (SPI mode, %lluMB)\n", cardSize);
                sdCardAvailable = true;
                return true;
            }
            else
            {
                Logger.println("❌ No SD card detected");
            }
        }
        else
        {
            Logger.println("❌ SD.begin() failed");
        }
    }
    
    Logger.println("❌ SD card initialization failed after 3 attempts");
    sdCardInitFailed = true;
    return false;
#endif
}

/**
 * @brief Check if audio file exists locally on SD card
 * @param url Original URL
 * @param ext File extension (e.g., "wav", "mp3") - can be NULL
 * @return true if file exists locally, false otherwise
 */
static bool audioFileExists(const char* url, const char* ext = nullptr)
{
    if (!initializeSDCard())
    {
        return false;
    }
    
    char localPath[128];
    if (!getLocalPathForUrl(url, localPath, ext))
    {
        return false;
    }
    
    return SD_EXISTS(localPath);
}

/**
 * @brief Ensure the AUDIO_FILES_DIR exists, creating intermediate dirs if needed
 */
static bool ensureAudioDirExists()
{
    if (SD_EXISTS(AUDIO_FILES_DIR))
    {
        return true;
    }

    // Build path incrementally to handle nested components like /sdcard/audio
    char path[128];
    size_t len = strlen(AUDIO_FILES_DIR);
    if (len == 0 || len >= sizeof(path))
    {
        Logger.println("❌ Invalid audio directory path");
        return false;
    }

    // Copy once for parsing
    strncpy(path, AUDIO_FILES_DIR, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    // Skip leading slash for tokenization
    char* token = strtok(path + 1, "/");
    char partial[128] = "/"; // Start at root
    while (token)
    {
        // Append next segment
        if (strlen(partial) + strlen(token) + 1 >= sizeof(partial))
        {
            Logger.println("❌ Audio directory path too long");
            return false;
        }
        strcat(partial, token);

        if (!SD_EXISTS(partial))
        {
            if (!SD_MKDIR(partial))
            {
                Logger.printf("❌ Failed to create directory: %s\n", partial);
                return false;
            }
        }

        // Add trailing slash for next component
        if (strlen(partial) + 1 < sizeof(partial))
        {
            strcat(partial, "/");
        }

        token = strtok(nullptr, "/");
    }

    return SD_EXISTS(AUDIO_FILES_DIR);
}

/**
 * @brief Add audio file to download queue
 * @param url URL to download
 * @param description Description for logging
 * @param ext File extension (e.g., "wav", "mp3") - can be NULL
 * @return true if added successfully, false otherwise
 */
static bool addToDownloadQueue(const char *url, const char *description, const char *ext = nullptr)
{
    if (downloadQueueCount >= MAX_DOWNLOAD_QUEUE)
    {
        Logger.println("⚠️ Download queue is full, cannot add more items");
        return false;
    }

    // Check if URL is already in queue
    for (int i = 0; i < downloadQueueCount; i++)
    {
        if (strcmp(downloadQueue[i].url, url) == 0)
        {
            Logger.printf("ℹ️ URL already in download queue: %s\n", url);
            return true; // Already queued, consider it success
        }
    }

    // Add new item to queue
    AudioDownloadItem *item = &downloadQueue[downloadQueueCount];
    strncpy(item->url, url, sizeof(item->url) - 1);
    item->url[sizeof(item->url) - 1] = '\0';

    // Store extension in the queue item
    if (ext && strlen(ext) > 0)
    {
        strncpy(item->ext, ext, sizeof(item->ext) - 1);
        item->ext[sizeof(item->ext) - 1] = '\0';
    }
    else
    {
        item->ext[0] = '\0';
    }

    if (!getLocalPathForUrl(url, item->localPath, ext))
    {
        Logger.printf("❌ Failed to generate local path for: %s\n", url);
        return false;
    }

    strncpy(item->description, description ? description : "Unknown", sizeof(item->description) - 1);
    item->description[sizeof(item->description) - 1] = '\0';

    item->inProgress = false;
    downloadQueueCount++;

    Logger.printf("📥 Added to download queue: %s -> %s\n", item->description, item->localPath);
    return true;
}

/**
 * @brief Queue downloads for any missing HTTP/HTTPS audio files from the registry
 */
static void enqueueMissingAudioFilesFromRegistry()
{
    if (keyRegistry.size() == 0)
    {
        return;
    }

    // If we cannot read the SD card, skip to avoid noisy logging
    if (!initializeSDCard())
    {
        Logger.println("⚠️ SD card not available, skipping download pre-queue");
        return;
    }

    int queued = 0;

    for (const auto& pair : keyRegistry)
    {
        const KeyEntry& entry = pair.second;
        
        // Only queue entries that have a streaming URL (means original was a URL)
        if (!entry.getUrl())
        {
            continue;
        }
        
        const char* downloadPath = entry.getUrl();
        const char* ext = entry.getExt();
        
        if (audioFileExists(downloadPath, ext))
        {
            continue; // Already cached
        }

        if (addToDownloadQueue(downloadPath, entry.audioKey.c_str(), ext))
        {
            queued++;
        }
    }

    if (queued > 0)
    {
        Logger.printf("📥 Queued %d missing audio file(s) for download\n", queued);
    }
}

/**
 * @brief Map a MIME type string to a file extension (without leading dot)
 * @param contentType Content-Type header value (may include parameters e.g. "audio/mpeg; charset=UTF-8")
 * @return Extension string (e.g. "mp3"), or nullptr if unrecognised
 */
static const char* mimeTypeToExtension(const char* contentType)
{
    if (!contentType || contentType[0] == '\0') return nullptr;
    if (strncmp(contentType, "audio/mpeg", 10) == 0) return "mp3";
    if (strncmp(contentType, "audio/mp3",  9) == 0) return "mp3";
    if (strncmp(contentType, "audio/wav",  9) == 0) return "wav";
    if (strncmp(contentType, "audio/wave", 10) == 0) return "wav";
    if (strncmp(contentType, "audio/x-wav", 11) == 0) return "wav";
    if (strncmp(contentType, "audio/vnd.wave", 14) == 0) return "wav";
    if (strncmp(contentType, "audio/m4a",  9) == 0) return "m4a";
    if (strncmp(contentType, "audio/mp4",  9) == 0) return "m4a";
    if (strncmp(contentType, "audio/aac",  9) == 0) return "aac";
    if (strncmp(contentType, "audio/ogg",  9) == 0) return "ogg";
    if (strncmp(contentType, "audio/flac", 10) == 0) return "flac";
    return nullptr;
}

/**
 * @brief Download next item in queue (non-blocking)
 * @return true if download started or completed, false if error or queue empty
 */
static bool processDownloadQueueInternal()
{
    if (downloadQueueIndex >= downloadQueueCount)
    {
        return false; // Queue empty or fully processed
    }
    
    if (WiFi.status() != WL_CONNECTED)
    {
        Logger.println("⚠️ WiFi not connected, skipping download queue processing");
        return false;
    }
    
    if (!initializeSDCard())
    {
        Logger.println("⚠️ SD card not available, skipping download queue processing");
        return false;
    }
    
    AudioDownloadItem* item = &downloadQueue[downloadQueueIndex];
    
    if (item->inProgress)
    {
        return false; // Already processing this item
    }
    
    Logger.printf("📥 Downloading audio file: %s\n", item->description);
    Logger.printf("    URL: %s\n", item->url);
    Logger.printf("    Local: %s\n", item->localPath);
    
    item->inProgress = true;
    
    // Ensure audio directory exists (handles nested paths like /sdcard/audio)
    if (!ensureAudioDirExists())
    {
        Logger.println("❌ Failed to ensure audio directory exists");
        item->inProgress = false;
        downloadQueueIndex++; // Skip this item
        return false;
    }
    
    // Download the file
    HTTPClient http;
    http.begin(item->url);
    http.addHeader("User-Agent", USER_AGENT_HEADER);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(30000);  // 30 second timeout for file downloads
    // Collect Content-Type so we can correct the file extension after the response
    const char* wantedHeaders[] = {"Content-Type"};
    http.collectHeaders(wantedHeaders, 1);
    
    int httpCode = http.GET();
    
    if (httpCode == 200)
    {
        // Detect actual audio format from Content-Type header and fix path/registry if needed
        String contentType = http.header("Content-Type");
        const char* detectedExt = mimeTypeToExtension(contentType.c_str());
        if (detectedExt && (item->ext[0] == '\0' || strcmp(item->ext, detectedExt) != 0))
        {
            Logger.printf("🔍 Content-Type '%s' → format '%s' (was '%s')\n",
                          contentType.c_str(), detectedExt,
                          item->ext[0] ? item->ext : "(default)");
            char newLocalPath[128];
            if (getLocalPathForUrl(item->url, newLocalPath, detectedExt))
            {
                strncpy(item->localPath, newLocalPath, sizeof(item->localPath) - 1);
                item->localPath[sizeof(item->localPath) - 1] = '\0';
                strncpy(item->ext, detectedExt, sizeof(item->ext) - 1);
                item->ext[sizeof(item->ext) - 1] = '\0';
                // Re-register with the correct extension so the player finds the right file
                keyRegistry.registerKey(item->description, item->url, detectedExt);
            }
        }

        // Get content length for progress tracking
        int contentLength = http.getSize();
        
        // Clean up any old files with different extensions for the same URL
        // This handles cases where a file was previously downloaded with .mp3 but should now be .wav
        char filenameBase[64];
        urlToBaseFilename(item->url, filenameBase, nullptr);  // Get base filename without extension
        // Remove the .mp3 that urlToBaseFilename adds when ext is null
        char* dot = strrchr(filenameBase, '.');
        if (dot) *dot = '\0';
        
        const char* extensions[] = {".mp3", ".wav", ".ogg", ".flac", ".aac", ".m4a"};
        for (int i = 0; i < 6; i++)
        {
            char oldPath[128];
            snprintf(oldPath, sizeof(oldPath), "%s/%s%s", AUDIO_FILES_DIR, filenameBase, extensions[i]);
            if (strcmp(oldPath, item->localPath) != 0 && SD_EXISTS(oldPath))
            {
                Logger.printf("🗑️ Removing old file with wrong extension: %s\n", oldPath);
                SD_REMOVE(oldPath);
            }
        }
        
        // Create file for writing
        File audioFile = SD_OPEN(item->localPath, FILE_WRITE);
        if (!audioFile)
        {
            Logger.printf("❌ Failed to create file: %s\n", item->localPath);
            http.end();
            item->inProgress = false;
            downloadQueueIndex++;
            return false;
        }
        
        // Download in chunks
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buffer[1024];
        int totalBytes = 0;
        
        while (http.connected() && (contentLength > 0 || contentLength == -1))
        {
            size_t availableBytes = stream->available();
            if (availableBytes > 0)
            {
                int bytesToRead = min(availableBytes, sizeof(buffer));
                int bytesRead = stream->readBytes(buffer, bytesToRead);
                
                if (bytesRead > 0)
                {
                    audioFile.write(buffer, bytesRead);
                    totalBytes += bytesRead;
                    
                    if (contentLength > 0)
                    {
                        contentLength -= bytesRead;
                    }
                }
            }
            else
            {
                delay(1); // Small delay to prevent busy waiting
            }
        }
        
        audioFile.close();
        Logger.printf("✅ Downloaded %d bytes to: %s\n", totalBytes, item->localPath);
    }
    else
    {
        Logger.printf("❌ HTTP download failed: %d for %s\n", httpCode, item->url);
    }
    
    http.end();
    item->inProgress = false;
    downloadQueueIndex++;
    
    return (httpCode == 200);
}

/**
 * @brief Load cached ETag from SD card
 * @return true if ETag was loaded, false otherwise
 */
static bool loadCachedEtag()
{
    if (!initializeSDCard())
    {
        return false;
    }
    
    File etagFile = SD_OPEN(CACHE_ETAG_FILE, FILE_READ);
    if (!etagFile)
    {
        cachedEtag[0] = '\0';
        return false;
    }
    
    String etagStr = etagFile.readString();
    etagFile.close();
    
    strncpy(cachedEtag, etagStr.c_str(), sizeof(cachedEtag) - 1);
    cachedEtag[sizeof(cachedEtag) - 1] = '\0';
    
    return strlen(cachedEtag) > 0;
}

/**
 * @brief Save ETag to SD card for future cache validation
 * @param etag The ETag or lastModified string to save
 * @return true if saved successfully
 */
static bool saveCachedEtag(const char* etag)
{
    if (!initializeSDCard() || !etag)
    {
        return false;
    }
    
    File etagFile = SD_OPEN(CACHE_ETAG_FILE, FILE_WRITE);
    if (!etagFile)
    {
        Logger.println("⚠️ Failed to save ETag file");
        return false;
    }
    
    etagFile.print(etag);
    etagFile.close();
    
    strncpy(cachedEtag, etag, sizeof(cachedEtag) - 1);
    cachedEtag[sizeof(cachedEtag) - 1] = '\0';
    
    return true;
}

/**
 * @brief Perform lightweight cache validation check via HTTP HEAD or lastModified endpoint
 * @return true if remote data has changed (cache is stale), false if unchanged
 */
static bool checkRemoteCacheValid()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return false; // Can't check, assume cache is valid
    }
    
    // Load cached ETag if not already loaded
    if (cachedEtag[0] == '\0')
    {
        loadCachedEtag();
    }
    
    // If no cached ETag, we need a full refresh
    if (cachedEtag[0] == '\0')
    {
        Logger.println("ℹ️ No cached ETag - full refresh needed");
        return true;
    }
    
    // Build the check URL - add lastModified query parameter
    String checkUrl = KNOWN_SEQUENCES_URL;
    if (checkUrl.indexOf('?') >= 0)
    {
        checkUrl += "&action=getLastModified";
    }
    else
    {
        checkUrl += "?action=getLastModified";
    }
    
    HTTPClient http;
    http.begin(checkUrl);
    http.addHeader("User-Agent", USER_AGENT_HEADER);
    http.setTimeout(5000);  // Short timeout for lightweight check
    
    int httpCode = http.GET();
    
    if (httpCode != 200)
    {
        Logger.printf("⚠️ Cache check failed (HTTP %d) - assuming valid\n", httpCode);
        http.end();
        return false; // Can't verify, assume valid
    }
    
    String response = http.getString();
    http.end();
    
    // Parse the lastModified from response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    
    if (error)
    {
        // Response might be plain text lastModified
        response.trim();
        if (response.length() > 0 && response != cachedEtag)
        {
            Logger.printf("📡 Remote changed: '%s' != '%s'\n", response.c_str(), cachedEtag);
            return true; // Changed
        }
        return false; // Same or parse error
    }
    
    // Check for lastModified field in JSON response
    const char* remoteLastModified = doc["lastModified"] | doc["etag"] | "";
    
    if (strlen(remoteLastModified) > 0 && strcmp(remoteLastModified, cachedEtag) != 0)
    {
        Logger.printf("📡 Remote lastModified changed: '%s' != '%s'\n", remoteLastModified, cachedEtag);
        return true; // Data has changed
    }
    
    Logger.println("✅ Cache still valid (lastModified unchanged)");
    return false; // Cache is still valid
}

/**
 * @brief Callback type for processing audio files during JSON parsing
 * @param file The audio file to process
 * @param userData Optional user data pointer for context
 * @return true if processing was successful
 */
typedef void (*AudioFileProcessCallback)(const AudioFile* file, void* userData);

/**
 * @brief Parse JSON string and process audio files with a callback
 * @param jsonString JSON string containing audio file entries
 * @param callback Function to call for each audio file entry
 * @param userData Optional user data to pass to callback
 * @return Number of files successfully processed, -1 on parse error
 */
static int parseAndRegisterAudioFiles(const String& jsonString, AudioFileProcessCallback callback=nullptr, void* userData = nullptr)
{
    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    
    if (error)
    {
        Logger.printf("❌ JSON parse error: %s\n", error.c_str());
        return -1;
    }
    
    // Extract lastModified for cache validation (if present at root level)
    const char* lastModified = doc["lastModified"] | "";
    if (strlen(lastModified) > 0)
    {
        saveCachedEtag(lastModified);
        Logger.printf("📋 Cached lastModified: %s\n", lastModified);
    }
    else
    {
        // Generate a timestamp-based etag if server doesn't provide one
        char timestampEtag[32];
        snprintf(timestampEtag, sizeof(timestampEtag), "ts-%lu", millis());
        saveCachedEtag(timestampEtag);
    }
    
    JsonObject root = doc.as<JsonObject>();
    int processedCount = 0;

    for (JsonPair kv : root)
    {
        if (processedCount >= MAX_AUDIO_FILES)
        {
            Logger.println("⚠️ Maximum audio files limit reached");
            break;
        }

        // Skip metadata keys (lastModified, etag, etc.) - only process object entries
        if (!kv.value().is<JsonObject>()) {
            continue;
        }
        
        JsonObject entryData = kv.value().as<JsonObject>();
        const char* key = kv.key().c_str();

        // Create temporary AudioFile on stack
        AudioFile file;
        file.audioKey = key;
        file.description = entryData["description"] | "Unknown";
        file.type = entryData["type"] | "unknown";
        file.data = entryData["path"] | entryData["data"]  | "";
        file.ext = entryData["ext"] | "";
        file.gap = entryData["gap"] | 0;
        file.duration = entryData["duration"] | 0;
        file.ringDuration = entryData["ring_duration"] | 0;
        
        // Register the main audio file
        if (!registerAudioFile(&file)) {
            continue;
        }
        processedCount++;

#if ENABLE_PLAYLIST_FEATURES
        // Build playlist with enrichment (ringback, click, previous/next)
        // Build desired playlist nodes
        std::vector<PlaylistNode> desiredNodes;
        
        // Parse "previous" array
        if (entryData["previous"].is<JsonArray>()) {
            JsonArray prevArray = entryData["previous"].as<JsonArray>();
            for (JsonVariant item : prevArray) {
                const char* prevKey = item.as<const char*>();
                if (prevKey && strlen(prevKey) > 0) {
                    desiredNodes.emplace_back(prevKey, 0, 0);
                }
            }
        }
        
        // Add ringback if specified
        if (file.ringDuration > 0)
            desiredNodes.emplace_back("ringback", 0, file.ringDuration);
        
        // Add the main audio file
        desiredNodes.emplace_back(file.audioKey, file.gap, file.duration);
        
        // Add click after main audio (only if "click" is a registered key)
        if (keyRegistry.hasKey("click"))
            desiredNodes.emplace_back("click", 0, 0);
        
        // Parse "next" array
        if (entryData["next"].is<JsonArray>()) {
            JsonArray nextArray = entryData["next"].as<JsonArray>();
            for (JsonVariant item : nextArray) {
                const char* nextKey = item.as<const char*>();
                if (nextKey && strlen(nextKey) > 0) {
                    desiredNodes.emplace_back(nextKey, 0, 0);
                }
            }
        }
        
        // Create or update the playlist
        Playlist *playlist = playlistRegistry.getPlaylistMutable(key);
        if (playlist) {
            playlist->update(desiredNodes);
        } else {
            playlist = playlistRegistry.createPlaylist(key);
            if (playlist) {
                for (const auto& node : desiredNodes) {
                    playlist->append(node);
                }
            }
        }
#else
        // Simple mode: no playlist creation, just use audio keys directly
#endif
        
        // Invoke callback if provided
        if (callback)
            callback(&file, userData);
    }

    return processedCount;
}

/**
 * @brief Check if cache is stale (needs refresh)
 * 
 * Uses a two-tier caching strategy:
 * 1. Lightweight check (every CACHE_CHECK_INTERVAL_MS): Quick lastModified comparison
 * 2. Full refresh (every CACHE_VALIDITY_HOURS): Force complete re-download
 * 
 * @param audioFileCount Optional count - if not provided, uses registry size
 * @return true if cache needs refresh, false otherwise
 */
static bool isCacheStale(int audioFileCount = -1)
{
    // Use registry size if count not provided
    if (audioFileCount < 0)
        audioFileCount = keyRegistry.size();
    
    if (audioFileCount == 0)
    {
        return true;
    }
    
    // No SD card means no persistent cache - always fetch fresh data
    if (!sdCardAvailable)
    {
        return true;
    }
    
    if (!initializeSDCard())
    {
        Logger.println("⚠️ Cannot check cache age without SD card");
        return true; // No cache available, needs refresh
    }
    
    // Read cache timestamp from file
    File timestampFile = SD_OPEN(CACHE_TIMESTAMP_FILE, FILE_READ);
    if (!timestampFile)
    {
        Logger.println("ℹ️ No cache timestamp file found");
        return true; // No timestamp file means stale cache
    }
    
    String timestampStr = timestampFile.readString();
    timestampFile.close();
    
    unsigned long savedTime = timestampStr.toInt();
    unsigned long currentTime = millis();
    unsigned long maxAge = CACHE_VALIDITY_HOURS * 60 * 60 * 1000UL; // Convert to milliseconds
    
    // Handle millis() rollover: if currentTime < savedTime, rollover occurred
    unsigned long cacheAge;
    if (currentTime >= savedTime)
    {
        cacheAge = currentTime - savedTime;
    }
    else
    {
        // Rollover occurred: calculate age considering 32-bit unsigned overflow
        cacheAge = (0xFFFFFFFF - savedTime) + currentTime + 1;
    }
    
    // TIER 2: Force full refresh after CACHE_VALIDITY_HOURS
    if (cacheAge > maxAge)
    {
        Logger.printf("⏰ Cache expired (age: %lu ms > max: %lu ms)\n", cacheAge, maxAge);
        return true;
    }
    
    // TIER 1: Lightweight check if WiFi connected and enough time has passed
    unsigned long timeSinceLastCheck = currentTime - lastCacheCheck;
    if (timeSinceLastCheck > CACHE_CHECK_INTERVAL_MS && WiFi.status() == WL_CONNECTED)
    {
        lastCacheCheck = currentTime;
        Logger.println("🔍 Performing lightweight cache validation...");
        
        if (checkRemoteCacheValid())
        {
            return true; // Remote data has changed
        }
    }
    
    return false; // Cache is still valid
}

/**
 * @brief Load audio files from SD card and register with AudioKeyRegistry
 * @return Number of files registered, 0 if failed or no cache
 */
static int loadAudioFilesFromSDCard()
{
    Logger.println("📖 Loading audio files from SD card...");
    
    if (!initializeSDCard())
    {
        Logger.println("❌ SD card not available for reading");
        return 0;
    }
    
    // Check if audio files JSON exists
    if (!SD_EXISTS(AUDIO_JSON_FILE))
    {
        Logger.println("ℹ️ No cached audio files found on SD card");
        return 0;
    }
    
    // Open audio files JSON
    File audioJsonFile = SD_OPEN(AUDIO_JSON_FILE, FILE_READ);
    if (!audioJsonFile)
    {
        Logger.println("❌ Failed to open audio files JSON for reading");
        return 0;
    }
    
    // Read file content
    String jsonString = audioJsonFile.readString();
    audioJsonFile.close();
    
    if (jsonString.length() == 0)
    {
        Logger.println("❌ Empty audio files JSON on SD card");
        return 0;
    }
    
    // Load cache timestamp
    File timestampFile = SD_OPEN(CACHE_TIMESTAMP_FILE, FILE_READ);
    if (timestampFile)
    {
        String timestampStr = timestampFile.readString();
        lastCacheTime = timestampStr.toInt();
        timestampFile.close();
    }
    else
    {
        lastCacheTime = 0;
        Logger.println("⚠️ No cache timestamp found");
    }
    
    // Parse and register audio files from JSON
    int registeredCount = parseAndRegisterAudioFiles(jsonString);
    
    if (registeredCount < 0) {
        return 0; // Parse error
    }
    
    // Resolve all playlists after registration
    playlistRegistry.resolveAllPlaylists();
    
    Logger.printf("✅ Loaded and registered %d audio files from SD card\n", registeredCount);
    return registeredCount;
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

AudioSource *initializeAudioFileManager()
{
    Logger.println("🔧 Initializing Audio File Manager...");
    
    // Reset state for clean initialization
    lastCacheTime = 0;
    sdCardAvailable = false;
    sdCardInitFailed = false;
    spiInitialized = false;

    AudioSource* source = nullptr;

    // Initialize SD card (handles SPI/MMC setup and retries internally)
    if (initializeSDCard())
    {
        // Create audio source appropriate for the SD mode
#if SD_USE_MMC
        source = new AudioSourceSDMMC(SD_AUDIO_PATH, "wav");
        Logger.println("✅ AudioSourceSDMMC created");
#else
        source = new AudioSourceSD(SD_AUDIO_PATH, "wav", SD_CS_PIN, SPI);
        Logger.println("✅ AudioSourceSD created");
#endif
    }
    else
    {
        Logger.println("⚠️ SD card not available - running in memory-only mode");
        Logger.println("ℹ️ Audio catalog will be downloaded when WiFi is available");
        return source;
    }

    // Try to load from SD card first - registers directly with AudioKeyRegistry
    int audioFileCount = loadAudioFilesFromSDCard();
    if (audioFileCount > 0)
    {
        Logger.println("✅ Audio files loaded from SD card cache");
        
        // Check if cache is stale
        bool stale = isCacheStale(audioFileCount);
        if (stale)
        {
            Logger.println("⏰ Cache is stale, will refresh when WiFi is available");
        }
        keyRegistry.listKeys();
        
        // Only queue downloads from cache if it's NOT stale
        // If stale, we'll get a fresh catalog with correct extensions first
        if (!stale)
        {
            // Queue any missing remote audio files so downloads can start immediately
            enqueueMissingAudioFilesFromRegistry();
        }
        else
        {
            Logger.println("ℹ️ Deferring download queue until catalog is refreshed");
        }
    }
    else
    {
        Logger.println("ℹ️ No cached audio files found, will download when WiFi is available");
    }
    
    return source;
}

// Forward declaration for internal download function
static bool downloadAudioInternal();

bool downloadAudio(int maxRetries, unsigned long retryDelayMs)
{
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED)
    {
        Logger.println("❌ WiFi not connected, cannot download audio files");
        return false;
    }
    
    // Check if cache is still valid
    if (!isCacheStale())
    {
        Logger.println("✅ Cache is still valid, skipping download");
        return true;
    }
    
    // Retry loop
    for (int attempt = 1; attempt <= maxRetries; attempt++)
    {
        if (attempt > 1)
        {
            Logger.printf("🔄 Retry attempt %d/%d after %lums delay...\n", attempt, maxRetries, retryDelayMs);
            delay(retryDelayMs);
        }
        
        if (downloadAudioInternal())
        {
            return true;
        }
    }
    
    Logger.printf("❌ Download failed after %d attempts\n", maxRetries);
    return false;
}

// Internal download implementation (single attempt)
static bool downloadAudioInternal()
{
    Logger.println("🌐 Downloading list from server...");
    
    // Build catalog URL with streaming parameter
    // - SD card available: streaming=false -> direct Drive download URLs for caching
    // - URL streaming mode: streaming=true -> authenticated URLs for real-time playback
    String catalogUrl = KNOWN_SEQUENCES_URL;
    String originalHost = "";
    
    // If DNS is pre-cached, try to use the IP instead of hostname
    // This allows downloads to work even if WireGuard broke public DNS
    if (dnsPreCached && cachedGitHubIP != IPAddress(0,0,0,0))
    {
        // Extract and replace hostname with cached IP
        int protoEnd = catalogUrl.indexOf("://");
        if (protoEnd > 0)
        {
            String protocol = catalogUrl.substring(0, protoEnd + 3);
            int hostStart = protoEnd + 3;
            int hostEnd = catalogUrl.indexOf('/', hostStart);
            if (hostEnd > hostStart)
            {
                originalHost = catalogUrl.substring(hostStart, hostEnd);
                String path = catalogUrl.substring(hostEnd);
                catalogUrl = protocol + cachedGitHubIP.toString() + path;
                Logger.printf("🌐 Using cached IP: %s -> %s\n", originalHost.c_str(), cachedGitHubIP.toString().c_str());
            }
        }
    }
    
    // Check if URL already has query parameters
    if (catalogUrl.indexOf('?') >= 0) {
        catalogUrl += "&streaming=";
    } else {
        catalogUrl += "?streaming=";
    }
    catalogUrl += sdCardAvailable ? "false" : "true";
    
    if (sdCardAvailable) {
        Logger.println("💾 SD card available - requesting direct download URLs");
    } else {
        Logger.println("🌐 URL streaming mode - requesting authenticated URLs");
    }
    
    HTTPClient http;
    http.begin(catalogUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", USER_AGENT_HEADER);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);  // 10 second timeout to prevent blocking
    
    // Add Host header if we're using IP instead of hostname (required for virtual hosts)
    if (originalHost.length() > 0)
    {
        http.addHeader("Host", originalHost);
    }
    
    Logger.printf("📡 Making GET request to: %s\n", catalogUrl.c_str());
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode != 200)
    {
        Logger.printf("❌ HTTP request failed: %d\n", httpResponseCode);
        http.end();
        return false;
    }
    
    String payload = http.getString();
    http.end();
    
    Logger.printf("✅ Received response (%d bytes)\n", payload.length());
    
    if (payload.length() > MAX_HTTP_RESPONSE_SIZE)
    {
        Logger.println("❌ Response too large");
        return false;
    }
    
    // Local mark-and-sweep: collect existing non-generator keys, then remove
    // any that weren't in the new catalog
    std::set<std::string> existingKeys;
    for (const auto& pair : keyRegistry) {
        // Only track non-generator keys for pruning
        if (pair.second.type != AudioStreamType::GENERATOR) {
            existingKeys.insert(pair.first);
        }
    }
    
    // Track which keys we see in the new catalog
    std::set<std::string> seenKeys;
    
    // Parse and register audio files directly with registry
    int registeredCount = parseAndRegisterAudioFiles(payload, 
            [](const AudioFile* file, void* userData) -> void {
        auto* seenKeys = static_cast<std::set<std::string>*>(userData);
            if (seenKeys) {
                seenKeys->insert(file->audioKey);
            }
    }, &seenKeys);
    
    if (registeredCount < 0) {
        return false; // Parse error
    }
    
    // Sweep: remove keys that existed before but weren't in new catalog
    int prunedCount = 0;
    for (const auto& key : existingKeys) {
        if (seenKeys.find(key) == seenKeys.end()) {
            Logger.printf("🗑️ Pruning orphaned key: %s\n", key.c_str());
            keyRegistry.unregisterKey(key.c_str());
            prunedCount++;
        }
    }
    if (prunedCount > 0) {
        Logger.printf("✅ Pruned %d orphaned audio keys\n", prunedCount);
    }
    
    // Resolve all playlists after registration
    playlistRegistry.resolveAllPlaylists();
    
    Logger.printf("✅ Downloaded and registered %d audio files%s\n", 
                  registeredCount,
                  prunedCount > 0 ? " (pruned orphans)" : "");
    
    // Only save to SD card and queue downloads if SD is available
    if (sdCardAvailable)
    {
        // Save raw JSON payload to SD card for caching
        File audioJsonFile = SD_OPEN(AUDIO_JSON_FILE, FILE_WRITE);
        if (audioJsonFile)
        {
            audioJsonFile.print(payload);
            audioJsonFile.close();
            
            // Save timestamp
            File timestampFile = SD_OPEN(CACHE_TIMESTAMP_FILE, FILE_WRITE);
            if (timestampFile)
            {
                timestampFile.print(millis());
                timestampFile.close();
                lastCacheTime = millis();
            }
            Logger.println("💾 Audio catalog cached to SD card");
        }
        else
        {
            Logger.println("⚠️ Failed to cache audio catalog to SD card");
        }

        // Clear old download queue before re-populating
        downloadQueueCount = 0;
        downloadQueueIndex = 0;
        
        // Queue any missing remote audio files from the registry
        enqueueMissingAudioFilesFromRegistry();
    }
    else
    {
        Logger.println("ℹ️ No SD card - audio catalog held in memory only");
    }
    
    return true;
}

void invalidateAudioCache()
{
    Logger.println("🔄 Invalidating audio cache...");
    lastCacheTime = 0;  // Force cache to be considered stale
    
    // Also remove timestamp file from SD card
    if (sdCardAvailable && initializeSDCard())
    {
        if (SD_EXISTS(CACHE_TIMESTAMP_FILE))
        {
            SD_REMOVE(CACHE_TIMESTAMP_FILE);
        }
    }
    Logger.println("✅ Cache invalidated - next download will fetch fresh data");
}

// ============================================================================
// AUDIO MAINTENANCE LOOP
// ============================================================================

void audioMaintenanceLoop()
{
    // 1. Periodic catalog refresh: re-download if cache is stale
    //    isCacheStale() is lightweight — only does HTTP when enough time has passed
    static unsigned long lastCatalogCheck = 0;
    unsigned long now = millis();
    if (now - lastCatalogCheck >= CACHE_CHECK_INTERVAL_MS)
    {
        lastCatalogCheck = now;
        if (WiFi.status() == WL_CONNECTED && isCacheStale())
        {
            Logger.println("🔄 Cache stale — refreshing audio catalog...");
            downloadAudio(1, 0);  // Single attempt, no delay (non-blocking context)
        }
    }

    // 2. Process download queue (rate-limited internally)
    processAudioDownloadQueue();
}

// ============================================================================
// DOWNLOAD QUEUE MANAGEMENT FUNCTIONS
// ============================================================================

bool processAudioDownloadQueue()
{
    // Skip if SD card is not available - audio files need SD storage
    if (!initializeSDCard())
    {
        return false;
    }
    
    static unsigned long lastDownloadCheck = 0;
    
    // Rate limit: only process if enough time has passed
    if (millis() - lastDownloadCheck < DOWNLOAD_QUEUE_CHECK_INTERVAL_MS)
    {
        return false;
    }
    
    lastDownloadCheck = millis();
    return processDownloadQueueInternal();
}

int getDownloadQueueCount()
{
    return downloadQueueCount - downloadQueueIndex; // Remaining items
}

int getTotalDownloadQueueSize()
{
    return downloadQueueCount;
}

void listDownloadQueue()
{
    Logger.printf("📥 Audio Download Queue (%d items, %d processed):\n", 
                 downloadQueueCount, downloadQueueIndex);
    Logger.println("========================================================");
    
    if (downloadQueueCount == 0)
    {
        Logger.println("   No items in download queue.");
        return;
    }
    
    for (int i = 0; i < downloadQueueCount; i++)
    {
        AudioDownloadItem* item = &downloadQueue[i];
        const char* status = i < downloadQueueIndex ? "✅ Downloaded" : 
                           item->inProgress ? "🔄 In Progress" : "⏳ Pending";
        
        Logger.printf("%2d. %s %s\n", i + 1, status, item->description);
        Logger.printf("    URL: %s\n", item->url);
        Logger.printf("    Local: %s\n", item->localPath);
        Logger.println();
    }
}

void clearDownloadQueue()
{
    Logger.println("🗑️ Clearing download queue...");
    downloadQueueCount = 0;
    downloadQueueIndex = 0;
    Logger.println("✅ Download queue cleared");
}

bool isDownloadQueueEmpty()
{
    return (downloadQueueIndex >= downloadQueueCount);
}

// ============================================================================
// REGISTRY INTEGRATION
// ============================================================================

/**
 * @brief Register a single audio file with the AudioKeyRegistry and create its playlist
 * 
 * This function is idempotent - calling it multiple times with the same AudioFile
 * will produce the same result. Existing registrations are updated, playlists are
 * recreated with the same content.
 * 
 * @param file Pointer to the AudioFile to register
 * @return true if registration successful, false if skipped or failed
 */
bool registerAudioFile(const AudioFile* file)
{
    if (!file || !file->audioKey || !file->type) {
        return false;
    }
    
    // Only register audio type entries
    if (strcmp(file->type, "audio") != 0) {
        Logger.printf("⏭️ Skipping non-audio entry: %s (%s)\n", file->audioKey, file->type);
        return false;
    }
    
    if (!file->data || strlen(file->data) == 0) {
        Logger.printf("⚠️ No data for: %s\n", file->audioKey);
        return false;
    }
    // Register the audio key - overload handles stream type detection and URL fallback
    // registerKey is idempotent - it will update existing entries
    keyRegistry.registerKey(file->audioKey, file->data, file->ext);
    
    return true;
}

unsigned long getAudioKeyRingDuration(const char* key)
{
    if (!key) return 0;
    
    const Playlist* playlist = playlistRegistry.getPlaylist(key);
    if (!playlist) return 0;
    
    // Look for a "ringback" node in the playlist and return its duration
    for (const auto& node : playlist->nodes) {
        if (node.audioKey == "ringback" && node.durationMs > 0) {
            return node.durationMs;
        }
    }
    return 0;
}
