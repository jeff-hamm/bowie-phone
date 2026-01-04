/**
 * @file audio_player.cpp
 * @brief Audio Player Implementation for Bowie Phone
 * 
 * This file implements audio playback functionality using the AudioTools library.
 * It integrates with known_processor for sequence-based file lookups.
 * 
 * @date 2025
 */

#include "audio_player.h"
#include "audio_file_manager.h"
#include "logging.h"
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include "AudioTools/CoreAudio/AudioEffects/SoundGenerator.h"

using namespace audio_tools;

// ============================================================================
// DIAL TONE GENERATOR (350 Hz + 440 Hz North American dial tone)
// ============================================================================

// Custom dual-tone generator for dial tone
class DualToneGenerator : public SoundGenerator<int16_t> {
public:
    DualToneGenerator(float freq1 = 350.0f, float freq2 = 440.0f, float amplitude = 16000.0f) 
        : m_freq1(freq1), m_freq2(freq2), m_amplitude(amplitude) {
        m_sampleRate = 44100; // Default
        recalcPhaseIncrements();
    }
    
    bool begin(AudioInfo info) override {
        SoundGenerator<int16_t>::begin(info);
        m_sampleRate = info.sample_rate;
        recalcPhaseIncrements();
        reset();
        return true;
    }
    
    void reset() {
        m_phase1 = 0.0f;
        m_phase2 = 0.0f;
    }
    
    void recalcPhaseIncrements() {
        // Phase increment per sample (radians)
        m_phaseInc1 = 2.0f * PI * m_freq1 / (float)m_sampleRate;
        m_phaseInc2 = 2.0f * PI * m_freq2 / (float)m_sampleRate;
    }
    
    int16_t readSample() override {
        // Generate two sine waves and add them together
        float sample1 = sinf(m_phase1) * m_amplitude * 0.5f;
        float sample2 = sinf(m_phase2) * m_amplitude * 0.5f;
        
        // Advance phases
        m_phase1 += m_phaseInc1;
        m_phase2 += m_phaseInc2;
        
        // Wrap phases to avoid floating point overflow (wrap at 2*PI)
        if (m_phase1 >= 2.0f * PI) m_phase1 -= 2.0f * PI;
        if (m_phase2 >= 2.0f * PI) m_phase2 -= 2.0f * PI;
        
        return (int16_t)(sample1 + sample2);
    }
    
private:
    float m_freq1, m_freq2;
    float m_amplitude;
    int m_sampleRate;
    float m_phaseInc1 = 0.0f;
    float m_phaseInc2 = 0.0f;
    float m_phase1 = 0.0f;
    float m_phase2 = 0.0f;
};

// Dial tone components
static DualToneGenerator* dialToneGenerator = nullptr;
static GeneratedSoundStream<int16_t>* dialToneStream = nullptr;
static StreamCopy* dialToneCopier = nullptr;
static AudioStream* dialToneOutput = nullptr;
static bool dialToneActive = false;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static AudioPlayer* audioPlayer = nullptr;
static bool isPlaying = false;
static unsigned long audioStartTime = 0;
static float currentVolume = DEFAULT_AUDIO_VOLUME;
static Preferences volumePrefs;
static AudioEventCallback eventCallback = nullptr;
static char currentAudioKey[32] = {0};  // Track what audio is currently playing

// URL Streaming support
static bool urlStreamingMode = false;
static URLStream* urlStream = nullptr;
static AudioStream* urlOutput = nullptr;
static AudioDecoder* urlDecoder = nullptr;
static StreamCopy* urlCopier = nullptr;
static EncodedAudioStream* urlEncodedStream = nullptr;
static char currentStreamURL[256] = {0};

// ============================================================================
// DIAL TONE FUNCTIONS
// ============================================================================

/**
 * @brief Initialize the dial tone generator
 * @param output The audio output stream (AudioBoardStream)
 */
void initDialToneGenerator(AudioStream &output)
{
    if (dialToneGenerator != nullptr) {
        Logger.println("⚠️ Dial tone generator already initialized");
        return;
    }
    
    Logger.println("🔧 Initializing dial tone generator (350 Hz + 440 Hz)...");
    
    dialToneOutput = &output;
    
    // Create the dual-tone generator (North American dial tone: 350 + 440 Hz)
    dialToneGenerator = new DualToneGenerator(350.0f, 440.0f, 16000.0f);
    
    // Create stream wrapper
    dialToneStream = new GeneratedSoundStream<int16_t>(*dialToneGenerator);
    
    // Create copier to output
    dialToneCopier = new StreamCopy(*dialToneOutput, *dialToneStream);
    
    Logger.println("✅ Dial tone generator initialized");
}

/**
 * @brief Start playing the synthesized dial tone
 * @return true if dial tone started successfully
 */
bool startDialTone()
{
    if (!dialToneGenerator || !dialToneStream || !dialToneCopier) {
        Logger.println("❌ Dial tone generator not initialized");
        return false;
    }
    
    if (dialToneActive) {
        Logger.println("⚠️ Dial tone already playing");
        return true;
    }
    
    // Stop any file-based audio first
    if (isPlaying) {
        stopAudio();
    }
    
    Logger.println("🎵 Starting synthesized dial tone (350 + 440 Hz)...");
    
    // Configure audio info
    AudioInfo info;
    info.sample_rate = 44100;
    info.channels = 2;
    info.bits_per_sample = 16;
    
    // Always reset the generator to ensure consistent frequency
    dialToneGenerator->reset();
    dialToneGenerator->begin(info);
    dialToneStream->begin(info);
    
    dialToneActive = true;
    isPlaying = true;
    strncpy(currentAudioKey, "dialtone", sizeof(currentAudioKey) - 1);
    audioStartTime = millis();
    
    // Notify callback
    if (eventCallback) {
        eventCallback(true);
    }
    
    Logger.println("✅ Dial tone started");
    return true;
}

/**
 * @brief Stop the synthesized dial tone
 */
void stopDialTone()
{
    if (!dialToneActive) {
        return;
    }
    
    Logger.println("⏹️ Stopping dial tone...");
    dialToneActive = false;
    // Don't clear isPlaying here - let stopAudio() handle it
}

/**
 * @brief Process dial tone output (call this in the main loop)
 * @return true if dial tone is still active
 */
bool processDialTone()
{
    if (!dialToneActive || !dialToneGenerator || !dialToneOutput) {
        return false;
    }
    
    // Use StreamCopy for proper I2S timing synchronization
    // This ensures dial tone output is synchronized with the I2S driver
    if (dialToneCopier) {
        dialToneCopier->copy();
    }
    return true;
}

/**
 * @brief Briefly pause dial tone, sample mic for DTMF, and resume
 * This creates a tiny gap (~5ms) that's barely audible but allows clean DTMF detection
 * @param fftInput The FFT stream to receive mic input
 * @param micSource The mic input source (kit stream)
 * @return true if dial tone is still active
 */
bool sampleDTMFDuringDialTone(AudioStream& fftInput, AudioStream& micSource)
{
    if (!dialToneActive) {
        return false;
    }
    
    // Read mic input and send to FFT
    // The dial tone output continues via DMA while we sample
    static uint8_t buffer[512];
    size_t bytesRead = micSource.readBytes(buffer, sizeof(buffer));
    if (bytesRead > 0) {
        fftInput.write(buffer, bytesRead);
    }
    
    return true;
}

/**
 * @brief Check if synthesized dial tone is playing
 */
bool isSynthDialTonePlaying()
{
    return dialToneActive;
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
    
    // Load volume from storage (after player is initialized)
    currentVolume = loadVolumeFromStorage();
    
    // Set the volume on the player
    if (audioPlayer)
    {
        audioPlayer->setVolume(currentVolume);
        Logger.printf("🔊 Initial volume set to %.2f\n", currentVolume);
    }
    
    Logger.println("✅ Audio player initialized");
}

void initAudioPlayerURLMode(AudioStream &output, AudioDecoder &decoder)
{
    Logger.println("🔧 Initializing audio player in URL streaming mode...");
    
    urlStreamingMode = true;
    urlOutput = &output;
    urlDecoder = &decoder;
    
    // Create URLStream for HTTP fetching
    urlStream = new URLStream(URL_STREAM_BUFFER_SIZE);
    
    // Create encoded stream for decoding (takes pointers)
    urlEncodedStream = new EncodedAudioStream(&output, &decoder);
    
    // Create stream copier
    urlCopier = new StreamCopy(*urlEncodedStream, *urlStream);
    urlCopier->setRetry(10);  // Retry on network hiccups
    
    // Load volume from storage
    currentVolume = loadVolumeFromStorage();
    Logger.printf("🔊 Initial volume set to %.2f\n", currentVolume);
    
    Logger.println("✅ Audio player initialized (URL streaming mode)");
}

bool isURLStreamingMode()
{
    return urlStreamingMode;
}

bool playAudioFromURL(const char* url)
{
    if (!url || strlen(url) == 0)
    {
        Logger.println("❌ Cannot play: no URL provided");
        return false;
    }
    
    if (!urlStreamingMode || !urlStream || !urlEncodedStream)
    {
        Logger.println("❌ Cannot play URL: URL streaming mode not initialized");
        return false;
    }
    
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED)
    {
        Logger.println("❌ Cannot stream: WiFi not connected");
        return false;
    }
    
    // Stop any current playback
    if (isPlaying)
    {
        Logger.println("⏹️ Stopping current audio to play new URL");
        stopAudio();
    }
    
    Logger.printf("🌐 Starting URL stream: %s\n", url);
    
    // Store the URL
    strncpy(currentStreamURL, url, sizeof(currentStreamURL) - 1);
    currentStreamURL[sizeof(currentStreamURL) - 1] = '\0';
    
    // Determine MIME type from URL extension
    const char* mimeType = "audio/mpeg";  // Default to MP3
    if (strstr(url, ".wav") != nullptr)
    {
        mimeType = "audio/wav";
    }
    else if (strstr(url, ".mp3") != nullptr)
    {
        mimeType = "audio/mpeg";
    }
    
    // Begin URL stream
    if (!urlStream->begin(url, mimeType))
    {
        Logger.printf("❌ Failed to open URL stream: %s\n", url);
        return false;
    }
    
    // Begin the encoded stream
    urlEncodedStream->begin();
    
    isPlaying = true;
    audioStartTime = millis();
    
    // Notify callback
    if (eventCallback)
    {
        eventCallback(true);
    }
    
    Logger.println("🎵 URL streaming started");
    return true;
}

bool playAudioPath(const char* filePath)
{
    // In URL streaming mode, check if this is actually a URL
    if (urlStreamingMode)
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
    
    // Check if file exists
    if (!SD.exists(filePath))
    {
        Logger.printf("❌ Audio file not found: %s\n", filePath);
        return false;
    }
    
    // Stop any currently playing audio first
    if (isPlaying)
    {
        Logger.println("⏹️ Stopping current audio to play new file");
        stopAudio();
    }
    
    Logger.printf("🎵 Starting audio playback: %s\n", filePath);
    
    // Use setPath which properly closes the previous file before opening the new one
    // This is important to prevent SD card corruption from leftover file handles
    if (!audioPlayer->setPath(filePath))
    {
        Logger.printf("❌ Failed to set path: %s\n", filePath);
        return false;
    }
    
    isPlaying = true;
    audioStartTime = millis();
    // Note: currentAudioKey is set by playAudioBySequence if called via key
    
    // Notify callback
    if (eventCallback)
    {
        eventCallback(true);
    }
    
    Logger.println("🎵 Audio playback started");
    return true;
}

bool playAudioBySequence(const char* sequence)
{
    if (!sequence)
    {
        Logger.println("❌ Cannot play: no sequence provided");
        return false;
    }
    
    // Special case: use synthesized dial tone for seamless looping
    // TEMPORARILY DISABLED - focusing on DTMF detection
    if (strcmp(sequence, "dialtone") == 0)
    {
        Logger.println("🎯 Dial tone DISABLED for DTMF testing");
        return false;  // Don't play dial tone
        // return startDialTone();
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
    
    Logger.printf("🎯 Playing audio for key: %s\n", sequence);
    
    // Process the audio key to get the file path
    const char* filePath = processAudioKey(sequence);
    
    if (!filePath)
    {
        Logger.printf("⚠️ No audio path returned for sequence: %s\n", sequence);
        return false;
    }
    
    Logger.printf("📂 Got file path: %s\n", filePath);
    return playAudioPath(filePath);
}

void stopAudio()
{
    // Stop synthesized dial tone if active
    if (dialToneActive)
    {
        stopDialTone();
    }
    
    // Handle URL streaming mode
    if (urlStreamingMode)
    {
        if (urlStream)
        {
            urlStream->end();
        }
        if (urlEncodedStream)
        {
            urlEncodedStream->end();
        }
        currentStreamURL[0] = '\0';
    }
    else if (audioPlayer)
    {
        // Stop playback but don't end/close the player
        // Using stop() instead of end() to keep the player ready for next file
        if (audioPlayer->isActive())
        {
            audioPlayer->stop();
            Logger.println("⏹️ Audio player stopped");
        }
    }
    
    if (!isPlaying)
    {
        return;
    }
    
    isPlaying = false;
    currentAudioKey[0] = '\0';  // Clear the audio key
    
    // Notify callback
    if (eventCallback)
    {
        eventCallback(false);
    }
    
    Logger.println("🔇 Audio playback stopped");
}

bool isAudioActive()
{
    return isPlaying || dialToneActive;
}

bool isDialTonePlaying()
{
    // Check both synthesized dial tone and file-based dial tone
    return dialToneActive || (isPlaying && (strcmp(currentAudioKey, "dialtone") == 0));
}

const char* getCurrentAudioKey()
{
    return currentAudioKey[0] != '\0' ? currentAudioKey : nullptr;
}

bool processAudio()
{
    // Handle synthesized dial tone first (highest priority for smooth playback)
    if (dialToneActive)
    {
        return processDialTone();
    }
    
    if (!isPlaying)
    {
        return false;
    }
    
    // Handle URL streaming mode
    if (urlStreamingMode)
    {
        if (!urlCopier || !urlStream)
        {
            return false;
        }
        
        // Copy data from URL stream to output
        size_t copied = urlCopier->copy();
        
        // Check if stream ended
        if (copied == 0 && !urlStream->available())
        {
            // If dial tone was playing, loop it
            if (currentAudioKey[0] != '\0' && strcmp(currentAudioKey, "dialtone") == 0)
            {
                Logger.println("🔄 Looping dial tone (URL mode)...");
                // Restart the URL stream
                if (currentStreamURL[0] != '\0')
                {
                    urlStream->end();
                    if (urlStream->begin(currentStreamURL, "audio/mpeg"))
                    {
                        urlEncodedStream->begin();
                        audioStartTime = millis();
                        return true;
                    }
                }
            }
            
            stopAudio();
            return false;
        }
        
        return true;
    }
    
    // SD card mode
    if (!audioPlayer)
    {
        return false;
    }
    
    audioPlayer->copy();
    
    // Check if playback finished
    if (!audioPlayer->isActive())
    {
        stopAudio();
        return false;
    }
    
    return true;
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

void setAudioEventCallback(AudioEventCallback callback)
{
    eventCallback = callback;
}
