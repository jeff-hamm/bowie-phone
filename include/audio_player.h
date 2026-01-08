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
void initAudioPlayerURLMode(AudioStream &output, AudioDecoder &decoder);

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
 * @param sequence DTMF sequence or audio key to look up and play
 * @return true if playback started, false if key not found or error
 * 
 * Looks up the sequence in known_processor and plays the associated file.
 */
bool playAudioBySequence(const char* sequence);

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
 * @brief Check if dial tone is currently playing
 * @return true if dial tone is playing, false otherwise
 */
bool isDialTonePlaying();

/**
 * @brief Initialize the synthesized dial tone generator
 * @param output The audio output stream (AudioBoardStream)
 */
void initDialToneGenerator(AudioStream &output);

/**
 * @brief Start playing the synthesized dial tone (350 + 440 Hz)
 * @return true if dial tone started successfully
 */
bool startDialTone();

/**
 * @brief Stop the synthesized dial tone
 */
void stopDialTone();

/**
 * @brief Check if synthesized dial tone is playing
 * @return true if dial tone is active
 */
bool isSynthDialTonePlaying();

/**
 * @brief Process dial tone output (call in main loop)
 * @return true if dial tone is still active
 */
bool processDialTone();

/**
 * @brief Sample mic for DTMF detection during dial tone
 * @param fftInput The FFT stream to receive mic input
 * @param micSource The mic input source (kit stream)
 * @return true if dial tone is still active
 */
bool sampleDTMFDuringDialTone(AudioStream& fftInput, AudioStream& micSource);

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
