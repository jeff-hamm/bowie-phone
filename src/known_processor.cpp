/**
 * @file known_processor.cpp
 # Default URL for known sequences (can be configured via build flags)
#ifndef KNOWN_SEQUENCES_URL
#define KNOWN_SEQUENCES_URL "https://raw.githubusercontent.com/jeff-hamm/bowie-phone/main/sample-sequence.json"
#endifrief Known Sequence Processor Implementation
 * 
 * This file implements remote sequence downloading, caching, and processing
 * functionality for known DTMF sequences retrieved from a remote server.
 * 
 * @author Bowie Phone Project
 * @date 2025
 */

#include "known_processor.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <FS.h>

// ============================================================================
// CONSTANTS AND CONFIGURATION
// ============================================================================

#define KNOWN_SEQUENCES_FILE "/known_sequences.json"
#define CACHE_TIMESTAMP_FILE "/known_cache_time.txt"
#define CACHE_VALIDITY_HOURS 24 ///< Cache validity in hours
#define MAX_KNOWN_SEQUENCES 50  ///< Maximum number of known sequences
#define MAX_HTTP_RESPONSE_SIZE 8192 ///< Maximum HTTP response size
#define AUDIO_FILES_DIR "/audio"   ///< Directory for cached audio files
#define MAX_DOWNLOAD_QUEUE 20      ///< Maximum items in download queue
#define MAX_FILENAME_LENGTH 64     ///< Maximum length for generated filenames

// SD card chip select pin (configurable via build flags)
#ifndef SD_CS_PIN
#define SD_CS_PIN 5 ///< Default SD card chip select pin
#endif

// Default URL for known sequences (can be configured via build flags)
#ifndef KNOWN_SEQUENCES_URL
#define KNOWN_SEQUENCES_URL "https://github.com/jeff-hamm/bowie-phone/sample-sequence.json"
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
    bool inProgress;        ///< Whether download is currently in progress
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static KnownSequence knownSequences[MAX_KNOWN_SEQUENCES];
static int knownSequenceCount = 0;
static unsigned long lastCacheTime = 0;
static bool sdCardInitialized = false;

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
    
    if (!SD.begin(SD_CS_PIN))
    {
        Serial.println("❌ SD card initialization failed");
        return false;
    }
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE)
    {
        Serial.println("❌ No SD card attached");
        return false;
    }
    
    Serial.printf("✅ SD card initialized (Type: %s)\n", 
                 cardType == CARD_MMC ? "MMC" : 
                 cardType == CARD_SD ? "SDSC" : 
                 cardType == CARD_SDHC ? "SDHC" : "Unknown");
    
    sdCardInitialized = true;
    return true;
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
    
    snprintf(filename, MAX_FILENAME_LENGTH, "audio_%08lx.mp3", hash);
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
    
    return SD.exists(localPath);
}

/**
 * @brief Add audio file to download queue
 * @param url URL to download
 * @param description Description for logging
 * @return true if added successfully, false otherwise
 */
static bool addToDownloadQueue(const char* url, const char* description)
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
    AudioDownloadItem* item = &downloadQueue[downloadQueueCount];
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
 * @brief Download next item in queue (non-blocking)
 * @return true if download started or completed, false if error or queue empty
 */
static bool processDownloadQueue()
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
    
    // Ensure audio directory exists
    if (!SD.exists(AUDIO_FILES_DIR))
    {
        if (!SD.mkdir(AUDIO_FILES_DIR))
        {
            Serial.println("❌ Failed to create audio directory");
            item->inProgress = false;
            downloadQueueIndex++; // Skip this item
            return false;
        }
    }
    
    // Download the file
    HTTPClient http;
    http.begin(item->url);
    http.addHeader("User-Agent", "BowiePhone/1.0");
    
    int httpCode = http.GET();
    
    if (httpCode == 200)
    {
        // Get content length for progress tracking
        int contentLength = http.getSize();
        
        // Create file for writing
        File audioFile = SD.open(item->localPath, FILE_WRITE);
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
    if (knownSequenceCount == 0)
    {
        return true;
    }
    
    if (!initializeSDCard())
    {
        Serial.println("⚠️ Cannot check cache age without SD card");
        return false; // Assume cache is valid if we can't check
    }
    
    // Read cache timestamp from file
    File timestampFile = SD.open(CACHE_TIMESTAMP_FILE, FILE_READ);
    if (!timestampFile)
    {
        Serial.println("ℹ️ No cache timestamp file found");
        return true; // No timestamp file means stale cache
    }
    
    String timestampStr = timestampFile.readString();
    timestampFile.close();
    
    unsigned long savedTime = timestampStr.toInt();
    unsigned long currentTime = millis();
    unsigned long cacheAge = currentTime - savedTime;
    unsigned long maxAge = CACHE_VALIDITY_HOURS * 60 * 60 * 1000UL; // Convert to milliseconds
    
    return (cacheAge > maxAge);
}

/**
 * @brief Save known sequences to SD card
 * @return true if successful, false otherwise
 */
static bool saveKnownSequencesToSDCard()
{
    Serial.println("💾 Saving known sequences to SD card...");
    
    if (!initializeSDCard())
    {
        Serial.println("❌ SD card not available for writing");
        return false;
    }
    
    // Create JSON document for storage
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    
    for (int i = 0; i < knownSequenceCount; i++)
    {
        JsonObject seq = root[knownSequences[i].sequence].to<JsonObject>();
        seq["description"] = knownSequences[i].description;
        seq["type"] = knownSequences[i].type;
        seq["path"] = knownSequences[i].path;
    }
    
    // Open file for writing
    File sequenceFile = SD.open(KNOWN_SEQUENCES_FILE, FILE_WRITE);
    if (!sequenceFile)
    {
        Serial.println("❌ Failed to open sequences file for writing");
        return false;
    }
    
    // Write JSON to file
    size_t bytesWritten = serializeJson(doc, sequenceFile);
    sequenceFile.close();
    
    if (bytesWritten == 0)
    {
        Serial.println("❌ Failed to write sequences to file");
        return false;
    }
    
    // Save timestamp to separate file
    File timestampFile = SD.open(CACHE_TIMESTAMP_FILE, FILE_WRITE);
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
    
    Serial.printf("✅ Saved %d known sequences to SD card (%d bytes)\n", 
                 knownSequenceCount, bytesWritten);
    
    return true;
}

/**
 * @brief Load known sequences from SD card
 * @return true if successful, false otherwise
 */
static bool loadKnownSequencesFromSDCard()
{
    Serial.println("📖 Loading known sequences from SD card...");
    
    if (!initializeSDCard())
    {
        Serial.println("❌ SD card not available for reading");
        return false;
    }
    
    // Check if sequences file exists
    if (!SD.exists(KNOWN_SEQUENCES_FILE))
    {
        Serial.println("ℹ️ No cached sequences found on SD card");
        return false;
    }
    
    // Open sequences file
    File sequenceFile = SD.open(KNOWN_SEQUENCES_FILE, FILE_READ);
    if (!sequenceFile)
    {
        Serial.println("❌ Failed to open sequences file for reading");
        return false;
    }
    
    // Read file content
    String jsonString = sequenceFile.readString();
    sequenceFile.close();
    
    if (jsonString.length() == 0)
    {
        Serial.println("❌ Empty sequences file on SD card");
        return false;
    }
    
    // Load cache timestamp
    File timestampFile = SD.open(CACHE_TIMESTAMP_FILE, FILE_READ);
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
    
    // Clear existing sequences
    knownSequenceCount = 0;
    
    // Load sequences from JSON
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root)
    {
        if (knownSequenceCount >= MAX_KNOWN_SEQUENCES)
        {
            Serial.println("⚠️ Maximum known sequences limit reached");
            break;
        }
        
        const char* sequence = kv.key().c_str();
        JsonObject seqData = kv.value().as<JsonObject>();
        
        // Allocate and copy strings
        knownSequences[knownSequenceCount].sequence = strdup(sequence);
        knownSequences[knownSequenceCount].description = strdup(seqData["description"] | "Unknown");
        knownSequences[knownSequenceCount].type = strdup(seqData["type"] | "unknown");
        knownSequences[knownSequenceCount].path = strdup(seqData["path"] | "");
        
        knownSequenceCount++;
    }
    
    Serial.printf("✅ Loaded %d known sequences from SD card\n", knownSequenceCount);
    return true;
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

void initializeKnownProcessor()
{
    Serial.println("🔧 Initializing Known Sequence Processor...");
    
    // Initialize variables
    knownSequenceCount = 0;
    lastCacheTime = 0;
    sdCardInitialized = false;
    
    // Try to load from SD card first
    if (loadKnownSequencesFromSDCard())
    {
        Serial.println("✅ Known sequences loaded from SD card cache");
        
        // Check if cache is stale
        if (isCacheStale())
        {
            Serial.println("⏰ Cache is stale, will refresh when WiFi is available");
        }
    }
    else
    {
        Serial.println("ℹ️ No cached sequences found, will download when WiFi is available");
    }
}

bool downloadKnownSequences()
{
    Serial.println("🌐 Downloading known sequences from server...");
    
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("❌ WiFi not connected, cannot download sequences");
        return false;
    }
    
    // Check if cache is still valid
    if (!isCacheStale())
    {
        Serial.println("✅ Cache is still valid, skipping download");
        return true;
    }
    
    HTTPClient http;
    http.begin(KNOWN_SEQUENCES_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "BowiePhone/1.0");
    
    Serial.printf("📡 Making GET request to: %s\n", KNOWN_SEQUENCES_URL);
    
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
    
    // Clear existing sequences (free memory first)
    for (int i = 0; i < knownSequenceCount; i++)
    {
        free((void*)knownSequences[i].sequence);
        free((void*)knownSequences[i].description);
        free((void*)knownSequences[i].type);
        free((void*)knownSequences[i].path);
    }
    knownSequenceCount = 0;
    
    // Load new sequences
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root)
    {
        if (knownSequenceCount >= MAX_KNOWN_SEQUENCES)
        {
            Serial.println("⚠️ Maximum known sequences limit reached");
            break;
        }
        
        const char* sequence = kv.key().c_str();
        JsonObject seqData = kv.value().as<JsonObject>();
        
        // Allocate and copy strings
        knownSequences[knownSequenceCount].sequence = strdup(sequence);
        knownSequences[knownSequenceCount].description = strdup(seqData["description"] | "Unknown");
        knownSequences[knownSequenceCount].type = strdup(seqData["type"] | "unknown");
        knownSequences[knownSequenceCount].path = strdup(seqData["path"] | "");
        
        Serial.printf("📝 Added sequence: %s -> %s (%s)\n", 
                     knownSequences[knownSequenceCount].sequence,
                     knownSequences[knownSequenceCount].description,
                     knownSequences[knownSequenceCount].type);
        
        knownSequenceCount++;
    }
    
    Serial.printf("✅ Downloaded and parsed %d known sequences\n", knownSequenceCount);
    
    // Save to SD card for caching
    if (saveKnownSequencesToSDCard())
    {
        Serial.println("💾 Sequences cached to SD card");
    }
    else
    {
        Serial.println("⚠️ Failed to cache sequences to SD card");
    }
    
    return true;
}

bool isKnownSequence(const char *sequence)
{
    if (!sequence || knownSequenceCount == 0)
    {
        return false;
    }
    
    for (int i = 0; i < knownSequenceCount; i++)
    {
        if (strcmp(knownSequences[i].sequence, sequence) == 0)
        {
            return true;
        }
    }
    
    return false;
}

const char* processKnownSequence(const char *sequence)
{
    if (!sequence)
    {
        Serial.println("❌ Invalid sequence pointer");
        return nullptr;
    }
    
    Serial.printf("🔍 Processing known sequence: %s\n", sequence);
    
    // Find the sequence
    KnownSequence *found = nullptr;
    for (int i = 0; i < knownSequenceCount; i++)
    {
        if (strcmp(knownSequences[i].sequence, sequence) == 0)
        {
            found = &knownSequences[i];
            break;
        }
    }
    
    if (!found)
    {
        Serial.printf("❌ Sequence not found in known sequences: %s\n", sequence);
        return nullptr;
    }
    
    // Process based on type
    Serial.printf("📋 Sequence Info:\n");
    Serial.printf("   Sequence: %s\n", found->sequence);
    Serial.printf("   Description: %s\n", found->description);
    Serial.printf("   Type: %s\n", found->type);
    Serial.printf("   Path: %s\n", found->path);
    
    // Handle different sequence types
    if (strcmp(found->type, "audio") == 0)
    {
        Serial.printf("🔊 Processing audio sequence: %s\n", found->description);
        
        if (strlen(found->path) == 0)
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
                
                // For now, we could stream it or skip playback
                Serial.printf("ℹ️ Audio will be available for local playback after download\n");
                return nullptr;
            }
        }
        else
        {
            // It's a local path - return for direct playback
            Serial.printf("🎵 Local audio path found: %s\n", found->path);
            return found->path;
        }
    }
    else if (strcmp(found->type, "service") == 0)
    {
        Serial.printf("🔧 Accessing service: %s\n", found->description);
        // TODO: Implement service access logic
        return nullptr;
    }
    else if (strcmp(found->type, "shortcut") == 0)
    {
        Serial.printf("⚡ Executing shortcut: %s\n", found->description);
        // TODO: Implement shortcut execution logic
        return nullptr;
    }
    else if (strcmp(found->type, "url") == 0)
    {
        Serial.printf("🌐 Opening URL: %s\n", found->path);
        // TODO: Implement URL opening logic
        return nullptr;
    }
    else
    {
        Serial.printf("❓ Unknown sequence type: %s\n", found->type);
        return nullptr;
    }
}

void listKnownSequences()
{
    Serial.printf("📋 Known Sequences (%d total):\n", knownSequenceCount);
    Serial.println("============================================================");
    
    if (knownSequenceCount == 0)
    {
        Serial.println("   No known sequences loaded.");
        Serial.println("   Try downloading with downloadKnownSequences()");
        return;
    }
    
    for (int i = 0; i < knownSequenceCount; i++)
    {
        Serial.printf("%2d. %s\n", i + 1, knownSequences[i].sequence);
        Serial.printf("    Description: %s\n", knownSequences[i].description);
        Serial.printf("    Type: %s\n", knownSequences[i].type);
        if (strlen(knownSequences[i].path) > 0)
        {
            Serial.printf("    Path: %s\n", knownSequences[i].path);
        }
        Serial.println();
    }
}

int getKnownSequenceCount()
{
    return knownSequenceCount;
}

void clearKnownSequences()
{
    Serial.println("🗑️ Clearing known sequences...");
    
    // Free allocated memory
    for (int i = 0; i < knownSequenceCount; i++)
    {
        free((void*)knownSequences[i].sequence);
        free((void*)knownSequences[i].description);
        free((void*)knownSequences[i].type);
        free((void*)knownSequences[i].path);
    }
    
    int clearedCount = knownSequenceCount;
    knownSequenceCount = 0;
    lastCacheTime = 0;
    
    // Clear SD card cache files
    if (initializeSDCard())
    {
        bool sequencesRemoved = false;
        bool timestampRemoved = false;
        
        if (SD.exists(KNOWN_SEQUENCES_FILE))
        {
            sequencesRemoved = SD.remove(KNOWN_SEQUENCES_FILE);
        }
        else
        {
            sequencesRemoved = true; // File doesn't exist, consider it "removed"
        }
        
        if (SD.exists(CACHE_TIMESTAMP_FILE))
        {
            timestampRemoved = SD.remove(CACHE_TIMESTAMP_FILE);
        }
        else
        {
            timestampRemoved = true; // File doesn't exist, consider it "removed"
        }
        
        if (sequencesRemoved && timestampRemoved)
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
    
    Serial.printf("✅ Cleared %d known sequences from memory\n", clearedCount);
}

// ============================================================================
// DOWNLOAD QUEUE MANAGEMENT FUNCTIONS
// ============================================================================

bool processAudioDownloadQueue()
{
    return processDownloadQueue();
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