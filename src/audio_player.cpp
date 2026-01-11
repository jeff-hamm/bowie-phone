/**
 * @file audio_player.cpp
 * @brief Audio Player Implementation for Bowie Phone
 * 
 * This file implements audio playback functionality using the AudioTools library.
 * It integrates with known_processor for sequence-based file lookups.
 * 
 * @date 2025
 */

#include <config.h>
#include "audio_player.h"
#include "audio_file_manager.h"
#include "logging.h"
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include "tone_generators.h"
#include "AudioTools/CoreAudio/AudioEffects/SoundGenerator.h"

using namespace audio_tools;

// ============================================================================
// CONFIGURATION
// ============================================================================

// Uncomment to disable dial tone (for DTMF detection testing)
//#define DISABLE_DIAL_TONE

// ============================================================================
// ACTIVE STREAM MANAGEMENT
// ============================================================================

enum class ActiveStreamType {
    NONE,
    GENERATOR,    // Dial tone, ringback, or other synthesized tones
    URL_STREAM,
    AUDIO_PLAYER  // SD card mode
};

static ActiveStreamType activeStreamType = ActiveStreamType::NONE;
static StreamCopy* activeStreamCopier = nullptr;
static AudioStream* audioOutput = nullptr;
static VolumeStream volume_out;
    // ============================================================================
    // TONE GENERATORS
    // ============================================================================

    // Single GeneratedSoundStream that switches between generators via setInput
    static GeneratedSoundStream<int16_t>
        toneStream;

// Available tone generators
static DualToneGenerator dialToneGenerator(350.0f, 440.0f, 16000.0f);
static DualToneGenerator ringbackToneGenerator(440.0f, 480.0f, 16000.0f);
static RepeatingToneGenerator<int16_t> ringbackRepeater(ringbackToneGenerator, 2000, 4000);

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static AudioPlayer* audioPlayer = nullptr;
static unsigned long audioStartTime = 0;
static unsigned long audioDurationLimit = 0;  // 0 = unlimited
static float currentVolume = DEFAULT_AUDIO_VOLUME;
static Preferences volumePrefs;
static AudioEventCallback eventCallback = nullptr;
static char currentAudioKey[32] = {0};

// Audio pair playback state (play audioKey, then filePath)
static bool audioPairPending = false;
static char pendingFilePath[256] = {0};

// Ring duration setting (how long ringback plays before audio)
static unsigned long ringDuration = 0;  // 0 = no ringback

// URL Streaming support
static URLStream* urlStream = nullptr;
static AudioDecoder* urlDecoder = nullptr;
static EncodedAudioStream* urlEncodedStream = nullptr;
static char currentStreamURL[256] = {0};

// ============================================================================
// STREAM MANAGEMENT HELPERS
// ============================================================================

/**
 * @brief Dispose the active stream copier and reset state
 */
static void disposeActiveStream()
{
    // Delete the dynamic StreamCopy
    if (activeStreamCopier) {
        delete activeStreamCopier;
        activeStreamCopier = nullptr;
    }
    
    // Clean up stream-specific resources
    switch (activeStreamType) {
        case ActiveStreamType::AUDIO_PLAYER:
            if (audioPlayer && audioPlayer->isActive()) {
                audioPlayer->stop();
                audioPlayer->setAudioInfo(AUDIO_INFO_DEFAULT());
            }
            break;
        case ActiveStreamType::URL_STREAM:
            if (urlStream)
                urlStream->end();
            if (urlEncodedStream)
                urlEncodedStream->end();
            currentStreamURL[0] = '\0';
        default:
            // Reset audio output to original config
            if (audioOutput)
            {
                audioOutput->setAudioInfo(AUDIO_INFO_DEFAULT());
            }
            break;
    }

    activeStreamType = ActiveStreamType::NONE;
    currentAudioKey[0] = '\0';
    audioDurationLimit = 0;
    
    // Don't clear pending pair or call callback if we're transitioning to second audio
    if (!audioPairPending) {
        if (eventCallback) {
            eventCallback(false);
        }
    }
}

// ============================================================================
// TONE GENERATOR FUNCTIONS
// ============================================================================

/**
 * @brief Helper to start a tone generator
 * @param generator The SoundGenerator to use
 * @param key The audio key name (e.g., "dialtone", "ringback")
 * @param description Log description
 * @return true if started successfully
 */
template<typename T>
static bool startToneGenerator(SoundGenerator<T>& generator, const char* key, const char* description)
{
    if (!audioOutput) {
        Logger.println("❌ Audio output not initialized");
        return false;
    }
    
    // Dispose any active stream first
    disposeActiveStream();
    
    Logger.printf("🎵 Starting %s...\n", description);
    
    AudioInfo info = AUDIO_INFO_DEFAULT();
    generator.begin(info);

    // Set the generator on the shared stream
    toneStream.setInput(generator);
    toneStream.begin(info);
    
    // Create new StreamCopy
    activeStreamCopier = new StreamCopy(*audioOutput, toneStream);
    activeStreamType = ActiveStreamType::GENERATOR;
    strncpy(currentAudioKey, key, sizeof(currentAudioKey) - 1);
    audioStartTime = millis();
    
    if (eventCallback) eventCallback(true);
    
    Logger.printf("✅ %s started\n", description);
    return true;
}

/**
 * @brief Start playing the synthesized dial tone
 * @return true if dial tone started successfully
 */
bool startDialTone()
{
    return startToneGenerator(dialToneGenerator, "dialtone", "dial tone (350 + 440 Hz)");
}

/**
 * @brief Start playing the synthesized ringback tone
 * @return true if ringback started successfully
 */
bool startRingback()
{
    return startToneGenerator(ringbackRepeater, "ringback", "ringback (440 + 480 Hz, 2s on / 4s off)");
}

/**
 * @brief Stop audio if currently playing the specified key
 * @param audioKey The audio key to stop (e.g., "dialtone", "ringback")
 */
void stopAudioKey(const char* audioKey)
{
    if (!audioKey || activeStreamType == ActiveStreamType::NONE) {
        return;
    }
    
    if (strcmp(currentAudioKey, audioKey) == 0) {
        disposeActiveStream();
        Logger.printf("⏹️ %s stopped\n", audioKey);
    }
}

/**
 * @brief Check if specific audio is currently playing
 * @param audioKey The audio key to check (e.g., "dialtone", "ringback")
 * @return true if the specified audio is playing, false otherwise
 */
bool isAudioKeyPlaying(const char* audioKey)
{
    return activeStreamType != ActiveStreamType::NONE && strcmp(currentAudioKey, audioKey) == 0;
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Load volume from Preferences storage
 * @return Volume value from storage, or default if not found
 */
static float loadVolumeFromStorage()
{
    if (!volumePrefs.begin("audio", true)) // Read-only
    {
        Logger.println("⚠️ Failed to open volume preferences for reading");
        return DEFAULT_AUDIO_VOLUME;
    }
    
    float volume = volumePrefs.getFloat("volume", DEFAULT_AUDIO_VOLUME);
    volumePrefs.end();
    
    // Validate range
    if (volume < 0.0f || volume > 1.0f)
    {
        Logger.printf("⚠️ Invalid volume in storage: %.2f, using default\n", volume);
        return DEFAULT_AUDIO_VOLUME;
    }
    
    Logger.printf("📖 Loaded volume from storage: %.2f\n", volume);
    return volume;
}

/**
 * @brief Save volume to Preferences storage
 * @param volume Volume value to save
 */
static void saveVolumeToStorage(float volume)
{
    if (!volumePrefs.begin("audio", false)) // Read-write
    {
        Logger.println("❌ Failed to open volume preferences for writing");
        return;
    }
    
    volumePrefs.putFloat("volume", volume);
    volumePrefs.end();
    
    Logger.printf("💾 Saved volume to storage: %.2f\n", volume);
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================
AudioStream* initOutput(AudioStream &output)
{

    // Load volume from storage (after player is initialized)
    currentVolume = loadVolumeFromStorage();

    // Set the volume on the player
    if (audioPlayer)
    {
        audioPlayer->setVolume(currentVolume);
        Logger.printf("🔊 Initial volume set to %.2f\n", currentVolume);
    }
    volume_out.setVolume(currentVolume);
    volume_out.setOutput(output);
    audioOutput = &volume_out;
    return audioOutput;
}

void initAudioPlayer(AudioSource &source, AudioStream &output, AudioDecoder &decoder)
{
    // Check if already initialized
    if (audioPlayer != nullptr)
    {
        Logger.println("⚠️ Audio player already initialized, skipping...");
        return;
    }
    
    Logger.println("🔧 Initializing audio player...");
    
    // Create audio player with provided source and decoder
    audioPlayer = new AudioPlayer(source, output, decoder);
    
    // Initialize the audio player first
    audioPlayer->begin();
    
    // IMPORTANT: Disable auto-advancing to next file AFTER begin()
    // begin() resets autonext from the source, so we must set it after
    // We want to play only the requested file, not iterate through all files
    audioPlayer->setAutoNext(false);
    initOutput(output);
    Logger.println("✅ Audio player initialized");
}

void initAudioUrlPlayer(AudioStream &output, AudioDecoder &decoder)
{
    Logger.println("🔧 Initializing audio player in URL streaming mode...");

    
    urlDecoder = &decoder;
    
    // Create URLStream for HTTP fetching
    urlStream = new URLStream(URL_STREAM_BUFFER_SIZE);
    
    // Configure URLStream to look like a browser to avoid bot detection
    urlStream->httpRequest().header().put("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    urlStream->httpRequest().header().put("Accept", "*/*");
    
    // Create encoded stream for decoding
    initOutput(output);

    urlEncodedStream = new EncodedAudioStream(audioOutput, &decoder);

    
    Logger.println("✅ Audio player initialized (URL streaming mode)");
}

bool isURLStreamingMode()
{
    return urlStream != nullptr;
}

bool playAudioFromURL(const char* url)
{
    if (!url || strlen(url) == 0)
    {
        Logger.println("❌ Cannot play: no URL provided");
        return false;
    }
    
    if (!urlStream || !urlEncodedStream || !audioOutput)
    {
        Logger.println("❌ Cannot play URL: URL streaming mode not initialized");
        return false;
    }
    
    if (WiFi.status() != WL_CONNECTED)
    {
        Logger.println("❌ Cannot stream: WiFi not connected");
        return false;
    }
    
    // Dispose any active stream first
    disposeActiveStream();
    
    Logger.printf("🌐 Starting URL stream: %s\n", url);
    
    strncpy(currentStreamURL, url, sizeof(currentStreamURL) - 1);
    currentStreamURL[sizeof(currentStreamURL) - 1] = '\0';
    
    // Determine MIME type from URL extension
    const char* mimeType = "audio/mpeg";
    if (strstr(url, ".wav") != nullptr) mimeType = "audio/wav";
    
    if (!urlStream->begin(url, mimeType))
    {
        Logger.printf("❌ Failed to open URL stream: %s\n", url);
        return false;
    }
    
    urlEncodedStream->begin();
    
    // Create new StreamCopy for URL streaming
    activeStreamCopier = new StreamCopy(*urlEncodedStream, *urlStream);
    activeStreamCopier->setRetry(10);
    activeStreamType = ActiveStreamType::URL_STREAM;
    audioStartTime = millis();
    
    if (eventCallback) eventCallback(true);
    
    Logger.println("🎵 URL streaming started");
    return true;
}

bool playAudioPath(const char* filePath)
{
    // In URL streaming mode, check if this is actually a URL
    if (urlStream)
    {
        if (strncmp(filePath, "http://", 7) == 0 || strncmp(filePath, "https://", 8) == 0)
        {
            return playAudioFromURL(filePath);
        }
        Logger.printf("❌ URL streaming mode but path is not a URL: %s\n", filePath);
        return false;
    }
    
    if (!audioPlayer || !filePath)
    {
        Logger.println("❌ Cannot play: player not initialized or no path");
        return false;
    }
    
    if (!SD.exists(filePath))
    {
        Logger.printf("❌ Audio file not found: %s\n", filePath);
        return false;
    }
    
    // Dispose any active stream first
    disposeActiveStream();
    
    Logger.printf("🎵 Starting audio playback: %s\n", filePath);
    
    if (!audioPlayer->setPath(filePath))
    {
        Logger.printf("❌ Failed to set path: %s\n", filePath);
        return false;
    }
    
    // setPath creates an active stream via audioPlayer
    activeStreamType = ActiveStreamType::AUDIO_PLAYER;
    audioStartTime = millis();
    
    if (eventCallback) eventCallback(true);
    
    Logger.println("🎵 Audio playback started");
    return true;
}

bool playAudioKey(const char* sequence, unsigned long durationMs)
{
    if (!sequence)
    {
        Logger.println("❌ Cannot play: no sequence provided");
        return false;
    }
    
    // Store duration limit (0 = unlimited)
    audioDurationLimit = durationMs;
    
    // Special case: use synthesized dial tone for seamless looping
    if (strcmp(sequence, "dialtone") == 0)
    {
#ifdef DISABLE_DIAL_TONE
        Logger.println("🎯 Dial tone DISABLED (DISABLE_DIAL_TONE defined)");
        return false;
#else
        bool result = startDialTone();
        if (result && durationMs > 0) {
            Logger.printf("⏱️ Duration limit set: %lu ms\n", durationMs);
        }
        return result;
#endif
    }
    
    // Special case: use synthesized ringback tone
    if (strcmp(sequence, "ringback") == 0)
    {
        bool result = startRingback();
        if (result && durationMs > 0) {
            Logger.printf("⏱️ Duration limit set: %lu ms\n", durationMs);
        }
        return result;
    }
    
    // Check if this is a known audio key
    if (!hasAudioKey(sequence))
    {
        Logger.printf("❌ Audio key not found: %s\n", sequence);
        return false;
    }
    
    // Store the audio key for tracking
    strncpy(currentAudioKey, sequence, sizeof(currentAudioKey) - 1);
    currentAudioKey[sizeof(currentAudioKey) - 1] = '\0';
    
    Logger.printf("🎯 Playing audio for key: %s", sequence);
    if (durationMs > 0) {
        Logger.printf(" (duration limit: %lu ms)\n", durationMs);
    } else {
        Logger.println();
    }
    
    // Process the audio key to get the file path
    const char* filePath = processAudioKey(sequence);
    
    if (!filePath)
    {
        Logger.printf("⚠️ No audio path returned for sequence: %s\n", sequence);
        return false;
    }
    
    Logger.printf("📂 Got file path: %s\n", filePath);
    
    // Check if this audio key has a ring duration configured
    unsigned long keyRingDuration = getAudioKeyRingDuration(sequence);
    if (keyRingDuration > 0)
    {
        Logger.printf("🔔 Audio key has ring_duration: %lu ms\n", keyRingDuration);
        return playAudioPair("ringback", keyRingDuration, filePath);
    }
    
    return playAudioPath(filePath);
}

void stopAudio()
{
    if (activeStreamType == ActiveStreamType::NONE) {
        return;
    }
    
    Logger.println("🔇 Stopping audio...");
    disposeActiveStream();
}

bool isAudioActive()
{
    return activeStreamType != ActiveStreamType::NONE;
}

const char* getCurrentAudioKey()
{
    return currentAudioKey[0] != '\0' ? currentAudioKey : nullptr;
}

bool processAudio()
{
    // Check if duration limit has been exceeded
    if (audioDurationLimit > 0 && activeStreamType != ActiveStreamType::NONE) {
        unsigned long elapsed = millis() - audioStartTime;
        if (elapsed >= audioDurationLimit) {
            Logger.printf("⏱️ Duration limit reached (%lu ms)\n", audioDurationLimit);
            disposeActiveStream();
            
            // Check if we have a pending audio pair file to play
            if (audioPairPending && pendingFilePath[0] != '\0') {
                Logger.printf("🎵 Transitioning to second audio: %s\n", pendingFilePath);
                audioPairPending = false;
                char pathCopy[256];
                strncpy(pathCopy, pendingFilePath, sizeof(pathCopy) - 1);
                pathCopy[sizeof(pathCopy) - 1] = '\0';
                pendingFilePath[0] = '\0';
                
                if (playAudioPath(pathCopy)) {
                    return true;
                }
            }
            return false;
        }
    }
    
    // If audioPlayer is active, ensure no other StreamCopy is running
    if (audioPlayer && audioPlayer->isActive()) {
        if (activeStreamType != ActiveStreamType::AUDIO_PLAYER && activeStreamType != ActiveStreamType::NONE) {
            // Dispose any stale stream copier
            if (activeStreamCopier) {
                delete activeStreamCopier;
                activeStreamCopier = nullptr;
            }
            activeStreamType = ActiveStreamType::AUDIO_PLAYER;
        }
        
        audioPlayer->copy();
        
        if (!audioPlayer->isActive()) {
            disposeActiveStream();
            return false;
        }
        return true;
    }
    
    // Handle StreamCopy-based streams (dial tone, ringback, URL)
    if (activeStreamCopier) {
        size_t copied = activeStreamCopier->copy();
        
        // For URL streams, check if stream ended (copied 0 and no more data)
        if (activeStreamType == ActiveStreamType::URL_STREAM) {
            if (copied == 0 && urlStream && !urlStream->available()) {
                disposeActiveStream();
                return false;
            }
        }
        // Tone generators run forever, no end check needed
        return true;
    }
    
    return false;
}

void setAudioVolume(float volume)
{
    // Clamp volume to valid range
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    
    currentVolume = volume;
    
    if (audioPlayer)
    {
        audioPlayer->setVolume(volume);
        Logger.printf("🔊 Volume set to %.2f\n", volume);
    }
    
    // Save to storage for persistence
    saveVolumeToStorage(volume);
}

float getAudioVolume()
{
    return currentVolume;
}

void setRingDuration(unsigned long durationMs)
{
    ringDuration = durationMs;
    Logger.printf("🔔 Ring duration set to %lu ms\n", durationMs);
}

unsigned long getRingDuration()
{
    return ringDuration;
}

bool playAudioPair(const char* audioKey, unsigned long durationMs, const char* filePath)
{
    if (!audioKey || !filePath || strlen(filePath) == 0)
    {
        Logger.println("❌ playAudioPair: invalid parameters");
        return false;
    }
    
    // Store the pending file path for after the first audio completes
    strncpy(pendingFilePath, filePath, sizeof(pendingFilePath) - 1);
    pendingFilePath[sizeof(pendingFilePath) - 1] = '\0';
    audioPairPending = true;
    
    Logger.printf("🎵 Playing audio pair: %s (%lu ms) -> %s\n", audioKey, durationMs, filePath);
    
    // Start the first audio with duration limit
    if (!playAudioKey(audioKey, durationMs))
    {
        Logger.println("❌ Failed to start first audio in pair");
        audioPairPending = false;
        pendingFilePath[0] = '\0';
        return false;
    }
    
    return true;
}

void setAudioEventCallback(AudioEventCallback callback)
{
    eventCallback = callback;
}
