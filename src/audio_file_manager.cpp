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
#include "audio_player.h"  // For isURLStreamingMode()
#include "logging.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SD_MMC.h>
#include <FS.h>
#include <SPI.h>
#include "AudioTools/Disk/AudioSourceSD.h"

// Helper macros for SD vs SD_MMC abstraction
#define SD_CARD (sdMmmcSupport ? (fs::FS&)SD_MMC : (fs::FS&)SD)
#define SD_EXISTS(path) (sdMmmcSupport ? SD_MMC.exists(path) : SD.exists(path))
#define SD_OPEN(path, mode) (sdMmmcSupport ? SD_MMC.open(path, mode) : SD.open(path, mode))
#define SD_MKDIR(path) (sdMmmcSupport ? SD_MMC.mkdir(path) : SD.mkdir(path))
#define SD_REMOVE(path) (sdMmmcSupport ? SD_MMC.remove(path) : SD.remove(path))

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

static AudioFile audioFiles[MAX_KNOWN_SEQUENCES];
static int audioFileCount = 0;
static unsigned long lastCacheTime = 0;
static bool sdCardAvailable = false;  // True if SD card is mounted and accessible
static bool sdCardInitFailed = false; // True if SD init was attempted and failed (don't retry)
static int sdCardCsPin = 13; // SD card chip select pin
static bool sdMmmcSupport = false; // MMC support flag

// DNS cache for use after WireGuard breaks public DNS
static IPAddress cachedGitHubIP;
static bool dnsPreCached = false;

// Download queue management
static AudioDownloadItem downloadQueue[MAX_DOWNLOAD_QUEUE];
static int downloadQueueCount = 0;
static int downloadQueueIndex = 0; // Current processing index

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Initialize SD card if not already done
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
    
    if (sdMmmcSupport)
    {
        // Using SD_MMC mode (SDMMC interface) - assumes already initialized in main.ino
        uint8_t cardType = SD_MMC.cardType();
        if (cardType == CARD_NONE)
        {
            Logger.println("❌ No SD_MMC card detected");
            sdCardInitFailed = true;
            return false;
        }
        
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Logger.printf("✅ SD_MMC Card Size: %lluMB\n", cardSize);
        sdCardAvailable = true;
        return true;
    }
    else
    {
        // Using SD mode (SPI interface)
        if (!SD.begin(sdCardCsPin))
        {
            Logger.println("❌ SD card initialization failed");
            sdCardInitFailed = true;
            return false;
        }
        delay(1000);
        uint8_t cardType = SD.cardType();
        if (cardType == CARD_NONE)
        {
            Logger.println("❌ No SD card attached");
            sdCardInitFailed = true;
            return false;
        }
        
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Logger.printf("✅ SD Card Size: %lluMB\n", cardSize);
        sdCardAvailable = true;
        return true;
    }
}

/**
 * @brief Convert URL to filesystem-safe filename (base hash only, no collision avoidance)
 * @param url Original URL
 * @param filename Output buffer for filename (should be at least MAX_FILENAME_LENGTH)
 * @param ext File extension to use (e.g., "wav", "mp3") - can be NULL to use default
 * @return true if conversion successful, false otherwise
 */
static bool urlToBaseFilename(const char* url, char* filename, const char* ext = nullptr)
{
    if (!url || !filename)
    {
        return false;
    }
    
    // Determine extension to use (default to mp3 if not specified)
    const char* extension = (ext && strlen(ext) > 0) ? ext : "mp3";
    
    // Extract filename from URL path or generate from URL hash
    const char* lastSlash = strrchr(url, '/');
    const char* urlFilename = lastSlash ? (lastSlash + 1) : url;
    
    // If we have a proper filename with extension, use it
    if (strlen(urlFilename) > 0 && strchr(urlFilename, '.'))
    {
        // Clean the filename - replace invalid characters
        int j = 0;
        for (int i = 0; urlFilename[i] && j < MAX_FILENAME_LENGTH - 1; i++)
        {
            char c = urlFilename[i];
            // Allow alphanumeric, dots, hyphens, underscores
            if (isalnum(c) || c == '.' || c == '-' || c == '_')
            {
                filename[j++] = c;
            }
            else if (c == ' ')
            {
                filename[j++] = '_';
            }
            // Skip other invalid characters
        }
        filename[j] = '\0';
        
        if (strlen(filename) > 0)
        {
            return true;
        }
    }
    
    // Generate filename from URL hash if no suitable filename found
    unsigned long hash = 5381;
    for (int i = 0; url[i]; i++)
    {
        hash = ((hash << 5) + hash) + url[i];
    }
    
    // Just use the base filename without collision avoidance
    snprintf(filename, MAX_FILENAME_LENGTH, "audio_%08lx.%s", hash, extension);
    return true;
}

/**
 * @brief Convert URL to filesystem-safe filename (with collision avoidance for new downloads)
 * @param url Original URL
 * @param filename Output buffer for filename (should be at least MAX_FILENAME_LENGTH)
 * @param ext File extension to use (e.g., "wav", "mp3") - can be NULL to use default
 * @return true if conversion successful, false otherwise
 */
static bool urlToFilename(const char* url, char* filename, const char* ext = nullptr)
{
    if (!url || !filename)
    {
        return false;
    }
    
    // Determine extension to use (default to mp3 if not specified)
    const char* extension = (ext && strlen(ext) > 0) ? ext : "mp3";
    
    // Extract filename from URL path or generate from URL hash
    const char* lastSlash = strrchr(url, '/');
    const char* urlFilename = lastSlash ? (lastSlash + 1) : url;
    
    // If we have a proper filename with extension, use it
    if (strlen(urlFilename) > 0 && strchr(urlFilename, '.'))
    {
        // Clean the filename - replace invalid characters
        int j = 0;
        for (int i = 0; urlFilename[i] && j < MAX_FILENAME_LENGTH - 1; i++)
        {
            char c = urlFilename[i];
            // Allow alphanumeric, dots, hyphens, underscores
            if (isalnum(c) || c == '.' || c == '-' || c == '_')
            {
                filename[j++] = c;
            }
            else if (c == ' ')
            {
                filename[j++] = '_';
            }
            // Skip other invalid characters
        }
        filename[j] = '\0';
        
        if (strlen(filename) > 0)
        {
            return true;
        }
    }
    
    // Generate filename from URL hash if no suitable filename found
    unsigned long hash = 5381;
    for (int i = 0; url[i]; i++)
    {
        hash = ((hash << 5) + hash) + url[i];
    }
    
    // Check if file with this hash already exists; if so, add counter
    char baseFilename[MAX_FILENAME_LENGTH];
    snprintf(baseFilename, MAX_FILENAME_LENGTH, "audio_%08lx.%s", hash, extension);
    
    // Check for hash collision by testing if file exists
    if (initializeSDCard())
    {
        char testPath[128];
        snprintf(testPath, sizeof(testPath), "%s/%s", AUDIO_FILES_DIR, baseFilename);
        
        if (SD_EXISTS(testPath))
        {
            // File exists, add a counter suffix
            for (int counter = 1; counter < 1000; counter++)
            {
                snprintf(filename, MAX_FILENAME_LENGTH, "audio_%08lx_%d.%s", hash, counter, extension);
                snprintf(testPath, sizeof(testPath), "%s/%s", AUDIO_FILES_DIR, filename);
                
                if (!SD_EXISTS(testPath))
                {
                    // Found an unused filename
                    return true;
                }
            }
            
            // Too many collisions, just use the base name and overwrite
            Logger.println("⚠️ Too many hash collisions, using base filename");
        }
    }
    
    snprintf(filename, MAX_FILENAME_LENGTH, "%s", baseFilename);
    return true;
}

/**
 * @brief Get local audio file path for a URL (uses base filename for lookups)
 * @param url Original URL
 * @param localPath Output buffer for local path
 * @param ext File extension to use (e.g., "wav", "mp3") - can be NULL
 * @return true if path generated successfully, false otherwise
 */
static bool getLocalAudioPath(const char* url, char* localPath, const char* ext = nullptr)
{
    if (!url || !localPath)
    {
        return false;
    }
    
    char filename[MAX_FILENAME_LENGTH];
    // Use base filename (without collision avoidance) for path lookups
    if (!urlToBaseFilename(url, filename, ext))
    {
        return false;
    }
    
    snprintf(localPath, 128, "%s/%s", AUDIO_FILES_DIR, filename);
    return true;
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
    if (!getLocalAudioPath(url, localPath, ext))
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

    if (!getLocalAudioPath(url, item->localPath, ext))
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
 * @brief Queue downloads for any missing HTTP/HTTPS audio files
 */
static void enqueueMissingAudioFiles()
{
    if (audioFileCount == 0)
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

    for (int i = 0; i < audioFileCount; i++)
    {
        const char* type = audioFiles[i].type;
        const char* path = audioFiles[i].path;
        const char* ext = audioFiles[i].ext;

        if (!type || strcmp(type, "audio") != 0)
        {
            continue; // Only queue audio entries
        }

        if (!path || strlen(path) == 0)
        {
            continue; // Nothing to fetch
        }

        bool isHttp = strncmp(path, "http://", 7) == 0;
        bool isHttps = strncmp(path, "https://", 8) == 0;

        if (!isHttp && !isHttps)
        {
            continue; // Local paths are already available
        }

        if (audioFileExists(path, ext))
        {
            continue; // Already cached
        }

        if (addToDownloadQueue(path, audioFiles[i].description, ext))
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
    
    int httpCode = http.GET();
    
    if (httpCode == 200)
    {
        // Get content length for progress tracking
        int contentLength = http.getSize();
        
        // Clean up any old files with different extensions for the same URL
        // This handles cases where a file was previously downloaded with .mp3 but should now be .wav
        char filenameBase[64];
        urlToFilename(item->url, filenameBase, nullptr);  // Get base filename without extension
        // Remove the .mp3 that urlToFilename adds when ext is null
        char* dot = strrchr(filenameBase, '.');
        if (dot) *dot = '\0';
        
        const char* extensions[] = {".mp3", ".wav", ".ogg", ".flac", ".aac"};
        for (int i = 0; i < 5; i++)
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
 * @brief Check if cache is stale
 * @return true if cache needs refresh, false otherwise
 */
static bool isCacheStale()
{
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
    
    return (cacheAge > maxAge);
}

/**
 * @brief Save audio files to SD card
 * @return true if successful, false otherwise
 */
static bool saveAudioFilesToSDCard()
{
    Logger.println("💾 Saving audio files to SD card...");
    
    if (!initializeSDCard())
    {
        Logger.println("❌ SD card not available for writing");
        return false;
    }
    
    // Create JSON document for storage
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    
    for (int i = 0; i < audioFileCount; i++)
    {
        JsonObject entry = root[audioFiles[i].audioKey].to<JsonObject>();
        entry["description"] = audioFiles[i].description;
        entry["type"] = audioFiles[i].type;
        entry["path"] = audioFiles[i].path;
        if (audioFiles[i].ext && strlen(audioFiles[i].ext) > 0)
        {
            entry["ext"] = audioFiles[i].ext;
        }
        if (audioFiles[i].ringDuration > 0)
        {
            entry["ring_duration"] = audioFiles[i].ringDuration;
        }
    }
    
    // Open file for writing
    File audioJsonFile = SD_OPEN(AUDIO_JSON_FILE, FILE_WRITE);
    if (!audioJsonFile)
    {
        Logger.println("❌ Failed to open audio files JSON for writing");
        return false;
    }
    
    // Write JSON to file
    size_t bytesWritten = serializeJson(doc, audioJsonFile);
    audioJsonFile.close();
    
    if (bytesWritten == 0)
    {
        Logger.println("❌ Failed to write audio files to JSON");
        return false;
    }
    
    // Save timestamp to separate file
    File timestampFile = SD_OPEN(CACHE_TIMESTAMP_FILE, FILE_WRITE);
    if (timestampFile)
    {
        timestampFile.print(millis());
        timestampFile.close();
        lastCacheTime = millis();
    }
    else
    {
        Logger.println("⚠️ Failed to save cache timestamp");
    }
    
    Logger.printf("✅ Saved %d audio files to SD card (%d bytes)\n", 
                 audioFileCount, bytesWritten);
    
    return true;
}

/**
 * @brief Load audio files from SD card
 * @return true if successful, false otherwise
 */
static bool loadAudioFilesFromSDCard()
{
    Logger.println("📖 Loading audio files from SD card...");
    
    if (!initializeSDCard())
    {
        Logger.println("❌ SD card not available for reading");
        return false;
    }
    
    // Check if audio files JSON exists
    if (!SD_EXISTS(AUDIO_JSON_FILE))
    {
        Logger.println("ℹ️ No cached audio files found on SD card");
        return false;
    }
    
    // Open audio files JSON
    File audioJsonFile = SD_OPEN(AUDIO_JSON_FILE, FILE_READ);
    if (!audioJsonFile)
    {
        Logger.println("❌ Failed to open audio files JSON for reading");
        return false;
    }
    
    // Read file content
    String jsonString = audioJsonFile.readString();
    audioJsonFile.close();
    
    if (jsonString.length() == 0)
    {
        Logger.println("❌ Empty audio files JSON on SD card");
        return false;
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
    
    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    
    if (error)
    {
        Logger.printf("❌ JSON parse error: %s\n", error.c_str());
        return false;
    }
    
    // Clear existing audio files
    audioFileCount = 0;
    
    // Load audio files from JSON
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root)
    {
        if (audioFileCount >= MAX_KNOWN_SEQUENCES)
        {
            Logger.println("⚠️ Maximum audio files limit reached");
            break;
        }
        
        const char* key = kv.key().c_str();
        JsonObject entryData = kv.value().as<JsonObject>();
        
        // Allocate and copy strings
        audioFiles[audioFileCount].audioKey = strdup(key);
        audioFiles[audioFileCount].description = strdup(entryData["description"] | "Unknown");
        audioFiles[audioFileCount].type = strdup(entryData["type"] | "unknown");
        audioFiles[audioFileCount].path = strdup(entryData["path"] | "");
        audioFiles[audioFileCount].ext = strdup(entryData["ext"] | "");
        audioFiles[audioFileCount].ringDuration = entryData["ring_duration"] | 0;
        
        audioFileCount++;;
    }
    
    Logger.printf("✅ Loaded %d audio files from SD card\n", audioFileCount);
    return true;
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

AudioSource *initializeAudioFileManager(int sdCsPin, bool mmcSupport,
                                        int sdClkPin, int sdMosiPin, int sdMisoPin,
                                        const char *startFilePath,
                                        bool *sdCardAvailableOut)
{
    Logger.println("🔧 Initializing Audio File Manager...");
    
    // Initialize variables
    sdMmmcSupport = mmcSupport;
    sdCardCsPin = sdCsPin;
    audioFileCount = 0;
    lastCacheTime = 0;
    sdCardAvailable = false;

    AudioSource* source = nullptr;

    // Initialize SD card in SPI mode
    if (!mmcSupport)
    {
        SPI.begin(sdClkPin, sdMisoPin, sdMosiPin, sdCsPin);
        
        for (int attempt = 1; attempt <= 3 && !sdCardAvailable; attempt++) {
            Logger.printf("🔧 SD SPI initialization attempt %d/3...\n", attempt);
            delay(attempt * 300);
            
            if (SD.begin(sdCsPin, SPI)) {
                uint8_t cardType = SD.cardType();
                if (cardType != CARD_NONE) {
                    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
                    Logger.printf("✅ SD card initialized (SPI mode, %lluMB)\n", cardSize);
                    sdCardAvailable = true;
                } else {
                    Logger.println("❌ No SD card detected");
                }
            } else {
                Logger.println("❌ SD.begin() failed");
            }
        }
        
        if (sdCardAvailable) {
            // Create AudioSourceSD now that SPI is initialized
            source = new AudioSourceSD(startFilePath, "wav", sdCsPin, SPI);
            Logger.println("✅ AudioSourceSD created");
        } else {
            Logger.println("⚠️ SD initialization failed - continuing without SD card");
        }
    }
    
    // Return SD card availability status if requested
    if (sdCardAvailableOut) {
        *sdCardAvailableOut = sdCardAvailable;
    }
    
    // Skip SD card operations if not available
    if (!sdCardAvailable)
    {
        Logger.println("⚠️ SD card not available - running in memory-only mode");
        Logger.println("ℹ️ Audio catalog will be downloaded when WiFi is available");
        return source;
    }
    
    // Try to load from SD card first
    if (loadAudioFilesFromSDCard())
    {
        Logger.println("✅ Audio files loaded from SD card cache");
        
        // Check if cache is stale
        bool stale = isCacheStale();
        if (stale)
        {
            Logger.println("⏰ Cache is stale, will refresh when WiFi is available");
        }
        listAudioKeys();
        
        // Only queue downloads from cache if it's NOT stale
        // If stale, we'll get a fresh catalog with correct extensions first
        if (!stale)
        {
            // Queue any missing remote audio files so downloads can start immediately
            enqueueMissingAudioFiles();
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
    
    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error)
    {
        Logger.printf("❌ JSON parse error: %s\n", error.c_str());
        return false;
    }
    
    // Clear existing audio files (free memory first)
    for (int i = 0; i < audioFileCount; i++)
    {
        free((void*)audioFiles[i].audioKey);
        free((void*)audioFiles[i].description);
        free((void*)audioFiles[i].type);
        free((void*)audioFiles[i].path);
        free((void*)audioFiles[i].ext);
    }
    audioFileCount = 0;
    
    // Load new audio files
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root)
    {
        if (audioFileCount >= MAX_KNOWN_SEQUENCES)
        {
            Logger.println("⚠️ Maximum audio files limit reached");
            break;
        }
        
        const char* key = kv.key().c_str();
        JsonObject entryData = kv.value().as<JsonObject>();
        
        // Allocate and copy strings
        audioFiles[audioFileCount].audioKey = strdup(key);
        audioFiles[audioFileCount].description = strdup(entryData["description"] | "Unknown");
        audioFiles[audioFileCount].type = strdup(entryData["type"] | "unknown");
        audioFiles[audioFileCount].path = strdup(entryData["path"] | "");
        audioFiles[audioFileCount].ext = strdup(entryData["ext"] | "");
        audioFiles[audioFileCount].ringDuration = entryData["ring_duration"] | 0;
        
        const char* extInfo = audioFiles[audioFileCount].ext;
        Logger.printf("📝 Added: %s -> %s (%s%s%s)\n", 
                     audioFiles[audioFileCount].audioKey,
                     audioFiles[audioFileCount].description,
                     audioFiles[audioFileCount].type,
                     (extInfo && strlen(extInfo) > 0) ? ", ." : "",
                     (extInfo && strlen(extInfo) > 0) ? extInfo : "");
        
        audioFileCount++;
    }
    
    Logger.printf("✅ Downloaded and parsed %d file list entries\n", audioFileCount);
    
    // Only save to SD card and queue downloads if SD is available
    if (sdCardAvailable)
    {
        // Save to SD card for caching
        if (saveAudioFilesToSDCard())
        {
            Logger.println("💾 Files list cached to SD card");
        }
        else
        {
            Logger.println("⚠️ Failed to cache files list to SD card");
        }

        // Clear old download queue before re-populating with new extensions
        downloadQueueCount = 0;
        downloadQueueIndex = 0;
        
        // Queue any missing remote audio files now that the list is refreshed
        enqueueMissingAudioFiles();
    }
    else
    {
        Logger.println("ℹ️ No SD card - audio catalog held in memory only");
    }
    
    return true;
}

bool hasAudioKey(const char *key)
{
    if (!key || audioFileCount == 0)
    {
        return false;
    }
    
    for (int i = 0; i < audioFileCount; i++)
    {
        if (strcmp(audioFiles[i].audioKey, key) == 0)
        {
            return true;
        }
    }
    
    return false;
}

unsigned long getAudioKeyRingDuration(const char *key)
{
    if (!key || audioFileCount == 0)
    {
        return 0;
    }
    
    for (int i = 0; i < audioFileCount; i++)
    {
        if (strcmp(audioFiles[i].audioKey, key) == 0)
        {
            return audioFiles[i].ringDuration;
        }
    }
    
    return 0;
}

bool hasAudioKeyWithPrefix(const char *prefix)
{
    if (!prefix || audioFileCount == 0)
    {
        return false;
    }
    
    size_t prefixLen = strlen(prefix);
    for (int i = 0; i < audioFileCount; i++)
    {
        // Check if any audio key starts with this prefix
        if (strncmp(audioFiles[i].audioKey, prefix, prefixLen) == 0)
        {
            return true;
        }
    }
    
    return false;
}

const char* processAudioKey(const char *key)
{
    if (!key)
    {
        Logger.println("❌ Invalid audio key pointer");
        return nullptr;
    }
    
    Logger.printf("🔍 Processing audio key: %s\n", key);
    
    // Find the audio file entry
    AudioFile *found = nullptr;
    for (int i = 0; i < audioFileCount; i++)
    {
        if (strcmp(audioFiles[i].audioKey, key) == 0)
        {
            found = &audioFiles[i];
            break;
        }
    }
    
    if (!found)
    {
        Logger.printf("❌ Audio key not found: %s\n", key);
        return nullptr;
    }
    
    // Log entry info
    Logger.printf("📋 Audio Entry:\n");
    Logger.printf("   Key: %s\n", found->audioKey);
    Logger.printf("   Description: %s\n", found->description);
    Logger.printf("   Type: %s\n", found->type);
    Logger.printf("   Path: %s\n", found->path);
    
    // Handle different entry types
    if (!found->type)
    {
        Logger.println("❌ Entry type is NULL");
        return nullptr;
    }
    
    if (strcmp(found->type, "audio") == 0)
    {
        Logger.printf("🔊 Processing audio: %s\n", found->description);
        
        if (!found->path || strlen(found->path) == 0)
        {
            Logger.println("❌ No audio path specified");
            return nullptr;
        }
        
        // Check if path is a URL
        if (strncmp(found->path, "http://", 7) == 0 || strncmp(found->path, "https://", 8) == 0)
        {
            // In URL streaming mode, return the URL directly for streaming
            if (isURLStreamingMode())
            {
                Logger.printf("🌐 URL streaming mode - returning URL directly: %s\n", found->path);
                return found->path;
            }
            
            // SD card mode - check for local cached version
            if (audioFileExists(found->path, found->ext))
            {
                // File exists locally - return path for playback
                static char localPath[128];
                if (getLocalAudioPath(found->path, localPath, found->ext))
                {
                    Logger.printf("🎵 Audio file found locally: %s\n", localPath);
                    return localPath;
                }
                else
                {
                    Logger.println("❌ Failed to generate local path");
                    return nullptr;
                }
            }
            else
            {
                // File doesn't exist - add to download queue
                Logger.printf("📥 Audio file not cached, adding to download queue\n");
                if (addToDownloadQueue(found->path, found->description, found->ext))
                {
                    Logger.printf("✅ Added to download queue: %s\n", found->description);
                }
                else
                {
                    Logger.printf("❌ Failed to add to download queue: %s\n", found->description);
                }
                
                Logger.printf("ℹ️ Audio will be available after download\n");
                return nullptr;
            }
        }
        else
        {
            // It's a local path - return for direct playback
            Logger.printf("🎵 Local audio path: %s\n", found->path);
            return found->path;
        }
    }
    else if (strcmp(found->type, "service") == 0)
    {
        Logger.printf("🔧 Service: %s\n", found->description);
        // TODO: Implement service access logic
        return nullptr;
    }
    else if (strcmp(found->type, "shortcut") == 0)
    {
        Logger.printf("⚡ Shortcut: %s\n", found->description);
        // TODO: Implement shortcut execution logic
        return nullptr;
    }
    else if (strcmp(found->type, "url") == 0)
    {
        Logger.printf("🌐 URL: %s\n", found->path ? found->path : "NULL");
        // TODO: Implement URL opening logic
        return nullptr;
    }
    else
    {
        Logger.printf("❓ Unknown type: %s\n", found->type);
        return nullptr;
    }
}

void listAudioKeys()
{
    Logger.printf("📋 Audio Files (%d total):\n", audioFileCount);
    Logger.println("============================================================");
    
    if (audioFileCount == 0)
    {
        Logger.println("   No audio files loaded.");
        Logger.println("   Try downloading with downloadAudio()");
        return;
    }
    
    for (int i = 0; i < audioFileCount; i++)
    {
        Logger.printf("%2d. %s\n", i + 1, audioFiles[i].audioKey);
        Logger.printf("    Description: %s\n", audioFiles[i].description);
        Logger.printf("    Type: %s\n", audioFiles[i].type);
        if (strlen(audioFiles[i].path) > 0)
        {
            Logger.printf("    Path: %s\n", audioFiles[i].path);
        }
        Logger.println();
    }
}

int getAudioKeyCount()
{
    return audioFileCount;
}

void clearAudioKeys()
{
    Logger.println("🗑️ Clearing audio files...");
    
    // Free allocated memory
    for (int i = 0; i < audioFileCount; i++)
    {
        free((void*)audioFiles[i].audioKey);
        free((void*)audioFiles[i].description);
        free((void*)audioFiles[i].type);
        free((void*)audioFiles[i].path);
        free((void*)audioFiles[i].ext);
    }
    
    int clearedCount = audioFileCount;
    audioFileCount = 0;
    lastCacheTime = 0;
    
    // Clear SD card cache files (only if SD is available)
    if (sdCardAvailable && initializeSDCard())
    {
        bool jsonRemoved = false;
        bool timestampRemoved = false;
        
        if (SD_EXISTS(AUDIO_JSON_FILE))
        {
            jsonRemoved = SD_REMOVE(AUDIO_JSON_FILE);
        }
        else
        {
            jsonRemoved = true; // File doesn't exist, consider it "removed"
        }
        
        if (SD_EXISTS(CACHE_TIMESTAMP_FILE))
        {
            timestampRemoved = SD_REMOVE(CACHE_TIMESTAMP_FILE);
        }
        else
        {
            timestampRemoved = true; // File doesn't exist, consider it "removed"
        }
        
        if (jsonRemoved && timestampRemoved)
        {
            Logger.println("✅ Cleared SD card cache files");
        }
        else
        {
            Logger.println("⚠️ Some SD card files could not be removed");
        }
    }
    else
    {
        Logger.println("⚠️ SD card not available for cache cleanup");
    }
    
    Logger.printf("✅ Cleared %d audio files from memory\n", clearedCount);
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