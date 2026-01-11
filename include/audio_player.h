/**
 * @file audio_player.h
 * @brief Audio Player Header for Bowie Phone
 * 
 * This library handles audio playback using the AudioTools library.
 * It integrates with known_processor for sequence-based lookups.
 * 
 * @date 2025
 */

#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

// ============================================================================
// INCLUDES
// ============================================================================
#include <Arduino.h>

// Include AudioTools first to get Vector and other base types defined
#include "AudioTools.h"

// Now include additional headers that depend on AudioTools types
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/Communication/HTTP/URLStream.h"

// ============================================================================
// CONSTANTS AND CONFIGURATION
// ============================================================================

#ifndef DEFAULT_AUDIO_VOLUME
#define DEFAULT_AUDIO_VOLUME 0.7f  ///< Default audio volume (0.0 to 1.0)
#endif

// URL Streaming Configuration
// Define FORCE_URL_STREAMING=1 in build flags to always use URL streaming
// even when SD card is present
#ifndef FORCE_URL_STREAMING
#define FORCE_URL_STREAMING 0
#endif

#ifndef URL_STREAM_BUFFER_SIZE
#define URL_STREAM_BUFFER_SIZE 2048  ///< Buffer size for URL streaming
#endif

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Initialize the audio player system
 * @param source Reference to the audio source
 * @param output Reference to the audio output stream
 * @param decoder Reference to the audio decoder
 * 
 * Sets up the audio player with the provided source and decoder.
 */
void initAudioPlayer(AudioSource &source, AudioStream &output, AudioDecoder &decoder);

/**
 * @brief Initialize audio player for URL streaming mode (no SD card)
 * @param output Reference to the audio output stream
 * @param decoder Reference to the audio decoder
 * 
 * Sets up URL-based streaming when SD card is unavailable.
 */
void initAudioUrlPlayer(AudioStream &output, AudioDecoder &decoder);

/**
 * @brief Check if audio player is in URL streaming mode
 * @return true if using URL streaming, false if using SD card
 */
bool isURLStreamingMode();

/**
 * @brief Play audio directly from a URL
 * @param url HTTP/HTTPS URL to stream audio from
 * @return true if streaming started, false otherwise
 */
bool playAudioFromURL(const char* url);

/**
 * @brief Start playing an audio file by path
 * @param filePath Path to the audio file to play
 * @return true if playback started, false otherwise
 * 
 * Non-blocking call. Use processAudio() in loop to continue playback.
 */
bool playAudioPath(const char* filePath);

/**
 * @brief Start playing an audio file by sequence/key
 * @param key DTMF sequence or audio key to look up and play
 * @param durationMs Maximum duration in milliseconds (0 = unlimited)
 * @return true if playback started, false if key not found or error
 * 
 * Looks up the key in known_processor and plays the associated file.
 * If durationMs > 0, audio will automatically stop after that duration.
 */
bool playAudioKey(const char* key, unsigned long durationMs = 0);

/**
 * @brief Play an audio key for a duration, then play a file path
 * @param audioKey The audio key to play first (e.g., "ringback")
 * @param durationMs Duration to play the audio key in milliseconds
 * @param filePath Path to the audio file to play after the audio key completes
 * @return true if playback started, false on error
 * 
 * Plays audioKey for durationMs, then automatically transitions to filePath.
 * Useful for playing ringback before the actual audio content.
 */
bool playAudioPair(const char* audioKey, unsigned long durationMs, const char* filePath);

/**
 * @brief Set the ring duration (how long ringback plays before audio)
 * @param durationMs Duration in milliseconds (0 = no ringback)
 */
void setRingDuration(unsigned long durationMs);

/**
 * @brief Get the current ring duration setting
 * @return Ring duration in milliseconds
 */
unsigned long getRingDuration();

/**
 * @brief Stop current audio playback
 */
void stopAudio();

/**
 * @brief Completely shut down audio subsystem for OTA updates
 * Stops all audio, ends the audio board stream, and releases I2S/I2C
 */
void shutdownAudioForOTA();

/**
 * @brief Check if audio is currently playing
 * @return true if playing, false otherwise
 */
bool isAudioActive();

/**
 * @brief Check if specific audio is currently playing
 * @param audioKey The audio key to check (e.g., "dialtone", "ringback")
 * @return true if the specified audio is playing, false otherwise
 */
bool isAudioKeyPlaying(const char* audioKey);

/**
 * @brief Start playing the synthesized dial tone (350 + 440 Hz)
 * @return true if dial tone started successfully
 */
bool startDialTone();

/**
 * @brief Start playing the synthesized ringback tone (440 + 480 Hz, 2s on / 4s off)
 * @return true if ringback started successfully
 */
bool startRingback();

/**
 * @brief Stop audio if currently playing the specified key
 * @param audioKey The audio key to stop (e.g., "dialtone", "ringback")
 */
void stopAudioKey(const char* audioKey);

/**
 * @brief Get the current audio key being played
 * @return Current audio key string, or nullptr if nothing playing
 */
const char* getCurrentAudioKey();

/**
 * @brief Process audio playback (call this in main loop)
 * @return true if still playing, false if finished or not playing
 */
bool processAudio();

/**
 * @brief Set the audio volume
 * @param volume Volume level (0.0 to 1.0)
 * 
 * Sets the volume and saves it to preferences for persistence.
 */
void setAudioVolume(float volume);

/**
 * @brief Get the current audio volume
 * @return Current volume level (0.0 to 1.0)
 */
float getAudioVolume();

/**
 * @brief Callback type for audio playback events
 */
typedef void (*AudioEventCallback)(bool started);

/**
 * @brief Set callback for audio playback events
 * @param callback Function to call when audio starts/stops
 */
void setAudioEventCallback(AudioEventCallback callback);

#endif // AUDIO_PLAYER_H
