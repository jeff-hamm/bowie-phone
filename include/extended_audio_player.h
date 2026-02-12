/**
 * @file extended_audio_player.h
 * @brief Extended Audio Player with queue support, multiple stream types, and audioKey naming
 * 
 * Extends the base AudioPlayer to support:
 * - URL streams and synthesized tone generators
 * - Naming streams by audioKey rather than direct paths
 * - Non-file based audio queues with automatic advancement
 * 
 * @date 2025
 */

#pragma once

#include <config.h>
#include "AudioTools.h"
#include "AudioTools/CoreAudio/AudioEffects/SoundGenerator.h"
#include "AudioTools/Communication/HTTP/URLStream.h"
#include "AudioTools/AudioCodecs/MultiDecoder.h"
#include "audio_key_registry.h"
#include "audio_playlist_registry.h"
#include "file_utils.h"
#include <SD.h>
#include <vector>

using namespace audio_tools;

// ============================================================================
// CONFIGURATION
// ============================================================================

#ifndef DEFAULT_AUDIO_VOLUME
#define DEFAULT_AUDIO_VOLUME 0.7f  ///< Default audio volume (0.0 to 1.0)
#endif

#ifndef URL_STREAM_BUFFER_SIZE
#define URL_STREAM_BUFFER_SIZE 2048  ///< Buffer size for URL streaming
#endif

// ============================================================================
// QUEUED AUDIO ITEM
// ============================================================================

/**
 * @brief Represents a queued audio item with all playback parameters
 */
struct QueuedAudioItem {
    AudioStreamType type;
    char audioKey[64];
    unsigned long durationMs;
    
    QueuedAudioItem() : type(AudioStreamType::NONE), durationMs(0) {
        audioKey[0] = '\0';
    }
    
    QueuedAudioItem(AudioStreamType t, const char* key, unsigned long duration)
        : type(t), durationMs(duration) {
        if (key) {
            strncpy(audioKey, key, sizeof(audioKey) - 1);
            audioKey[sizeof(audioKey) - 1] = '\0';
        } else {
            audioKey[0] = '\0';
        }
    }
};

// ============================================================================
// AUDIO EVENT CALLBACK
// ============================================================================

/**
 * @brief Callback type for audio playback events
 * @param isPlaying true when audio starts, false when it stops
 */
typedef void (*AudioEventCallback)(bool isPlaying);

// ============================================================================
// EXTENDED AUDIO SOURCE
// ============================================================================

using namespace audio_tools;

/**
 * @brief Custom AudioSource that handles multiple stream types
 * 
 * This source manages file streams, URL streams, and generated tones,
 * providing a unified interface for the AudioPlayer.
 */
class ExtendedAudioSource : public AudioSource {
public:
    ExtendedAudioSource();
    ~ExtendedAudioSource();
    
    // AudioSource interface
    Stream* selectStream(int index) override;
    Stream* selectStream(const char* path) override;
    Stream* nextStream(int offset) override;
    Stream* previousStream(int offset = 1) override;
    int index() override { return currentIndex; }
    void setTimeoutAutoNext(int millisec) override { timeoutMs = millisec; }
    int timeoutAutoNext() override { return timeoutMs; }
    bool begin() override;
    void end();  // Not in base class, but useful for cleanup
    bool isAutoNext() override { return false; }  // We handle advancement ourselves
    
    // ========================================================================
    // REGISTRY
    // ========================================================================
    
    /**
     * @brief Set the audio key registry to use
     * @param registry Pointer to registry (caller retains ownership)
     */
    void setRegistry(AudioKeyRegistry* registry) { this->registry = registry; }
    
    /**
     * @brief Get the current registry
     */
    AudioKeyRegistry* getRegistry() const { return registry; }
    
    // ========================================================================
    // STREAM CONTROL
    // ========================================================================
    
    bool setURLStream(const char* url);
    bool setGeneratorStream(const char* generatorName);
    bool setFileStream(const char* filePath);
    
    AudioStreamType getCurrentStreamType() const { return currentType; }
    const char* getCurrentKey() const { return currentKey; }
    
    // Initialize URL streaming components
    void initURLStreaming(int bufferSize = URL_STREAM_BUFFER_SIZE);

    // Access to generator stream for direct manipulation
    GeneratedSoundStream<int16_t>& getGeneratorStream() { return generatorStream; }
    
    // Check if a generator is registered
    bool hasGenerator(const char* name) const;
    
protected:
    // Registry reference
    AudioKeyRegistry* registry = nullptr;
    
    // Stream type tracking
    AudioStreamType currentType = AudioStreamType::NONE;
    char currentKey[64] = {0};
    int currentIndex = 0;
    int timeoutMs = 1000;
    
    // Generator stream wrapper
    GeneratedSoundStream<int16_t> generatorStream;
    
    // URL streaming
    URLStream* urlStream = nullptr;
    int urlBufferSize = 2048;
    
    // File streaming (uses SD card)
    File currentFile;
    
    // Helper to close current stream
    void closeCurrentStream();
};

// ============================================================================
// EXTENDED AUDIO PLAYER
// ============================================================================

/**
 * @brief Extended AudioPlayer with queue support and multi-stream-type handling
 * 
 * Features:
 * - Queue-based playback with automatic advancement
 * - Support for audioKey naming (maps to files, URLs, or generators)
 * - Duration limits per audio item
 * - Automatic queue clearing on stop
 */
class ExtendedAudioPlayer {
public:
    ExtendedAudioPlayer(int urlStreamBufferSize = URL_STREAM_BUFFER_SIZE);
    ~ExtendedAudioPlayer();
    
    // ========================================================================
    // INITIALIZATION
    // ========================================================================
    
    /**
     * @brief Initialize with SD card source and decoder
     * @param output Audio output stream
     * @param decoder Audio decoder for encoded files
     * @param enableStreaming If true, initialize URL streaming mode
     */
    void begin(AudioStream& output, bool enableStreaming = true);
    int urlStreamBufferSize;

    /**
     * @brief Check if streaming fallback is enabled
         * 
         * When enabled, if local file playback fails, will attempt to stream from URL.
         */
    bool isStreamingEnabled() const { return streamingEnabled; }
    
    /**
     * @brief Enable or disable streaming fallback
     * 
     * When enabled, if local file is not available, will attempt to stream from URL.
     * Requires URL streaming to be initialized via beginURLMode() or initURLStreaming().
     */
    void setStreamingEnabled(bool enabled) { streamingEnabled = enabled; }
    

    // ========================================================================
    // PLAYBACK CONTROL
    // ========================================================================
    
    /**
     * @brief Play audio immediately, clearing any queued audio
     * 
     * Stops current playback, clears the queue, and starts the specified audio.
     * 
     * @param type The type of stream (GENERATOR, URL_STREAM, FILE_STREAM)
     * @param audioKey The key identifying the audio (mapped to actual resource)
     * @param durationMs Maximum playback duration (0 = unlimited)
     * @return true if started successfully
     */
    bool playAudio(AudioStreamType type, const char* audioKey, unsigned long durationMs = 0);
    
    /**
     * @brief Play audio immediately by key (auto-detects stream type), clearing queue
     * @param audioKey The audio key to play
     * @param durationMs Maximum playback duration (0 = unlimited)
     * @return true if started successfully
     */
    bool playAudioKey(const char* audioKey, unsigned long durationMs = 0);
    
    /**
     * @brief Play audio by file path or URL, clearing queue
     * @param path File path or URL to play
     * @return true if started successfully
     */
    bool playPath(const char* path);
    
    /**
     * @brief Queue audio for playback after current audio finishes
     * 
     * If player is inactive, starts playback immediately.
     * If player is active, queues the audio for later playback.
     * 
     * @param type The type of stream (GENERATOR, URL_STREAM, FILE_STREAM)
     * @param audioKey The key identifying the audio (mapped to actual resource)
     * @param durationMs Maximum playback duration (0 = unlimited)
     * @return true if queued or started successfully
     */
    bool queueAudio(AudioStreamType type, const char* audioKey, unsigned long durationMs = 0);
    
    /**
     * @brief Queue audio by key (auto-detects stream type)
     * @param audioKey The audio key to play
     * @param durationMs Maximum playback duration (0 = unlimited)
     * @return true if started or queued successfully
     */
    bool queueAudioKey(const char* audioKey, unsigned long durationMs = 0);
    
    /**
     * @brief Play a playlist immediately, clearing queue
     * 
     * Loads all items from the named playlist and queues them for playback.
     * Clears any existing queue first.
     * 
     * @param playlistName The name of the playlist to play
     * @return true if playlist exists and started successfully
     */
    bool playPlaylist(const char* playlistName);
    
    /**
     * @brief Queue a playlist for playback after current audio finishes
     * @param playlistName The name of the playlist to queue
     * @return true if playlist exists and was queued successfully
     */
    bool queuePlaylist(const char* playlistName);
    
    /**
     * @brief Stop current audio and clear the queue
     */
    void stop();
    
    /**
     * @brief Check if audio is currently playing
     */
    bool isActive() const;
    
    /**
     * @brief Get the timestamp when playback last started
     * @return Timestamp in milliseconds when last audio started playing
     */
    unsigned long getLastActive() const { return playbackStartTime; }
    
    /**
     * @brief Set active state (false clears queue)
     */
    void setActive(bool active);
    
    /**
     * @brief Process audio - call this in your main loop
     * @return true if audio is still playing
     */
    bool copy();
    
    /**
     * @brief Move to next queued item (called automatically on stream end)
     * @return true if moved to next item successfully
     */
    bool next();
    
    // ========================================================================
    // QUEUE MANAGEMENT
    // ========================================================================
    
    /**
     * @brief Get number of items in the queue
     */
    size_t getQueueSize() const { return audioQueue.size(); }
    
    /**
     * @brief Clear the audio queue
     */
    void clearQueue();
    
    /**
     * @brief Check if a specific audioKey is currently playing
     */
    bool isAudioKeyPlaying(const char* audioKey) const;
    
    /**
     * @brief Stop playback only if the specified key is playing
     */
    void stopAudioKey(const char* audioKey);
    
    /**
     * @brief Get the currently playing audio key
     */
    const char* getCurrentAudioKey() const;
    
    // ========================================================================
    // VOLUME CONTROL
    // ========================================================================
    
    /**
     * @brief Set volume (0.0 to 1.0, persisted to storage)
     */
    void setVolume(float volume);
    
    /**
     * @brief Get current volume
     */
    float getVolume() const { return currentVolume; }
    
    // ========================================================================
    // CALLBACKS
    // ========================================================================
    
    /**
     * @brief Set callback for audio start/stop events
     */
    void setAudioEventCallback(AudioEventCallback callback) { eventCallback = callback; }
    
    // ========================================================================
    // REGISTRY
    // ========================================================================
    
    /**
     * @brief Set the audio key registry
     * @param registry Pointer to registry (caller retains ownership)
     * 
     * The registry handles all key resolution, generator lookups, etc.
     */
    void setRegistry(AudioKeyRegistry* registry);
    
    /**
     * @brief Get the current registry
     */
    AudioKeyRegistry* getRegistry() const { return registry; }
    
    /**
     * @brief Check if an audioKey is registered or resolvable
     */
    bool hasAudioKey(const char* audioKey) const;
    
    // ========================================================================
    // DECODER MANAGEMENT
    // ========================================================================
    
    /**
     * @brief Add a decoder with MIME type
     * 
     * If no decoder exists, sets this as the decoder.
     * If a MultiDecoder exists, forwards the addDecoder call.
     * If a non-MultiDecoder exists, converts to MultiDecoder and adds both.
     * 
     * @param newDecoder The decoder to add
     * @param mime The MIME type for this decoder
     */
    void addDecoder(AudioDecoder& newDecoder, const char* mime);
    
protected:
    // Core components
    ExtendedAudioSource* source = nullptr;
    AudioPlayer* player = nullptr;
    AudioDecoder* decoder = nullptr;
    MultiDecoder* ownedMultiDecoder = nullptr;  // Owned if we created it
    bool isMultiDecoder = false;  // Track if decoder is a MultiDecoder
    AudioStream* output = nullptr;
    VolumeStream volumeStream;
    
    // Registry for key resolution
    AudioKeyRegistry* registry = nullptr;
    
    // Encoded stream for URL mode
    EncodedAudioStream* encodedStream = nullptr;
    StreamCopy* streamCopier = nullptr;
    
    // State tracking
    bool initialized = false;
    bool urlMode = false;           // True if URL streaming has been initialized
    bool streamingEnabled = false;  // True if streaming fallback is enabled
    bool isPlaying = false;
    float currentVolume = 0.5f;
    
    // Current playback state
    AudioStreamType currentType = AudioStreamType::NONE;
    char currentKey[64] = {0};
    unsigned long currentDurationMs = 0;
    unsigned long playbackStartTime = 0;
    
    // Audio queue
    std::vector<QueuedAudioItem> audioQueue;
    
    // Event callback
    AudioEventCallback eventCallback = nullptr;
    
    // Helper methods
    bool startStream(AudioStreamType type, const char* audioKey, unsigned long durationMs);
    void stopInternal();
    void onStreamEnd();
    AudioStreamType detectStreamType(const char* audioKey) const;
    const char* resolveAudioKey(const char* audioKey) const;
    
    // Volume persistence
    void loadVolumeFromStorage();
    void saveVolumeToStorage();
    
    // Static callback for EOF handling
    static void onEOFCallback(AudioPlayer& player);
};

// ============================================================================
// GLOBAL INSTANCE ACCESS
// ============================================================================

/**
 * @brief Get the global extended audio player instance
 */
ExtendedAudioPlayer& getExtendedAudioPlayer();
