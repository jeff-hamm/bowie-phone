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
    
    // IMPORTANT: Disable auto-advancing to next file
    // We want to play only the requested file, not iterate through all files
    audioPlayer->setAutoNext(false);
    
    // Load volume from storage
    currentVolume = loadVolumeFromStorage();
    
    // Set the volume on the player
    if (audioPlayer)
    {
        audioPlayer->setVolume(currentVolume);
        Logger.printf("🔊 Initial volume set to %.2f\n", currentVolume);
    }
    
    audioPlayer->begin();
    Logger.println("✅ Audio player initialized");
}

bool playAudioPath(const char* filePath)
{
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
    if (!audioPlayer)
    {
        return;
    }
    
    // Stop playback but don't end/close the player
    // Using stop() instead of end() to keep the player ready for next file
    if (audioPlayer->isActive())
    {
        audioPlayer->stop();
        Logger.println("⏹️ Audio player stopped");
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
    return isPlaying;
}

bool isDialTonePlaying()
{
    return isPlaying && (strcmp(currentAudioKey, "dialtone") == 0);
}

const char* getCurrentAudioKey()
{
    return currentAudioKey[0] != '\0' ? currentAudioKey : nullptr;
}

bool processAudio()
{
    if (!audioPlayer || !isPlaying)
    {
        return false;
    }
    
    audioPlayer->copy();
    
    // Check if playback finished
    if (!audioPlayer->isActive())
    {
        // If dial tone was playing, loop it
        if (currentAudioKey[0] != '\0' && strcmp(currentAudioKey, "dialtone") == 0)
        {
            Logger.println("🔄 Looping dial tone...");
            // Re-get the file path and restart playback
            const char* filePath = processAudioKey("dialtone");
            if (filePath && audioPlayer->setPath(filePath))
            {
                audioStartTime = millis();
                return true;
            }
        }
        
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
