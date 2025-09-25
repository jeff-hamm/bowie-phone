#include "dtmf_decoder.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT
#include "AudioTools/Disk/AudioSourceSDMMC.h" // or AudioSourceIdxSDFAT.h
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "sequence_processor.h"
#include "special_command_processor.h"
#include "known_processor.h"
#include "wifi_manager.h"
#include <SD.h>

AudioBoardStream kit(AudioKitAC101); // Audio source
AudioRealFFT fft;                       // or AudioKissFFT
StreamCopy copier(fft, kit);            // copy mic to tfl
int channels = 2;
int samples_per_second = 44100;
int bits_per_sample = 16;

// Audio playback components

const char *startFilePath="/";
const char* ext="mp3";
AudioSourceSDMMC source(startFilePath, ext);
MP3DecoderHelix decoder;  // or change to MP3DecoderMAD
AudioPlayer player(source, kit, decoder);
bool isPlayingAudio = false;
unsigned long audioStartTime = 0;

// DTMF sequence configuration
const unsigned long SEQUENCE_TIMEOUT = 2000; // 2 seconds timeout
const int MAX_SEQUENCE_LENGTH = 20;          // Maximum digits in sequence
char dtmfSequence[MAX_SEQUENCE_LENGTH + 1];  // +1 for null terminator
int sequenceIndex = 0;
unsigned long lastDigitTime = 0;

// Helper function to setup audio input and FFT
void setupAudioInput()
{
    // Setup audio input (DTMF detection)
    auto cfg = kit.defaultConfig(RXTX_MODE);
    cfg.input_device = ADC_INPUT_LINE2;
    cfg.channels = channels;
    cfg.sample_rate = samples_per_second;
    cfg.bits_per_sample = bits_per_sample;
    cfg.sd_active = true; // Enable SD card support
    kit.begin(cfg);

    // Setup FFT
    auto tcfg = fft.defaultConfig();
    tcfg.length = 8192;
    tcfg.channels = channels;
    tcfg.sample_rate = samples_per_second;
    tcfg.bits_per_sample = bits_per_sample;
    tcfg.callback = &fftResult;
    fft.begin(tcfg);
}

// Audio playback functions
void startAudioPlayback(const char* filePath)
{
    if (!filePath || isPlayingAudio)
    {
        return;
    }
    
    Serial.printf("🎵 Starting audio playback: %s\n", filePath);
    player.playPath(filePath);
    isPlayingAudio = true;
    audioStartTime = millis();
    Serial.println("🎵 Audio playback started");
}

void stopAudioPlayback()
{
    if(player.isActive())
        player.end();
    if (!isPlayingAudio)
    {
        return;
    }
    
    isPlayingAudio = false;
    // Restart the input audio system and FFT processing
    setupAudioInput();
    
    Serial.println("🎤 Resumed audio input and FFT processing");
}

// Check for new DTMF digits and manage sequence collection
// Returns true when a complete sequence is ready to process
bool checkForDTMFSequence()
{
    static unsigned long lastAnalysisTime = 0;
    unsigned long currentTime = millis();

    // Analyze DTMF data every 50ms
    if (currentTime - lastAnalysisTime > 50)
    {
        char detectedChar = analyzeDTMF();
        if (detectedChar != 0)
        {
            Serial.printf("DTMF digit detected: %c\n", detectedChar);

            // Add digit to sequence
            if (sequenceIndex < MAX_SEQUENCE_LENGTH)
            {
                dtmfSequence[sequenceIndex] = detectedChar;
                sequenceIndex++;
                dtmfSequence[sequenceIndex] = '\0'; // Null terminate
                lastDigitTime = currentTime;
                Serial.printf("Added digit '%c' to sequence: '%s'\n", detectedChar, dtmfSequence);
            }
        }
        lastAnalysisTime = currentTime;
    }

    // Check if sequence is complete (timeout or buffer full)
    if (sequenceIndex > 0 &&
        ((sequenceIndex == MAX_SEQUENCE_LENGTH - 1) ||
         (currentTime - lastDigitTime > SEQUENCE_TIMEOUT)))
    {

        Serial.printf("Sequence complete: timeout or buffer full\n");
        return true; // Sequence ready to process
    }

    return false; // No complete sequence yet
}




void setup()
{
    Serial.begin(115200);
    delay(1000); // Give serial time to initialize

    Serial.printf("=== Bowie Phone Starting ===\n");

    // Initialize WiFi first
    initWiFi();

    // Initialize OTA updates (must be after WiFi)
    initOTA();

    // Initialize special commands system
    initializeSpecialCommands();
    
    // Initialize known sequences processor
    initializeKnownProcessor();

    AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning); // setup Audiokit
    
    // Setup audio input and FFT processing
    setupAudioInput();
}

void loop()
{
    // Handle WiFi management (config portal and OTA)
    handleWiFiLoop();
    
    // Skip other processing if in config mode
    if (isConfigMode)
    {
        return;
    }

    // Process audio download queue (non-blocking)
    static unsigned long lastDownloadCheck = 0;
    if (millis() - lastDownloadCheck > 1000) // Check every second
    {
        processAudioDownloadQueue();
        lastDownloadCheck = millis();
    }

    // Handle audio playback
    if (isPlayingAudio)
    {
        player.copy();
        if (!player.isActive())
            // Audio finished playing
            stopAudioPlayback();
    }
    else
    {
        // Only process DTMF when not playing audio
        // Process audio - FFT callback collects frequency data
        copier.copy();
        
        // Check for complete DTMF sequences
        if (checkForDTMFSequence())
        {
            // Process the complete sequence and check for audio playback
            const char* audioPath = processNumberSequence(dtmfSequence);
            
            // If an audio file path was returned, start playback
            if (audioPath && SD.exists(audioPath))
            {
                startAudioPlayback(audioPath);
            }

            // Reset sequence buffer for next sequence
            sequenceIndex = 0;
            dtmfSequence[0] = '\0';
        }
    }
}