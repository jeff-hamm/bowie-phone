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
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SD_MMC.h>
#include <FS.h>

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
    bool inProgress;        ///< Whether download is currently in progress
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static AudioFile audioFiles[MAX_KNOWN_SEQUENCES];
static int audioFileCount = 0;
static unsigned long lastCacheTime = 0;
static bool sdCardInitialized = false;
static int sdCardCsPin = 13; // SD card chip select pin
static bool sdMmmcSupport = false; // MMC support flag

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
    if (sdCardInitialized)
    {
        return true;
    }
    
    Serial.println("🔧 Initializing SD card...");
    
    if (sdMmmcSupport)
    {
        // Using SD_MMC mode (SDMMC interface) - assumes already initialized in main.ino
        uint8_t cardType = SD_MMC.cardType();
        if (cardType == CARD_NONE)
        {
            Serial.println("❌ No SD_MMC card detected");
            return false;
        }
        
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("✅ SD_MMC Card Size: %lluMB\n", cardSize);
        sdCardInitialized = true;
        return true;
    }
    else
    {
        // Using SD mode (SPI interface)
        if (!SD.begin(sdCardCsPin))
        {
            Serial.println("❌ SD card initialization failed");
            return false;
        }
        delay(1000);
        uint8_t cardType = SD.cardType();
        if (cardType == CARD_NONE)
        {
            Serial.println("❌ No SD card attached");
            return false;
        }
        
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("✅ SD Card Size: %lluMB\n", cardSize);
        sdCardInitialized = true;
        return true;
    }
}

/**
 * @brief Convert URL to filesystem-safe filename
 * @param url Original URL
 * @param filename Output buffer for filename (should be at least MAX_FILENAME_LENGTH)
 * @return true if conversion successful, false otherwise
 */
static bool urlToFilename(const char* url, char* filename)
{
    if (!url || !filename)
    {
        return false;
    }
    
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
    snprintf(baseFilename, MAX_FILENAME_LENGTH, "audio_%08lx.mp3", hash);
    
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
                snprintf(filename, MAX_FILENAME_LENGTH, "audio_%08lx_%d.mp3", hash, counter);
                snprintf(testPath, sizeof(testPath), "%s/%s", AUDIO_FILES_DIR, filename);
                
                if (!SD_EXISTS(testPath))
                {
                    // Found an unused filename
                    return true;
                }
            }
            
            // Too many collisions, just use the base name and overwrite
            Serial.println("⚠️ Too many hash collisions, using base filename");
        }
    }
    
    snprintf(filename, MAX_FILENAME_LENGTH, "%s", baseFilename);
    return true;
}

/**
 * @brief Get local audio file path for a URL
 * @param url Original URL
 * @param localPath Output buffer for local path
 * @return true if path generated successfully, false otherwise
 */
static bool getLocalAudioPath(const char* url, char* localPath)
{
    if (!url || !localPath)
    {
        return false;
    }
    
    char filename[MAX_FILENAME_LENGTH];
    if (!urlToFilename(url, filename))
    {
        return false;
    }
    
    snprintf(localPath, 128, "%s/%s", AUDIO_FILES_DIR, filename);
    return true;
}

/**
 * @brief Check if audio file exists locally on SD card
 * @param url Original URL
 * @return true if file exists locally, false otherwise
 */
static bool audioFileExists(const char* url)
{
    if (!initializeSDCard())
    {
        return false;
    }
    
    char localPath[128];
    if (!getLocalAudioPath(url, localPath))
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
        Serial.println("❌ Invalid audio directory path");
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
            Serial.println("❌ Audio directory path too long");
            return false;
        }
        strcat(partial, token);

        if (!SD_EXISTS(partial))
        {
            if (!SD_MKDIR(partial))
            {
                Serial.printf("❌ Failed to create directory: %s\n", partial);
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
 * @return true if added successfully, false otherwise
 */
static bool addToDownloadQueue(const char *url, const char *description)
{
    if (downloadQueueCount >= MAX_DOWNLOAD_QUEUE)
    {
        Serial.println("⚠️ Download queue is full, cannot add more items");
        return false;
    }

    // Check if URL is already in queue
    for (int i = 0; i < downloadQueueCount; i++)
    {
        if (strcmp(downloadQueue[i].url, url) == 0)
        {
            Serial.printf("ℹ️ URL already in download queue: %s\n", url);
            return true; // Already queued, consider it success
        }
    }

    // Add new item to queue
    AudioDownloadItem *item = &downloadQueue[downloadQueueCount];
    strncpy(item->url, url, sizeof(item->url) - 1);
    item->url[sizeof(item->url) - 1] = '\0';

    if (!getLocalAudioPath(url, item->localPath))
    {
        Serial.printf("❌ Failed to generate local path for: %s\n", url);
        return false;
    }

    strncpy(item->description, description ? description : "Unknown", sizeof(item->description) - 1);
    item->description[sizeof(item->description) - 1] = '\0';

    item->inProgress = false;
    downloadQueueCount++;

    Serial.printf("📥 Added to download queue: %s -> %s\n", item->description, item->localPath);
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
        Serial.println("⚠️ SD card not available, skipping download pre-queue");
        return;
    }

    int queued = 0;

    for (int i = 0; i < audioFileCount; i++)
    {
        const char* type = audioFiles[i].type;
        const char* path = audioFiles[i].path;

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

        if (audioFileExists(path))
        {
            continue; // Already cached
        }

        if (addToDownloadQueue(path, audioFiles[i].description))
        {
            queued++;
        }
    }

    if (queued > 0)
    {
        Serial.printf("📥 Queued %d missing audio file(s) for download\n", queued);
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
        Serial.println("⚠️ WiFi not connected, skipping download queue processing");
        return false;
    }
    
    if (!initializeSDCard())
    {
        Serial.println("⚠️ SD card not available, skipping download queue processing");
        return false;
    }
    
    AudioDownloadItem* item = &downloadQueue[downloadQueueIndex];
    
    if (item->inProgress)
    {
        return false; // Already processing this item
    }
    
    Serial.printf("📥 Downloading audio file: %s\n", item->description);
    Serial.printf("    URL: %s\n", item->url);
    Serial.printf("    Local: %s\n", item->localPath);
    
    item->inProgress = true;
    
    // Ensure audio directory exists (handles nested paths like /sdcard/audio)
    if (!ensureAudioDirExists())
    {
        Serial.println("❌ Failed to ensure audio directory exists");
        item->inProgress = false;
        downloadQueueIndex++; // Skip this item
        return false;
    }
    
    // Download the file
    HTTPClient http;
    http.begin(item->url);
    http.addHeader("User-Agent", USER_AGENT_HEADER);
    
    int httpCode = http.GET();
    
    if (httpCode == 200)
    {
        // Get content length for progress tracking
        int contentLength = http.getSize();
        
        // Create file for writing
        File audioFile = SD_OPEN(item->localPath, FILE_WRITE);
        if (!audioFile)
        {
            Serial.printf("❌ Failed to create file: %s\n", item->localPath);
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
        Serial.printf("✅ Downloaded %d bytes to: %s\n", totalBytes, item->localPath);
    }
    else
    {
        Serial.printf("❌ HTTP download failed: %d for %s\n", httpCode, item->url);
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
    
    if (!initializeSDCard())
    {
        Serial.println("⚠️ Cannot check cache age without SD card");
        return false; // Assume cache is valid if we can't check
    }
    
    // Read cache timestamp from file
    File timestampFile = SD_OPEN(CACHE_TIMESTAMP_FILE, FILE_READ);
    if (!timestampFile)
    {
        Serial.println("ℹ️ No cache timestamp file found");
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
    Serial.println("💾 Saving audio files to SD card...");
    
    if (!initializeSDCard())
    {
        Serial.println("❌ SD card not available for writing");
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
    }
    
    // Open file for writing
    File audioJsonFile = SD_OPEN(AUDIO_JSON_FILE, FILE_WRITE);
    if (!audioJsonFile)
    {
        Serial.println("❌ Failed to open audio files JSON for writing");
        return false;
    }
    
    // Write JSON to file
    size_t bytesWritten = serializeJson(doc, audioJsonFile);
    audioJsonFile.close();
    
    if (bytesWritten == 0)
    {
        Serial.println("❌ Failed to write audio files to JSON");
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
        Serial.println("⚠️ Failed to save cache timestamp");
    }
    
    Serial.printf("✅ Saved %d audio files to SD card (%d bytes)\n", 
                 audioFileCount, bytesWritten);
    
    return true;
}

/**
 * @brief Load audio files from SD card
 * @return true if successful, false otherwise
 */
static bool loadAudioFilesFromSDCard()
{
    Serial.println("📖 Loading audio files from SD card...");
    
    if (!initializeSDCard())
    {
        Serial.println("❌ SD card not available for reading");
        return false;
    }
    
    // Check if audio files JSON exists
    if (!SD_EXISTS(AUDIO_JSON_FILE))
    {
        Serial.println("ℹ️ No cached audio files found on SD card");
        return false;
    }
    
    // Open audio files JSON
    File audioJsonFile = SD_OPEN(AUDIO_JSON_FILE, FILE_READ);
    if (!audioJsonFile)
    {
        Serial.println("❌ Failed to open audio files JSON for reading");
        return false;
    }
    
    // Read file content
    String jsonString = audioJsonFile.readString();
    audioJsonFile.close();
    
    if (jsonString.length() == 0)
    {
        Serial.println("❌ Empty audio files JSON on SD card");
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
        Serial.println("⚠️ No cache timestamp found");
    }
    
    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    
    if (error)
    {
        Serial.printf("❌ JSON parse error: %s\n", error.c_str());
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
            Serial.println("⚠️ Maximum audio files limit reached");
            break;
        }
        
        const char* key = kv.key().c_str();
        JsonObject entryData = kv.value().as<JsonObject>();
        
        // Allocate and copy strings
        audioFiles[audioFileCount].audioKey = strdup(key);
        audioFiles[audioFileCount].description = strdup(entryData["description"] | "Unknown");
        audioFiles[audioFileCount].type = strdup(entryData["type"] | "unknown");
        audioFiles[audioFileCount].path = strdup(entryData["path"] | "");
        
        audioFileCount++;
    }
    
    Serial.printf("✅ Loaded %d audio files from SD card\n", audioFileCount);
    return true;
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

void initializeAudioFileManager(int sdCsPin, bool mmcSupport, bool sdAlreadyInitialized)
{
    Serial.println("🔧 Initializing Audio File Manager...");
    
    // Initialize variables
    sdMmmcSupport = mmcSupport;
    sdCardCsPin = sdCsPin;
    audioFileCount = 0;
    lastCacheTime = 0;
    sdCardInitialized = sdAlreadyInitialized;  // Use passed value instead of forcing false
    
    // Try to load from SD card first
    if (loadAudioFilesFromSDCard())
    {
        Serial.println("✅ Audio files loaded from SD card cache");
        
        // Check if cache is stale
        if (isCacheStale())
        {
            Serial.println("⏰ Cache is stale, will refresh when WiFi is available");
        }
        listAudioKeys();
        
        // Queue any missing remote audio files so downloads can start immediately
        enqueueMissingAudioFiles();
    }
    else
    {
        Serial.println("ℹ️ No cached audio files found, will download when WiFi is available");
    }
}

bool downloadAudio()
{
    Serial.println("🌐 Downloading list from server...");
    
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("❌ WiFi not connected, cannot download audio files");
        return false;
    }
    
    // Check if cache is still valid
    if (!isCacheStale())
    {
        Serial.println("✅ Cache is still valid, skipping download");
        return true;
    }
    
    HTTPClient http;
    http.begin(KNOWN_FILES_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", USER_AGENT_HEADER);
    
    Serial.printf("📡 Making GET request to: %s\n", KNOWN_FILES_URL);
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode != 200)
    {
        Serial.printf("❌ HTTP request failed: %d\n", httpResponseCode);
        http.end();
        return false;
    }
    
    String payload = http.getString();
    http.end();
    
    Serial.printf("✅ Received response (%d bytes)\n", payload.length());
    
    if (payload.length() > MAX_HTTP_RESPONSE_SIZE)
    {
        Serial.println("❌ Response too large");
        return false;
    }
    
    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error)
    {
        Serial.printf("❌ JSON parse error: %s\n", error.c_str());
        return false;
    }
    
    // Clear existing audio files (free memory first)
    for (int i = 0; i < audioFileCount; i++)
    {
        free((void*)audioFiles[i].audioKey);
        free((void*)audioFiles[i].description);
        free((void*)audioFiles[i].type);
        free((void*)audioFiles[i].path);
    }
    audioFileCount = 0;
    
    // Load new audio files
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root)
    {
        if (audioFileCount >= MAX_KNOWN_SEQUENCES)
        {
            Serial.println("⚠️ Maximum audio files limit reached");
            break;
        }
        
        const char* key = kv.key().c_str();
        JsonObject entryData = kv.value().as<JsonObject>();
        
        // Allocate and copy strings
        audioFiles[audioFileCount].audioKey = strdup(key);
        audioFiles[audioFileCount].description = strdup(entryData["description"] | "Unknown");
        audioFiles[audioFileCount].type = strdup(entryData["type"] | "unknown");
        audioFiles[audioFileCount].path = strdup(entryData["path"] | "");
        
        Serial.printf("📝 Added: %s -> %s (%s)\n", 
                     audioFiles[audioFileCount].audioKey,
                     audioFiles[audioFileCount].description,
                     audioFiles[audioFileCount].type);
        
        audioFileCount++;
    }
    
    Serial.printf("✅ Downloaded and parsed %d file list entries\n", audioFileCount);
    
    // Save to SD card for caching
    if (saveAudioFilesToSDCard())
    {
        Serial.println("💾 Files list cached to SD card");
    }
    else
    {
        Serial.println("⚠️ Failed to cache files list to SD card");
    }

    // Queue any missing remote audio files now that the list is refreshed
    enqueueMissingAudioFiles();
    
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

const char* processAudioKey(const char *key)
{
    if (!key)
    {
        Serial.println("❌ Invalid audio key pointer");
        return nullptr;
    }
    
    Serial.printf("🔍 Processing audio key: %s\n", key);
    
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
        Serial.printf("❌ Audio key not found: %s\n", key);
        return nullptr;
    }
    
    // Log entry info
    Serial.printf("📋 Audio Entry:\n");
    Serial.printf("   Key: %s\n", found->audioKey);
    Serial.printf("   Description: %s\n", found->description);
    Serial.printf("   Type: %s\n", found->type);
    Serial.printf("   Path: %s\n", found->path);
    
    // Handle different entry types
    if (!found->type)
    {
        Serial.println("❌ Entry type is NULL");
        return nullptr;
    }
    
    if (strcmp(found->type, "audio") == 0)
    {
        Serial.printf("🔊 Processing audio: %s\n", found->description);
        
        if (!found->path || strlen(found->path) == 0)
        {
            Serial.println("❌ No audio path specified");
            return nullptr;
        }
        
        // Check if path is a URL
        if (strncmp(found->path, "http://", 7) == 0 || strncmp(found->path, "https://", 8) == 0)
        {
            // It's a web URL - check for local cached version
            if (audioFileExists(found->path))
            {
                // File exists locally - return path for playback
                static char localPath[128];
                if (getLocalAudioPath(found->path, localPath))
                {
                    Serial.printf("🎵 Audio file found locally: %s\n", localPath);
                    return localPath;
                }
                else
                {
                    Serial.println("❌ Failed to generate local path");
                    return nullptr;
                }
            }
            else
            {
                // File doesn't exist - add to download queue
                Serial.printf("📥 Audio file not cached, adding to download queue\n");
                if (addToDownloadQueue(found->path, found->description))
                {
                    Serial.printf("✅ Added to download queue: %s\n", found->description);
                }
                else
                {
                    Serial.printf("❌ Failed to add to download queue: %s\n", found->description);
                }
                
                Serial.printf("ℹ️ Audio will be available after download\n");
                return nullptr;
            }
        }
        else
        {
            // It's a local path - return for direct playback
            Serial.printf("🎵 Local audio path: %s\n", found->path);
            return found->path;
        }
    }
    else if (strcmp(found->type, "service") == 0)
    {
        Serial.printf("🔧 Service: %s\n", found->description);
        // TODO: Implement service access logic
        return nullptr;
    }
    else if (strcmp(found->type, "shortcut") == 0)
    {
        Serial.printf("⚡ Shortcut: %s\n", found->description);
        // TODO: Implement shortcut execution logic
        return nullptr;
    }
    else if (strcmp(found->type, "url") == 0)
    {
        Serial.printf("🌐 URL: %s\n", found->path ? found->path : "NULL");
        // TODO: Implement URL opening logic
        return nullptr;
    }
    else
    {
        Serial.printf("❓ Unknown type: %s\n", found->type);
        return nullptr;
    }
}

void listAudioKeys()
{
    Serial.printf("📋 Audio Files (%d total):\n", audioFileCount);
    Serial.println("============================================================");
    
    if (audioFileCount == 0)
    {
        Serial.println("   No audio files loaded.");
        Serial.println("   Try downloading with downloadAudio()");
        return;
    }
    
    for (int i = 0; i < audioFileCount; i++)
    {
        Serial.printf("%2d. %s\n", i + 1, audioFiles[i].audioKey);
        Serial.printf("    Description: %s\n", audioFiles[i].description);
        Serial.printf("    Type: %s\n", audioFiles[i].type);
        if (strlen(audioFiles[i].path) > 0)
        {
            Serial.printf("    Path: %s\n", audioFiles[i].path);
        }
        Serial.println();
    }
}

int getAudioKeyCount()
{
    return audioFileCount;
}

void clearAudioKeys()
{
    Serial.println("🗑️ Clearing audio files...");
    
    // Free allocated memory
    for (int i = 0; i < audioFileCount; i++)
    {
        free((void*)audioFiles[i].audioKey);
        free((void*)audioFiles[i].description);
        free((void*)audioFiles[i].type);
        free((void*)audioFiles[i].path);
    }
    
    int clearedCount = audioFileCount;
    audioFileCount = 0;
    lastCacheTime = 0;
    
    // Clear SD card cache files
    if (initializeSDCard())
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
            Serial.println("✅ Cleared SD card cache files");
        }
        else
        {
            Serial.println("⚠️ Some SD card files could not be removed");
        }
    }
    else
    {
        Serial.println("⚠️ SD card not available for cache cleanup");
    }
    
    Serial.printf("✅ Cleared %d audio files from memory\n", clearedCount);
}

// ============================================================================
// DOWNLOAD QUEUE MANAGEMENT FUNCTIONS
// ============================================================================

bool processAudioDownloadQueue()
{
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
    Serial.printf("📥 Audio Download Queue (%d items, %d processed):\n", 
                 downloadQueueCount, downloadQueueIndex);
    Serial.println("========================================================");
    
    if (downloadQueueCount == 0)
    {
        Serial.println("   No items in download queue.");
        return;
    }
    
    for (int i = 0; i < downloadQueueCount; i++)
    {
        AudioDownloadItem* item = &downloadQueue[i];
        const char* status = i < downloadQueueIndex ? "✅ Downloaded" : 
                           item->inProgress ? "🔄 In Progress" : "⏳ Pending";
        
        Serial.printf("%2d. %s %s\n", i + 1, status, item->description);
        Serial.printf("    URL: %s\n", item->url);
        Serial.printf("    Local: %s\n", item->localPath);
        Serial.println();
    }
}

void clearDownloadQueue()
{
    Serial.println("🗑️ Clearing download queue...");
    downloadQueueCount = 0;
    downloadQueueIndex = 0;
    Serial.println("✅ Download queue cleared");
}

bool isDownloadQueueEmpty()
{
    return (downloadQueueIndex >= downloadQueueCount);
}
