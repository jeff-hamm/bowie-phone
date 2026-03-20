/**
 * @file extended_audio_player.cpp
 * @brief Extended Audio Player Implementation
 * 
 * Implements queue-based audio playback with support for multiple stream types
 * and audioKey naming.
 * 
 * @date 2025
 */

#include "extended_audio_player.h"
#if ENABLE_PLAYLIST_FEATURES
#include "audio_playlist_registry.h"
#endif
#include "logging.h"
#include <Preferences.h>
#include <WiFi.h>

// SD card abstraction: use SD_MMC or SPI SD based on compile-time config
#if SD_USE_MMC
  #include <SD_MMC.h>
  #define SD_FS        SD_MMC
#else
  #include <SD.h>
  #define SD_FS        SD
#endif

using namespace audio_tools;

// ============================================================================
// CONFIGURATION
// ============================================================================

// Uncomment to disable dial tone (for DTMF detection testing)
//#define DISABLE_DIAL_TONE

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static ExtendedAudioPlayer* globalPlayer = nullptr;

ExtendedAudioPlayer& getExtendedAudioPlayer() {
    if (!globalPlayer) {
        globalPlayer = new ExtendedAudioPlayer();
    }
    return *globalPlayer;
}

#if ENABLE_PLAYLIST_FEATURES
// Registry references (initialized on first use)
static AudioPlaylistRegistry& playlistRegistry = getAudioPlaylistRegistry();
#endif

// ============================================================================
// EXTENDED AUDIO SOURCE IMPLEMENTATION
// ============================================================================

ExtendedAudioSource::ExtendedAudioSource() {
    // No hardcoded generators - they must be registered
}

ExtendedAudioSource::~ExtendedAudioSource() {
    end();
    if (urlStream) {
        delete urlStream;
        urlStream = nullptr;
    }
    // Note: We don't delete generators - caller retains ownership
}

static const char* extensionToMime(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return nullptr;
    if (strcasecmp(dot, ".mp3") == 0)  return "audio/mpeg";
    if (strcasecmp(dot, ".wav") == 0)  return "audio/wav";
    if (strcasecmp(dot, ".m4a") == 0)  return "audio/m4a";
    if (strcasecmp(dot, ".aac") == 0)  return "audio/aac";
    if (strcasecmp(dot, ".ogg") == 0)  return "audio/ogg";
    if (strcasecmp(dot, ".flac") == 0) return "audio/flac";
    return nullptr;
}

const char* ExtendedAudioSource::mime() {
    if (currentType == AudioStreamType::GENERATOR) return "audio/pcm";
    // For file streams, prefer magic-bytes detection over extension
    if (currentType == AudioStreamType::FILE_STREAM && detectedFileMime[0] != '\0') {
        return detectedFileMime;
    }
    if (currentKey[0] != '\0') {
        const char* m = extensionToMime(currentKey);
        if (m) return m;
    }
    return nullptr;
}

bool ExtendedAudioSource::begin() {
    Logger.println("🔧 ExtendedAudioSource::begin()");
    return true;
}

void ExtendedAudioSource::end() {
    closeCurrentStream();
}

void ExtendedAudioSource::closeCurrentStream() {
    detectedFileMime[0] = '\0';
    switch (currentType) {
        case AudioStreamType::URL_STREAM:
            if (urlStream) {
                urlStream->end();
            }
            break;
        case AudioStreamType::FILE_STREAM:
            if (currentFile) {
                currentFile.close();
            }
            break;
        case AudioStreamType::GENERATOR:
            generatorStream.end();
            break;
        default:
            break;
    }
    currentType = AudioStreamType::NONE;
    currentKey[0] = '\0';
}

bool ExtendedAudioSource::hasGenerator(const char* name) const {
    if (!name || !registry) return false;
    return registry->hasGenerator(name);
}

void ExtendedAudioSource::initURLStreaming(int bufferSize) {
    urlBufferSize = bufferSize;
    if (!urlStream) {
        urlStream = new URLStream(bufferSize);
        // Configure URLStream to look like a browser
        urlStream->httpRequest().header().put("User-Agent", 
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        urlStream->httpRequest().header().put("Accept", "*/*");
    }
}

Stream* ExtendedAudioSource::selectStream(int index) {
    // Not used in our implementation - we use selectStream(path) instead
    currentIndex = index;
    return nullptr;
}

Stream* ExtendedAudioSource::selectStream(const char* path) {
    if (!path) return nullptr;
    
    Logger.printf("📂 ExtendedAudioSource::selectStream(%s)\n", path);
    
    // Close any existing stream
    closeCurrentStream();
    
    // Determine stream type from path
    if (strncmp(path, "gen://", 6) == 0) {
        // Generator stream (e.g., "gen://dialtone", "gen://ringback")
        return setGeneratorStream(path + 6) ? &generatorStream : nullptr;
    }
    else if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        // URL stream
        return setURLStream(path) ? urlStream : nullptr;
    }
    else {
        // File stream
        return setFileStream(path) ? &currentFile : nullptr;
    }
}

Stream* ExtendedAudioSource::nextStream(int offset) {
    // We don't auto-advance - the ExtendedAudioPlayer handles queuing
    return nullptr;
}

Stream* ExtendedAudioSource::previousStream(int offset) {
    // We don't support going back
    return nullptr;
}

bool ExtendedAudioSource::setURLStream(const char* url) {
    if (!url || strlen(url) == 0) {
        Logger.println("❌ Invalid URL");
        return false;
    }
    
    if (!urlStream) {
        initURLStreaming(urlBufferSize);
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Logger.println("❌ Cannot stream: WiFi not connected");
        return false;
    }
    
    Logger.printf("🌐 Opening URL stream: %s\n", url);
    
    // Determine MIME type from URL extension
    const char* mimeType = extensionToMime(url);
    if (!mimeType) mimeType = "audio/mpeg";
    
    if (!urlStream->begin(url, mimeType)) {
        Logger.printf("❌ Failed to open URL stream: %s\n", url);
        return false;
    }
    
    currentType = AudioStreamType::URL_STREAM;
    strncpy(currentKey, url, sizeof(currentKey) - 1);
    currentKey[sizeof(currentKey) - 1] = '\0';
    
    Logger.println("✅ URL stream opened");
    return true;
}

bool ExtendedAudioSource::setGeneratorStream(const char* generatorName) {
    if (!generatorName) {
        Logger.println("❌ Invalid generator name");
        return false;
    }
    
    if (!registry) {
        Logger.println("❌ No registry set");
        return false;
    }
    
    // Look up the generator in the registry
    SoundGenerator<int16_t>* generator = registry->getGenerator(generatorName);
    if (!generator) {
        Logger.printf("❌ Generator not registered: %s\n", generatorName);
        return false;
    }
    
    Logger.printf("🎵 Setting up generator: %s\n", generatorName);
    
    AudioInfo info = AUDIO_INFO_DEFAULT();
    generator->begin(info);
    
    generatorStream.setInput(*generator);
    generatorStream.begin(info);
    
    // Verify generator can produce data
    uint8_t testBuf[64];
    size_t testBytes = generatorStream.readBytes(testBuf, sizeof(testBuf));
    Logger.printf("🔬 Generator test read: %d bytes (first 4: %02x %02x %02x %02x)\n",
                  (int)testBytes,
                  testBytes > 0 ? testBuf[0] : 0, testBytes > 1 ? testBuf[1] : 0,
                  testBytes > 2 ? testBuf[2] : 0, testBytes > 3 ? testBuf[3] : 0);
    
    currentType = AudioStreamType::GENERATOR;
    strncpy(currentKey, generatorName, sizeof(currentKey) - 1);
    currentKey[sizeof(currentKey) - 1] = '\0';
    
    Logger.printf("✅ Generator started: %s\n", generatorName);
    return true;
}

const char* ExtendedAudioSource::detectMimeFromFileContent(File& file) {
    if (!file || file.size() < 12) return nullptr;
    
    size_t pos = file.position();
    uint8_t header[12];
    size_t bytesRead = file.read(header, sizeof(header));
    file.seek(pos);  // Restore position
    
    if (bytesRead < 12) return nullptr;
    
    // WAV: "RIFF" at 0, "WAVE" at 8
    if (memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
        return "audio/wav";
    }
    // MP4/M4A: "ftyp" at offset 4
    if (memcmp(header + 4, "ftyp", 4) == 0) {
        return "audio/m4a";
    }
    // OGG: "OggS" at 0
    if (memcmp(header, "OggS", 4) == 0) {
        return "audio/ogg";
    }
    // FLAC: "fLaC" at 0
    if (memcmp(header, "fLaC", 4) == 0) {
        return "audio/flac";
    }
    // MP3: ID3 tag or sync word
    if (memcmp(header, "ID3", 3) == 0 || (header[0] == 0xFF && (header[1] & 0xE0) == 0xE0)) {
        return "audio/mpeg";
    }
    return nullptr;
}

bool ExtendedAudioSource::setFileStream(const char* filePath) {
    if (!filePath) {
        Logger.println("❌ Invalid file path");
        return false;
    }
    
    if (!SD_FS.exists(filePath)) {
        Logger.printf("❌ File not found: %s\n", filePath);
        return false;
    }
    
    Logger.printf("📁 Opening file: %s\n", filePath);
    
    currentFile = SD_FS.open(filePath, FILE_READ);
    if (!currentFile) {
        Logger.printf("❌ Failed to open file: %s\n", filePath);
        return false;
    }
    
    // Detect actual format from magic bytes — don't trust the file extension
    detectedFileMime[0] = '\0';
    const char* detected = detectMimeFromFileContent(currentFile);
    if (detected) {
        strncpy(detectedFileMime, detected, sizeof(detectedFileMime) - 1);
        detectedFileMime[sizeof(detectedFileMime) - 1] = '\0';
        const char* extMime = extensionToMime(filePath);
        if (extMime && strcmp(extMime, detected) != 0) {
            Logger.printf("⚠️ Format mismatch: file '%s' extension says %s but content is %s\n",
                          filePath, extMime, detected);
        }
    }
    
    currentType = AudioStreamType::FILE_STREAM;
    strncpy(currentKey, filePath, sizeof(currentKey) - 1);
    currentKey[sizeof(currentKey) - 1] = '\0';
    
    Logger.printf("✅ File opened: %s (%d bytes)\n", filePath, currentFile.size());
    return true;
}

// ============================================================================
// EXTENDED AUDIO PLAYER IMPLEMENTATION
// ============================================================================

ExtendedAudioPlayer::ExtendedAudioPlayer(int bufferSize) {
    urlStreamBufferSize = bufferSize;
    source = new ExtendedAudioSource();
}

ExtendedAudioPlayer::~ExtendedAudioPlayer() {
    if (player) {
        delete player;
        player = nullptr;
    }
    if (source) {
        delete source;
        source = nullptr;
    }
    if (encodedStream) {
        delete encodedStream;
        encodedStream = nullptr;
    }
    if (streamCopier) {
        delete streamCopier;
        streamCopier = nullptr;
    }
    if (ownedMultiDecoder) {
        delete ownedMultiDecoder;
        ownedMultiDecoder = nullptr;
    }
}

void ExtendedAudioPlayer::begin(AudioStream& outputStream, bool enableStreaming) {
    Logger.printf("🔧 ExtendedAudioPlayer::begin() - %s mode\n", 
                  enableStreaming ? "URL streaming" : "SD card");
    
    streamingEnabled = enableStreaming;
    output = &outputStream;
    
    // Set up volume stream
    volumeStream.setOutput(outputStream);
    loadVolumeFromStorage();
    volumeStream.setVolume(currentVolume);
    
    // Initialize URL streaming if enabled
    if (enableStreaming) {
        source->initURLStreaming(urlStreamBufferSize);
        encodedStream = new EncodedAudioStream(&volumeStream, decoder);
    }
    
    // Create the audio player with our extended source
    player = new AudioPlayer(*source, volumeStream, *decoder);
    
    // Register PCM passthrough decoder for generator streams
    // and set source as MimeSource so MultiDecoder skips auto-detection for generators
    if (isMultiDecoder && ownedMultiDecoder) {
        ownedMultiDecoder->addDecoder(pcmDecoder, "audio/pcm");
        ownedMultiDecoder->setMimeSource(*source);
        Logger.println("🔧 PCM decoder registered + MimeSource wired on MultiDecoder");
    } else {
        Logger.printf("⚠️ MultiDecoder not active (isMulti=%d, owned=%p)\n", 
                      isMultiDecoder, ownedMultiDecoder);
    }
    
    // Configure player
    player->begin(-1, false);  // Don't auto-start
    player->setAutoNext(false);  // We handle advancement via queue
    player->setAutoFade(true);
    
    // Set EOF callback to handle queue advancement
    player->setOnEOFCallback(onEOFCallback);
    
    initialized = true;
    
    Logger.printf("✅ ExtendedAudioPlayer initialized (volume: %.2f)\n", currentVolume);
}

void ExtendedAudioPlayer::onEOFCallback(AudioPlayer& p) {
    Logger.println("📻 Stream ended (EOF callback)");
    
    // Find the global player and trigger queue advancement
    if (globalPlayer) {
        globalPlayer->onStreamEnd();
    }
}

void ExtendedAudioPlayer::onStreamEnd() {
    Logger.println("🔄 onStreamEnd - checking queue");
    
    // Check if there are queued items
    if (!audioQueue.empty()) {
        Logger.printf("📋 Queue has %d items, advancing...\n", audioQueue.size());
        next();
    } else {
        // Play 'click' after real audio ends (not after click/dialtone/off_hook themselves)
        bool shouldClick = currentKey[0] != '\0'
            && strcmp(currentKey, "click") != 0
            && strcmp(currentKey, "dialtone") != 0
            && strcmp(currentKey, "ringback") != 0
            && strcmp(currentKey, "off_hook") != 0
            && strcmp(currentKey, "wrong_number") != 0
            && currentType != AudioStreamType::GENERATOR;
        
        Logger.println("📋 Queue empty, stopping playback");
        stopInternal();
        
        if (shouldClick && registry && registry->hasKey("click")) {
            Logger.println("🔊 Playing click sound");
            playAudioKey("click");
        }
    }
}

bool ExtendedAudioPlayer::playAudio(AudioStreamType type, const char* audioKey, unsigned long durationMs) {
    if (!initialized) {
        Logger.println("❌ Player not initialized");
        return false;
    }
    
    if (!audioKey || strlen(audioKey) == 0) {
        Logger.println("❌ Invalid audioKey");
        return false;
    }
    
    Logger.printf("▶️ playAudio(type=%d, key=%s, duration=%lu) - clearing queue\n", 
                  static_cast<int>(type), audioKey, durationMs);
    
    // Clear queue and stop current playback
    clearQueue();
    if (isPlaying) {
        stopInternal();
    }
    
    // Start immediately
    return startStream(type, audioKey, durationMs);
}

bool ExtendedAudioPlayer::playAudioKey(const char* audioKey, unsigned long durationMs) {
    if (!audioKey) return false;
    
#if ENABLE_PLAYLIST_FEATURES
    // Use playlist if one exists (includes ringback, click, previous/next)
    if (playlistRegistry.hasPlaylist(audioKey)) {
        return playPlaylist(audioKey);
    }
#endif
    
    // Detect stream type from audioKey
    AudioStreamType type = detectStreamType(audioKey);
    
    return playAudio(type, audioKey, durationMs);
}

bool ExtendedAudioPlayer::playPath(const char* path) {
    if (!path) return false;
    
    // Determine stream type from path
    AudioStreamType type = AudioStreamType::FILE_STREAM;
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        type = AudioStreamType::URL_STREAM;
    }
    
    return playAudio(type, path, 0);
}

bool ExtendedAudioPlayer::queueAudio(AudioStreamType type, const char* audioKey, unsigned long durationMs) {
    if (!initialized) {
        Logger.println("❌ Player not initialized");
        return false;
    }
    
    if (!audioKey || strlen(audioKey) == 0) {
        Logger.println("❌ Invalid audioKey");
        return false;
    }
    
    Logger.printf("🎵 queueAudio(type=%d, key=%s, duration=%lu)\n", 
                  static_cast<int>(type), audioKey, durationMs);
    
    // If player is not active, start immediately
    if (!isPlaying) {
        return startStream(type, audioKey, durationMs);
    }
    
    // Player is active - queue this audio
    Logger.printf("📋 Queuing audio: %s\n", audioKey);
    audioQueue.emplace_back(type, audioKey, durationMs);
    Logger.printf("📋 Queue size: %d\n", audioQueue.size());
    
    return true;
}

bool ExtendedAudioPlayer::queueAudioKey(const char* audioKey, unsigned long durationMs) {
    if (!audioKey) return false;
    
    // Detect stream type from audioKey
    AudioStreamType type = detectStreamType(audioKey);
    
    return queueAudio(type, audioKey, durationMs);
}

#if ENABLE_PLAYLIST_FEATURES
bool ExtendedAudioPlayer::playPlaylist(const char* playlistName) {
    if (!playlistName) return false;
    
    const Playlist* playlist = playlistRegistry.getPlaylist(playlistName);
    
    if (!playlist || playlist->empty()) {
        Logger.printf("❌ Playlist not found or empty: %s\n", playlistName);
        return false;
    }
    
    Logger.printf("▶️ Playing playlist: %s (%d items)\n", playlistName, (int)playlist->size());
    
    // Clear queue and stop current playback
    clearQueue();
    if (isPlaying) {
        stopInternal();
    }
    
    // Queue all items from the playlist
    bool first = true;
    for (const auto& node : playlist->nodes) {
        const char* key = node.getAudioKey();
        if (!key || strlen(key) == 0) continue;
        
        // Skip missing keys (like "click" which may not exist)
        if (!hasAudioKey(key)) {
            Logger.printf("⏭️ Skipping missing key in playlist: %s\n", key);
            continue;
        }
        
        AudioStreamType type = detectStreamType(key);
        
        if (first) {
            // Start the first item immediately
            if (startStream(type, key, node.durationMs)) {
                first = false;
            }
        } else {
            // Queue subsequent items
            audioQueue.emplace_back(type, key, node.durationMs);
        }
    }
    
    return !first;  // Return true if we started at least one item
}

bool ExtendedAudioPlayer::queuePlaylist(const char* playlistName) {
    if (!playlistName) return false;
    
    const Playlist* playlist = playlistRegistry.getPlaylist(playlistName);

    
    if (!playlist || playlist->empty()) {
        Logger.printf("❌ Playlist not found or empty: %s\n", playlistName);
        return false;
    }
    
    Logger.printf("📋 Queuing playlist: %s (%d items)\n", playlistName, (int)playlist->size());
    
    // Queue all items from the playlist
    for (const auto& node : playlist->nodes) {
        const char* key = node.getAudioKey();
        if (!key || strlen(key) == 0) continue;
        
        // Skip missing keys (like "click" which may not exist)
        if (!hasAudioKey(key)) {
            Logger.printf("⏭️ Skipping missing key in playlist: %s\n", key);
            continue;
        }
        
        queueAudioKey(key, node.durationMs);
    }
    
    return true;
}
#endif // ENABLE_PLAYLIST_FEATURES

AudioStreamType ExtendedAudioPlayer::detectStreamType(const char* audioKey) const {
    if (!audioKey) return AudioStreamType::NONE;
    
    // Check registry first
    if (registry) {
        return registry->getKeyType(audioKey);
    }
    
    // Fallback checks without registry
    if (strncmp(audioKey, "http://", 7) == 0 || strncmp(audioKey, "https://", 8) == 0) {
        return AudioStreamType::URL_STREAM;
    }
    
    // Assume it's a file path
    return AudioStreamType::FILE_STREAM;
}

const char* ExtendedAudioPlayer::resolveAudioKey(const char* audioKey) const {
    if (!audioKey) return nullptr;
    
    // Use registry if available
    if (registry) {
        // Generators handled separately
        if (registry->hasGenerator(audioKey)) {
            static char genPath[80];
            snprintf(genPath, sizeof(genPath), "gen://%s", audioKey);
            return genPath;
        }
        
        // Try registry resolution
        const char* resolved = registry->resolveKey(audioKey);
        if (resolved) return resolved;
    }
    
    // URLs pass through directly
    if (strncmp(audioKey, "http://", 7) == 0 || strncmp(audioKey, "https://", 8) == 0) {
        return audioKey;
    }
    
    // Assume it's already a file path
    return audioKey;
}

bool ExtendedAudioPlayer::startStream(AudioStreamType type, const char* audioKey, unsigned long durationMs) {
    Logger.printf("▶️ Starting stream: type=%d, key=%s, duration=%lu\n",
                  static_cast<int>(type), audioKey, durationMs);
    
    // Resolve audioKey to actual resource path
    const char* localPath = nullptr;
    const char* streamingPath = nullptr;
    bool tryStreaming = false;
    
    switch (type) {
        case AudioStreamType::GENERATOR:
            // For generators, check if registered and create gen:// URL
            if (registry && registry->hasGenerator(audioKey)) {
#ifdef DISABLE_DIAL_TONE
                if (strcmp(audioKey, "dialtone") == 0) {
                    Logger.println("🎯 Dial tone DISABLED (DISABLE_DIAL_TONE defined)");
                    return false;
                }
#endif
                static char genPath[80];
                snprintf(genPath, sizeof(genPath), "gen://%s", audioKey);
                localPath = genPath;
            } else {
                Logger.printf("❌ Generator not registered: %s\n", audioKey);
                return false;
            }
            break;
            
        case AudioStreamType::URL_STREAM:
            localPath = audioKey;  // URLs pass through directly
            break;
            
        case AudioStreamType::FILE_STREAM:
            // Use registry for resolution
            if (registry) {
                // Get the entry to check for streaming URL
                const KeyEntry* entry = registry->getEntry(audioKey);
                if (entry) {
                    localPath = entry->file->path.c_str();
                    streamingPath = entry->file->alternatePath.c_str();  // May be null
                } else {
                    // Try to resolve via callback
                    localPath = registry->resolveKey(audioKey);
                }
            }
            
            // Fall back to treating as direct path
            if (!localPath) {
                localPath = audioKey;
            }
            break;
            
        default:
            Logger.println("❌ Invalid stream type");
            return false;
    }
    
    if (!localPath) {
        Logger.printf("❌ Failed to resolve audioKey: %s\n", audioKey);
        return false;
    }
    
    Logger.printf("📂 Resolved resource path: %s\n", localPath);
    if (streamingPath) {
        Logger.printf("🌐 Streaming fallback available: %s\n", streamingPath);
    }
    
    // Try to play from local path first
    bool playbackStarted = player->setPath(localPath);
    
    // If local playback failed and streaming is enabled, try streaming URL
    if (!playbackStarted && streamingEnabled && streamingPath) {
        Logger.println("⚠️ Local playback failed, attempting streaming fallback...");
        playbackStarted = player->setPath(streamingPath);
        if (playbackStarted) {
            Logger.println("✅ Streaming fallback successful");
            // Update type to reflect we're actually streaming
            type = AudioStreamType::URL_STREAM;
        }
    }
    
    if (!playbackStarted) {
        Logger.printf("❌ Failed to set path: %s\n", localPath);
        if (streamingPath && !streamingEnabled) {
            Logger.println("💡 Tip: Enable streaming with setStreamingEnabled(true) to use URL fallback");
        }
        return false;
    }
    
    // Store current playback state
    currentType = type;
    strncpy(currentKey, audioKey, sizeof(currentKey) - 1);
    currentKey[sizeof(currentKey) - 1] = '\0';
    currentDurationMs = durationMs;
    playbackStartTime = millis();
    isPlaying = true;
    
    // Start playback
    player->play();
    
    // Reset stall detector for the new stream
    lastNonZeroCopyTime = millis();
    zeroCopyCount = 0;
    
    // Notify callback
    if (eventCallback) {
        eventCallback(true);
    }
    
    Logger.println("✅ Stream started");
    return true;
}

void ExtendedAudioPlayer::stop() {
    Logger.println("⏹️ stop() called");
    
    // Clear the queue
    clearQueue();
    
    // Stop playback
    stopInternal();
}

void ExtendedAudioPlayer::emergencyStop() {
    Logger.println("🚨 emergencyStop() — aborting all audio and resetting state");
    
    // Clear queue first so onStreamEnd() doesn't try to advance
    audioQueue.clear();
    
    // Force-stop the player (may be in a bad state)
    if (player) {
        player->stop();
    }
    if (source) {
        source->end();
    }
    
    // Reset all playback state
    currentType = AudioStreamType::NONE;
    currentKey[0] = '\0';
    currentDurationMs = 0;
    isPlaying = false;
    lastCopyBytes = 0;
    zeroCopyCount = 0;
    lastNonZeroCopyTime = 0;
    
    // Notify callback
    if (eventCallback) {
        eventCallback(false);
    }
}

void ExtendedAudioPlayer::stopInternal() {
    playbackEndTime = millis();  // Record when this audio ended

    if (player) {
        player->stop();
    }
    
    if (source) {
        source->end();
    }
    
    currentType = AudioStreamType::NONE;
    currentKey[0] = '\0';
    currentDurationMs = 0;
    isPlaying = false;
    
    // Notify callback
    if (eventCallback) {
        eventCallback(false);
    }
}

void ExtendedAudioPlayer::clearQueue() {
    Logger.printf("🗑️ Clearing queue (%d items)\n", audioQueue.size());
    audioQueue.clear();
}

bool ExtendedAudioPlayer::isActive() const {
    return isPlaying && player && player->isActive();
}

void ExtendedAudioPlayer::setActive(bool active) {
    if (!active) {
        // Clearing active state clears the queue
        clearQueue();
        stopInternal();
    } else if (player) {
        player->play();
        isPlaying = true;
    }
}

bool ExtendedAudioPlayer::copy() {
    if (!initialized || !player) {
        return false;
    }
    
    // Check duration limit
    if (currentDurationMs > 0 && isPlaying) {
        unsigned long elapsed = millis() - playbackStartTime;
        if (elapsed >= currentDurationMs) {
            Logger.printf("⏱️ Duration limit reached (%lu ms)\n", currentDurationMs);
            onStreamEnd();
            return isPlaying;  // May have advanced to next in queue
        }
    }
    
    // Process audio
    if (player->isActive()) {
        size_t bytesCopied = player->copy();
        lastCopyBytes = bytesCopied;
        
        // Stall detection: if decoder is stuck (e.g. wrong format) it loops
        // consuming input but producing 0 output.  Detect and abort.
        if (bytesCopied > 0) {
            lastNonZeroCopyTime = millis();
            zeroCopyCount = 0;
        } else {
            zeroCopyCount++;
            unsigned long stallMs = millis() - lastNonZeroCopyTime;
            if (lastNonZeroCopyTime > 0 &&
                (stallMs >= COPY_STALL_TIMEOUT_MS || zeroCopyCount >= COPY_STALL_MAX_ZERO)) {
                Logger.printf("🛑 Audio stall detected: 0 bytes for %lu ms (%d calls) on '%s' — aborting\n",
                              stallMs, zeroCopyCount, currentKey);
                emergencyStop();
                return false;
            }
        }
        // Periodic diagnostic for generator streams
        if (source && source->getCurrentStreamType() == AudioStreamType::GENERATOR) {
            static unsigned long lastDiag = 0;
            static unsigned long copyCount = 0;
            static size_t totalBytes = 0;
            copyCount++;
            totalBytes += bytesCopied;
            unsigned long now = millis();
            if (now - lastDiag >= 2000) {
                copyCount = 0;
                totalBytes = 0;
                lastDiag = now;
            }
        }
        return true;
    }
    
    // Player became inactive - might be end of stream
    if (isPlaying) {
        // This will be handled by EOF callback, but check here too
        onStreamEnd();
    }
    
    return isPlaying;
}

bool ExtendedAudioPlayer::next() {
    Logger.println("⏭️ next() called");
    
    // Stop current playback without clearing queue
    if (player) {
        player->stop();
    }
    if (source) {
        source->end();
    }
    
    currentType = AudioStreamType::NONE;
    currentKey[0] = '\0';
    
    // Check queue
    if (audioQueue.empty()) {
        Logger.println("📋 Queue empty, stopping");
        isPlaying = false;
        if (eventCallback) {
            eventCallback(false);
        }
        return false;
    }
    
    // Get next item from queue
    QueuedAudioItem item = audioQueue.front();
    audioQueue.erase(audioQueue.begin());
    
    Logger.printf("📋 Dequeued: %s (remaining: %d)\n", item.audioKey, audioQueue.size());
    
    // Start the dequeued item
    return startStream(item.type, item.audioKey, item.durationMs);
}

bool ExtendedAudioPlayer::isAudioKeyPlaying(const char* audioKey) const {
    if (!isPlaying || !audioKey) return false;
    return strcmp(currentKey, audioKey) == 0;
}

void ExtendedAudioPlayer::stopAudioKey(const char* audioKey) {
    if (isAudioKeyPlaying(audioKey)) {
        // Don't clear queue - just advance to next
        onStreamEnd();
    }
}

const char* ExtendedAudioPlayer::getCurrentAudioKey() const {
    return currentKey[0] != '\0' ? currentKey : nullptr;
}

void ExtendedAudioPlayer::setVolume(float volume) {
    // Clamp to valid range
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    
    currentVolume = volume;
    
    if (player) {
        player->setVolume(volume);
    }
    volumeStream.setVolume(volume);
    
    Logger.printf("🔊 Volume set to %.2f\n", volume);
    
    // Persist to storage
    saveVolumeToStorage();
}

void ExtendedAudioPlayer::loadVolumeFromStorage() {
    Preferences prefs;
    if (!prefs.begin("audio", true)) {  // Read-only
        Logger.println("⚠️ Failed to open volume preferences");
        currentVolume = DEFAULT_AUDIO_VOLUME;
        return;
    }
    
    currentVolume = prefs.getFloat("volume", DEFAULT_AUDIO_VOLUME);
    prefs.end();
    
    // Validate
    if (currentVolume < 0.0f || currentVolume > 1.0f) {
        Logger.printf("⚠️ Invalid stored volume: %.2f, using default\n", currentVolume);
        currentVolume = DEFAULT_AUDIO_VOLUME;
    }
    
    Logger.printf("📖 Loaded volume: %.2f\n", currentVolume);
}

void ExtendedAudioPlayer::saveVolumeToStorage() {
    Preferences prefs;
    if (!prefs.begin("audio", false)) {  // Read-write
        Logger.println("❌ Failed to save volume");
        return;
    }
    
    prefs.putFloat("volume", currentVolume);
    prefs.end();
    
    Logger.printf("💾 Saved volume: %.2f\n", currentVolume);
}

// ============================================================================
// REGISTRY
// ============================================================================

void ExtendedAudioPlayer::setRegistry(AudioKeyRegistry* reg) {
    registry = reg;
    if (source) {
        source->setRegistry(reg);
    }
    Logger.println("🔑 Registry set");
}

bool ExtendedAudioPlayer::hasAudioKey(const char* audioKey) const {
    if (!audioKey) return false;
    
    if (registry) {
        return registry->hasKey(audioKey);
    }
    
    return false;
}

// ============================================================================
// DECODER MANAGEMENT
// ============================================================================

void ExtendedAudioPlayer::addDecoder(AudioDecoder& newDecoder, const char* mime,
                                     bool (*check)(uint8_t* data, size_t len)) {
    // First ensure the decoder is registered (this may create the MultiDecoder)
    addDecoder(newDecoder, mime);

    // Now wire the custom detection function so this MIME is active
    if (isMultiDecoder && ownedMultiDecoder) {
        ownedMultiDecoder->mimeDetector().setCheck(mime, check);
        Logger.printf("🎵 Activated custom MIME detection for %s\n", mime);
    }
}

void ExtendedAudioPlayer::addDecoder(AudioDecoder& newDecoder, const char* mime) {
    if (!decoder) {
        // No decoder yet - set this as the decoder
        decoder = &newDecoder;
        isMultiDecoder = false;
        // Store MIME type so we can re-add it when converting to MultiDecoder
        strncpy(firstDecoderMime, mime, sizeof(firstDecoderMime) - 1);
        firstDecoderMime[sizeof(firstDecoderMime) - 1] = '\0';
        Logger.printf("🎵 Added decoder for %s (first decoder)\n", mime);
        return;
    }
    
    // Check if current decoder is already a MultiDecoder
    if (isMultiDecoder && ownedMultiDecoder) {
        // Already a MultiDecoder - just add to it
        ownedMultiDecoder->addDecoder(newDecoder, mime);
        Logger.printf("🎵 Added decoder for %s (to existing MultiDecoder)\n", mime);
        return;
    }
    
    // Current decoder is not a MultiDecoder - need to wrap it
    // Create a new MultiDecoder and re-add the first decoder
    ownedMultiDecoder = new MultiDecoder();
    
    // Re-add the existing decoder with its stored MIME type
    if (firstDecoderMime[0] != '\0') {
        ownedMultiDecoder->addDecoder(*decoder, firstDecoderMime);
        Logger.printf("🎵 Re-added first decoder for %s to MultiDecoder\n", firstDecoderMime);
    }
    
    // Add the new decoder
    ownedMultiDecoder->addDecoder(newDecoder, mime);
    
    // Update our decoder pointer to the MultiDecoder
    decoder = ownedMultiDecoder;
    isMultiDecoder = true;
    if(player)
        player->setDecoder(*decoder);
    if(encodedStream)
        encodedStream->setDecoder(decoder);
    Logger.printf("🎵 Created MultiDecoder and added decoder for %s\n", mime);
}
