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
#include "web_queue.h"
#include "extended_audio_player.h"
#include "audio_key_registry.h"
#if ENABLE_PLAYLIST_FEATURES
#include "audio_playlist_registry.h"
#endif
#include "tone_generators.h"
#include "file_utils.h"
#include "logging.h"
#include <WiFi.h>
#include "http_utils.h"
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



// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

//static AudioEntry audioEntries[MAX_AUDIO_FILES];
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

// Registry mutex — kept for download queue re-registration serialisation
static SemaphoreHandle_t registryMutex = nullptr;

// True while a catalog download is in flight (prevents duplicate enqueue)
static bool catalogDownloadPending = false;

static AudioKeyRegistry &audioKeyRegistry = AudioKeyRegistry::instance;
#if ENABLE_PLAYLIST_FEATURES
    // Registry references (initialized on first use)
static AudioPlaylistRegistry &playlistRegistry = getAudioPlaylistRegistry();
#endif

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
 * @param audioKey Registry key for this file (used for logging and re-registration)
 * @param ext File extension hint (e.g., "wav", "mp3") — may be corrected by Content-Type
 * @return true if added successfully, false otherwise
 */
static bool addToDownloadQueue(const char* url, const char* audioKey, const char* ext = nullptr)
{
    char localPath[128];
    if (!getLocalPathForUrl(url, localPath, ext)) {
        Logger.printf("❌ Failed to generate local path for: %s\n", url);
        return false;
    }
    auto result = webQueue.enqueue(audioKey, url, localPath, ext);
    return result == WebQueue::EnqueueResult::OK ||
           result == WebQueue::EnqueueResult::ALREADY_QUEUED;
}

/**
 * @brief Queue downloads for any missing HTTP/HTTPS audio files from the registry
 */
static void enqueueMissingAudioFilesFromRegistry()
{
    if (audioKeyRegistry.size() == 0)
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

    for (const auto& pair : audioKeyRegistry)
    {
        const AudioEntry& entry = pair.second;
        
        // Only queue entries that have a streaming URL (means original was a URL)
        FileData* f = entry.getFile();
        if (!f || f->alternatePath.empty())
        {
            continue;
        }
        
        const char* downloadPath = f->alternatePath.c_str();
        const char* ext = f->ext.c_str();
        
        // Check using ext from registry (set after Content-Type detection)
        if (audioFileExists(downloadPath, ext))
        {
            continue; // Already cached
        }
        
        // Also check if the primary local path already exists on disk
        // (covers the case where Content-Type changed the extension and
        //  the registry path was updated but ext field wasn't set yet)
        if (!entry.file->path.empty() && SD_EXISTS(entry.file->path.c_str()))
        {
            continue; // Already cached under the corrected path
        }

        // Check if the file exists with a different extension (e.g., catalog says
        // .wav but a previous download detected Content-Type audio/mp4 and saved
        // as .m4a).  If found, re-register with the actual extension so the player
        // finds it and we don't re-download every boot.
        {
            static const char* knownExts[] = {"wav", "mp3", "m4a", "aac", "ogg", "flac"};
            bool found = false;
            for (int i = 0; i < 6; i++)
            {
                // Skip the extension we already checked above
                if (ext && strcmp(ext, knownExts[i]) == 0) continue;
                if (audioFileExists(downloadPath, knownExts[i]))
                {
                    Logger.printf("🔄 Found cached file for '%s' with ext '%s' (registry had '%s'), re-registering\n",
                                  entry.audioKey.c_str(), knownExts[i], ext ? ext : "(none)");
                    audioKeyRegistry.registerKey(entry.audioKey.c_str(), downloadPath, knownExts[i]);
                    found = true;
                    break;
                }
            }
            if (found) continue;
        }

        if (addToDownloadQueue(downloadPath, entry.audioKey.c_str(), ext)) {
            queued++;
        }
    }

    if (queued > 0)
    {
        Logger.printf("📥 Queued %d missing audio file(s) for download\n", queued);
    }
}

// Bidirectional MIME ↔ extension table.
// First entry per extension is the canonical MIME type returned by extensionToMimeType().
struct MimeExtMapping { const char* mime; const char* ext; };
extern const MimeExtMapping kMimeExtTable[] = {
    {"audio/mpeg",      "mp3"},
    {"audio/mp3",       "mp3"},
    {"audio/wav",       "wav"},
    {"audio/wave",      "wav"},
    {"audio/x-wav",     "wav"},
    {"audio/vnd.wave",  "wav"},
    {"audio/m4a",       "m4a"},
    {"audio/mp4",       "m4a"},
    {"audio/aac",       "aac"},
    {"audio/ogg",       "ogg"},
    {"audio/flac",      "flac"},
};
extern const int kMimeExtTableSize = (int)(sizeof(kMimeExtTable) / sizeof(kMimeExtTable[0]));

/**
 * @brief Map a MIME type string to a file extension (without leading dot)
 * @param contentType Content-Type header value (may include parameters e.g. "audio/mpeg; charset=UTF-8")
 * @return Extension string (e.g. "mp3"), or nullptr if unrecognised
 */
static const char* mimeTypeToExtension(const char* contentType)
{
    if (!contentType || contentType[0] == '\0') return nullptr;
    for (int i = 0; i < kMimeExtTableSize; i++) {
        if (strncmp(contentType, kMimeExtTable[i].mime, strlen(kMimeExtTable[i].mime)) == 0)
            return kMimeExtTable[i].ext;
    }
    return nullptr;
}

/**
 * @brief Map a file extension to its canonical MIME type
 * @param ext Extension without leading dot (e.g. "mp3")
 * @return MIME type string (e.g. "audio/mpeg"), or nullptr if unrecognised
 */
static const char* extensionToMimeType(const char* ext)
{
    if (!ext || ext[0] == '\0') return nullptr;
    for (int i = 0; i < kMimeExtTableSize; i++) {
        if (strcmp(ext, kMimeExtTable[i].ext) == 0)
            return kMimeExtTable[i].mime;
    }
    return nullptr;
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
    
    HttpClient http(HTTP_TIMEOUT_CATALOG_MS);
    
    if (!http.get(checkUrl))
    {
        Logger.printf("⚠️ Cache check failed (HTTP %d) - assuming valid\n", http.statusCode());
        return false; // Can't verify, assume valid
    }
    
    String response = http.getString();
    
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

// ============================================================================
// JSON PARSING HELPERS
// ============================================================================

/**
 * @brief Build a tone generator from JSON parameters
 * 
 * Supports 1–4 simultaneous frequencies. Optionally wraps the generator
 * in a RepeatingToneGenerator for cadence (toneMs/silenceMs).
 * 
 * JSON format:
 *   "frequencies": [440]               → ToneGenerator<1>
 *   "frequencies": [350, 440]          → ToneGenerator<2>
 *   "frequencies": [350, 440, 480]     → ToneGenerator<3>
 *   "frequencies": [350, 440, 480, 620] → ToneGenerator<4>
 *   "amplitude": 16000                  (optional, default 16000)
 *   "toneMs": 2000                      (optional — if set, wraps in RepeatingToneGenerator)
 *   "silenceMs": 3000                   (optional — required if toneMs is set)
 * 
 * @param entryData JSON object containing generator parameters
 * @return Heap-allocated SoundGenerator (caller must manage ownership), or nullptr on error
 */
static SoundGenerator<int16_t>* buildGeneratorFromJson(JsonObject& entryData) {
    JsonArray freqArray = entryData["frequencies"].as<JsonArray>();
    if (!freqArray || freqArray.size() == 0) {
        // Fallback: single frequency field
        float freq = entryData["frequency"] | 440.0f;
        freqArray = JsonArray();  // won't be used
        auto* gen = new ToneGenerator<1>(std::array<float, 1>{freq},
                                         entryData["amplitude"] | 16000.0f);
        unsigned long toneMs = entryData["toneMs"] | 0;
        if (toneMs > 0) {
            unsigned long silenceMs = entryData["silenceMs"] | 0;
            auto* repeating = new RepeatingToneGenerator<int16_t>(
                std::unique_ptr<SoundGenerator<int16_t>>(gen), toneMs, silenceMs);
            return repeating;
        }
        return gen;
    }

    int n = freqArray.size();
    if (n > 4) n = 4;

    float amplitude = entryData["amplitude"] | 16000.0f;
    SoundGenerator<int16_t>* baseGen = nullptr;

    switch (n) {
        case 1: {
            std::array<float, 1> f = { freqArray[0].as<float>() };
            baseGen = new ToneGenerator<1>(f, amplitude);
            break;
        }
        case 2: {
            std::array<float, 2> f = { freqArray[0].as<float>(), freqArray[1].as<float>() };
            baseGen = new ToneGenerator<2>(f, amplitude);
            break;
        }
        case 3: {
            std::array<float, 3> f = { freqArray[0].as<float>(), freqArray[1].as<float>(), freqArray[2].as<float>() };
            baseGen = new ToneGenerator<3>(f, amplitude);
            break;
        }
        case 4: {
            std::array<float, 4> f = { freqArray[0].as<float>(), freqArray[1].as<float>(), freqArray[2].as<float>(), freqArray[3].as<float>() };
            baseGen = new ToneGenerator<4>(f, amplitude);
            break;
        }
        default: 
            return nullptr;
    }

    unsigned long toneMs = entryData["toneMs"] | 0;
    if (toneMs > 0) {
        unsigned long silenceMs = entryData["silenceMs"] | 0;
        // Owning constructor — RepeatingToneGenerator takes ownership of baseGen
        auto* repeating = new RepeatingToneGenerator<int16_t>(
            std::unique_ptr<SoundGenerator<int16_t>>(baseGen), toneMs, silenceMs);
        return repeating;
    }

    return baseGen;
}

/**
 * @brief Parse AudioTiming fields from a JSON object
 */
static AudioTiming parseAudioTiming(JsonObject obj) {
    AudioTiming t;
    t.durationMs = obj["duration"]  | 0;
    t.gapBefore  = obj["gap"]       | obj["gapBefore"] | 0;
    t.gapAfter   = obj["gapAfter"]  | 0;
    t.loop       = obj["loop"]      | 0;
    return t;
}

/**
 * @brief Parse a single AudioLink from a JSON value
 * 
 * Supports two formats:
 *   - Simple string: "audioKey"
 *   - Object with timing: { "key": "audioKey", "duration": 1000, "gap": 500 }
 * 
 * @param value JSON value (string or object)
 * @return Heap-allocated AudioLink, or nullptr if invalid
 */
static AudioLink* parseAudioLink(JsonVariant value) {
    if (value.is<const char*>()) {
        const char* key = value.as<const char*>();
        if (key && strlen(key) > 0) {
            return new AudioLink(key);
        }
    } else if (value.is<JsonObject>()) {
        JsonObject obj = value.as<JsonObject>();
        const char* key = obj["key"] | "";
        if (strlen(key) > 0) {
            return new AudioLink(key, parseAudioTiming(obj));
        }
    }
    return nullptr;
}

/**
 * @brief Case-insensitive comparison of a JSON string value
 */
static bool jsonTypeEquals(const char* value, const char* target) {
    if (!value || !target) return false;
    return strcasecmp(value, target) == 0;
}

/**
 * @brief FNV-1a hash of a serialized JSON object for change detection
 */
static uint32_t hashJsonObject(JsonObject obj) {
    uint32_t h = 2166136261u;
    String s;
    serializeJson(obj, s);
    for (size_t i = 0; i < s.length(); i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/**
 * @brief Callback type for processing audio entries during JSON parsing
 * @param entry The audio entry to process
 * @param userData Optional user data pointer for context
 */
typedef void (*AudioEntryProcessCallback)(const AudioEntry* entry, void* userData);

/**
 * @brief Parse JSON string and process audio entries with a callback
 * @param jsonString JSON string containing audio file entries
 * @param callback Function to call for each audio entry
 * @param userData Optional user data to pass to callback
 * @return Number of entries successfully processed, -1 on parse error
 */
static int parseAndRegisterAudioFiles(const String& jsonString, AudioEntryProcessCallback callback=nullptr, void* userData = nullptr)
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

        // Hash the JSON for change detection — skip reconstruction if unchanged
        uint32_t hash = hashJsonObject(entryData);
        const AudioEntry* existing = audioKeyRegistry.getEntry(key);
        if (existing && existing->contentHash == hash) {
            processedCount++;
            if (callback) callback(existing, userData);
            continue;
        }

        const char* typeStr = entryData["type"] | "audio";

        // Case-insensitive type detection
        AudioStreamType streamType = AudioStreamType::FILE_STREAM;
        if (jsonTypeEquals(typeStr, "generator")) {
            streamType = AudioStreamType::GENERATOR;
        } else if (jsonTypeEquals(typeStr, "url")) {
            streamType = AudioStreamType::URL_STREAM;
        }

        // Build a complete AudioEntry, then register once
        AudioEntry entry;
        entry.audioKey = key;
        entry.type = streamType;
        entry.contentHash = hash;

        // Type-specific payload
        if (streamType == AudioStreamType::GENERATOR) {
            entry.generator = buildGeneratorFromJson(entryData);
            if (!entry.generator)
            {
                Logger.printf("⚠️ Failed to build generator for '%s'\n", key);
                continue;
            }
        } else {
            entry.file = new FileData();
            entry.file->path = entryData["path"] | entryData["data"] | entryData["url"] | "";
            entry.file->ext = entryData["ext"] | entryData["codec"] | entryData["extension"] | "";
            if (entry.file->path.empty()) {
                Logger.printf("⚠️ No path for: %s\n", key);
                continue;
            }
        }

        // Timing metadata
        entry.timing = parseAudioTiming(entryData);

        // AudioLinks
        if (!entryData["previous"].isNull())
            entry.previous = parseAudioLink(entryData["previous"]);
        if (!entryData["next"].isNull())
            entry.next = parseAudioLink(entryData["next"]);

        // Single registration point — moves entry into registry
        audioKeyRegistry.registerEntry(std::move(entry));

        // Re-fetch pointer for callback (entry was moved)
        const AudioEntry* registered = audioKeyRegistry.getEntry(key);
        processedCount++;

        // Invoke callback if provided
        if (callback && registered)
            callback(registered, userData);
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
static bool isCacheStale(int audioFileCount = -1, bool allowRemoteValidation = true)
{
    // Use registry size if count not provided
    if (audioFileCount < 0)
        audioFileCount = audioKeyRegistry.size();
    
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
    
    // TIER 1: Lightweight check if WiFi connected.
    // Always check on the first eligible call after boot, then rate-limit.
    unsigned long timeSinceLastCheck = currentTime - lastCacheCheck;
    bool firstCheckAfterBoot = (lastCacheCheck == 0);
    if (allowRemoteValidation && (firstCheckAfterBoot || timeSinceLastCheck > CACHE_CHECK_INTERVAL_MS) && WiFi.status() == WL_CONNECTED)
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
#if ENABLE_PLAYLIST_FEATURES
    playlistRegistry.resolveAllPlaylists();
#endif
    
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
        // Create registry mutex and wire it into the download queue so that
        // re-registrations from the download task are safe.
        if (!registryMutex) registryMutex = xSemaphoreCreateMutex();
        webQueue.setRegistryMutex(registryMutex);
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
        // Keep boot fast: skip remote validation here and defer it to maintenance loop.
        bool stale = isCacheStale(audioFileCount, false);
        if (stale)
        {
            Logger.println("⏰ Cache is stale, will refresh when WiFi is available");
        }
        else
        {
            // Cache looks fresh locally — force remote validation in the first maintenance loop
            // Do NOT delete SD cache files; they're our fallback if WiFi is unavailable.
            Logger.println("Cache looks fresh locally, will verify remotely in maintenance loop");
            lastCacheCheck = 0;  // triggers lightweight remote check on first loop
        }
        audioKeyRegistry.listKeys();
        
        // Always queue missing audio files immediately from the cached registry.
        // If cache is stale, a catalog refresh will also be enqueued (non-blocking);
        // when it completes its callback will re-queue any newly-discovered files.
        enqueueMissingAudioFilesFromRegistry();
    }
    else
    {
        Logger.println("ℹ️ No cached audio files found, will download when WiFi is available");
    }
    
    return source;
}

// ============================================================================
// CATALOG URL BUILDER
// ============================================================================

/**
 * @brief Build the catalog URL with streaming parameter and optional DNS cache IP.
 * @return Fully-qualified catalog URL ready for HTTP GET.
 */
static String buildCatalogUrl()
{
    String catalogUrl = KNOWN_SEQUENCES_URL;

    // If DNS is pre-cached, try to use the IP instead of hostname
    if (dnsPreCached && cachedGitHubIP != IPAddress(0,0,0,0))
    {
        int protoEnd = catalogUrl.indexOf("://");
        if (protoEnd > 0)
        {
            String protocol = catalogUrl.substring(0, protoEnd + 3);
            int hostStart = protoEnd + 3;
            int hostEnd = catalogUrl.indexOf('/', hostStart);
            if (hostEnd > hostStart)
            {
                String path = catalogUrl.substring(hostEnd);
                catalogUrl = protocol + cachedGitHubIP.toString() + path;
            }
        }
    }

    // Append streaming parameter
    catalogUrl += (catalogUrl.indexOf('?') >= 0) ? "&streaming=" : "?streaming=";
    catalogUrl += sdCardAvailable ? "false" : "true";
    return catalogUrl;
}

// ============================================================================
// CATALOG COMPLETION CALLBACK
// ============================================================================

/**
 * @brief Called by the download queue when a catalog fetch completes.
 *
 * Runs on core 1 (from tick()), so registry access is safe.
 * Parses JSON, registers entries, prunes orphans, saves to SD, and
 * enqueues missing audio files for download.
 */
static void onCatalogDownloaded(bool success, const String& payload, void* /*userData*/)
{
    catalogDownloadPending = false;

    if (!success || payload.length() == 0) {
        Logger.println("❌ Catalog download failed");
        return;
    }

    Logger.printf("✅ Catalog received (%d bytes), parsing...\n", payload.length());

    if (payload.length() > MAX_HTTP_RESPONSE_SIZE) {
        Logger.println("❌ Catalog response too large");
        return;
    }

    // Mark-and-sweep: collect existing non-generator keys
    std::set<std::string> existingKeys;
    for (const auto& pair : audioKeyRegistry) {
        if (pair.second.type != AudioStreamType::GENERATOR)
            existingKeys.insert(pair.first);
    }

    std::set<std::string> seenKeys;

    int registeredCount = parseAndRegisterAudioFiles(payload,
        [](const AudioEntry* entry, void* ud) {
            auto* seen = static_cast<std::set<std::string>*>(ud);
            if (seen) seen->insert(entry->audioKey);
        }, &seenKeys);

    if (registeredCount < 0) return;

    // Prune orphaned keys
    int prunedCount = 0;
    for (const auto& key : existingKeys) {
        if (seenKeys.find(key) == seenKeys.end()) {
            Logger.printf("🗑️ Pruning orphaned key: %s\n", key.c_str());
            audioKeyRegistry.unregisterKey(key.c_str());
            prunedCount++;
        }
    }
    if (prunedCount > 0)
        Logger.printf("✅ Pruned %d orphaned audio keys\n", prunedCount);

#if ENABLE_PLAYLIST_FEATURES
    playlistRegistry.resolveAllPlaylists();
#endif

    Logger.printf("✅ Registered %d audio files%s\n",
                  registeredCount, prunedCount > 0 ? " (pruned orphans)" : "");

    // Save to SD card
    if (sdCardAvailable) {
        File audioJsonFile = SD_OPEN(AUDIO_JSON_FILE, FILE_WRITE);
        if (audioJsonFile) {
            audioJsonFile.print(payload);
            audioJsonFile.close();

            File timestampFile = SD_OPEN(CACHE_TIMESTAMP_FILE, FILE_WRITE);
            if (timestampFile) {
                timestampFile.print(millis());
                timestampFile.close();
                lastCacheTime = millis();
            }
            Logger.println("💾 Audio catalog cached to SD card");
        } else {
            Logger.println("⚠️ Failed to cache audio catalog to SD card");
        }

        // Queue missing audio file downloads
        enqueueMissingAudioFilesFromRegistry();
    }
}

// ============================================================================
// PUBLIC DOWNLOAD API
// ============================================================================

bool downloadAudio(int /*maxRetries*/, unsigned long /*retryDelayMs*/)
{
    if (WiFi.status() != WL_CONNECTED) {
        Logger.println("❌ WiFi not connected, cannot download audio catalog");
        return false;
    }

    if (catalogDownloadPending) {
        Logger.println("ℹ️ Catalog download already in progress");
        return true;
    }

    String url = buildCatalogUrl();
    Logger.printf("📡 Enqueueing catalog download: %s\n", url.c_str());

    auto result = webQueue.enqueueCatalog(url.c_str(), onCatalogDownloaded);
    if (result == WebQueue::EnqueueResult::OK) {
        catalogDownloadPending = true;
        return true;
    }
    if (result == WebQueue::EnqueueResult::ALREADY_QUEUED) {
        catalogDownloadPending = true;
        return true;
    }

    Logger.println("⚠️ Failed to enqueue catalog download (queue full?)");
    return false;
}

void invalidateAudioCache()
{
    Logger.println("🔄 Invalidating audio cache...");
    lastCacheTime = 0;  // Force cache to be considered stale
    lastCacheCheck = 0;
    cachedEtag[0] = '\0';

    // Do NOT delete SD cache files — they're our fallback if the catalog
    // download fails (no WiFi, server down, power loss during refresh).
    // The maintenance loop will enqueue a fresh catalog download which,
    // on success, overwrites the SD cache atomically.

    Logger.println("✅ Cache invalidated - next maintenance loop will refresh");
}

// ============================================================================
// AUDIO MAINTENANCE LOOP
// ============================================================================

void audioMaintenanceLoop()
{
    // 1. Periodic catalog refresh: enqueue catalog download if stale
    //    isCacheStale() is lightweight — only does HTTP when enough time has passed
    static unsigned long lastCatalogCheck = 0;
    unsigned long now = millis();
    if (now - lastCatalogCheck >= CACHE_CHECK_INTERVAL_MS)
    {
        lastCatalogCheck = now;
        // Skip remote cache validation while audio is playing — the HTTP
        // round-trip (up to 10 s) would starve the audio DMA buffer.
        if (WiFi.status() == WL_CONNECTED &&
            !getExtendedAudioPlayer().isActive() &&
            isCacheStale())
        {
            Logger.println("🔄 Cache stale — enqueueing catalog refresh...");
            downloadAudio();  // non-blocking: enqueues catalog download
        }
    }

    // 2. Cooperative download queue: one chunk (~4 KB) per call.
    //    When idle this is rate-limited internally; when streaming it returns
    //    in ~2-4 ms so audio playback copy() isn't starved.
    webQueue.tick();

    // 3. Refill the download page when it drains (and no catalog is pending)
    //    compact() is called inside tick() when >=50% slots are done/failed,
    //    so slots are freed automatically before we try to enqueue here.
    if (!catalogDownloadPending && webQueue.pendingCount() == 0 && !webQueue.isActive())
        enqueueMissingAudioFilesFromRegistry();
}

// ============================================================================
// DOWNLOAD QUEUE MANAGEMENT FUNCTIONS
// ============================================================================

bool processAudioDownloadQueue()
{
    if (!initializeSDCard()) return false;
    return webQueue.tick();
}

int getDownloadQueueCount()     { return webQueue.pendingCount(); }
// int getTotalDownloadQueueSize() { return webQueue.totalCount(); }
// void listDownloadQueue()        { webQueue.listItems(); }
// void clearDownloadQueue()       { webQueue.reset(); }
bool isDownloadQueueEmpty()     { return webQueue.isEmpty(); }

// ============================================================================
// REGISTRY INTEGRATION
// ============================================================================