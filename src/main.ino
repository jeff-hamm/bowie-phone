#include "dtmf_decoder.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT
#include "AudioTools/Disk/AudioSourceSDMMC.h" // or AudioSourceIdxSDFAT.h
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "sequence_processor.h"
#include "special_command_processor.h"
#include "audio_file_manager.h"
#include "wifi_manager.h"
#include "logging.h"
#include "phone_service.h"
#include "audio_player.h"
#include <WiFi.h>
#include <SD_MMC.h>

AudioBoardStream kit(AudioKitEs8388V1); // Audio source
AudioRealFFT fft;                       // or AudioKissFFT
StreamCopy copier(fft, kit);            // copy mic to tfl
int channels = 2;
int samples_per_second = 44100;
int bits_per_sample = 16;

// Audio playback components

const char *startFilePath="/";
//const char* ext="mp3";
AudioSourceSDMMC source(startFilePath);
MP3DecoderHelix decoder;  // or change to MP3DecoderMAD

// DTMF sequence configuration
const unsigned long SEQUENCE_TIMEOUT = 2000; // 2 seconds timeout
const int MAX_SEQUENCE_LENGTH = 20;          // Maximum digits in sequence
char dtmfSequence[MAX_SEQUENCE_LENGTH + 1];  // +1 for null terminator
int sequenceIndex = 0;
unsigned long lastDigitTime = 0;
/*
 * Audio Input Device Options (configurable via AUDIO_INPUT_DEVICE build flag):
 * 
 * ADC_INPUT_LINE1 - Microphone only (0x2020)
 * ADC_INPUT_LINE2 - Line in only (0x0408) 
 * ADC_INPUT_LINE3 - Right mic + left line in (0x0420)
 * ADC_INPUT_ALL   - Both microphone and line in (0x0408 | 0x2020) [DEFAULT]
 * 
 * Set via platformio.ini build_flags:
 * -DAUDIO_INPUT_DEVICE=ADC_INPUT_LINE2  ; for line in only
 */
// Helper function to setup audio input and FFT
void setupAudioInput()
{
    // Setup audio input (DTMF detection)
    auto cfg = kit.defaultConfig(RXTX_MODE);
    
    // Configure input device - can be set via build flags
#ifdef AUDIO_INPUT_DEVICE
    cfg.input_device = AUDIO_INPUT_DEVICE;
#else
    cfg.input_device = ADC_INPUT_ALL; // Default: both microphone and line in
#endif
    
    cfg.channels = channels;
    cfg.sample_rate = samples_per_second;
    cfg.bits_per_sample = bits_per_sample;
    cfg.sd_active = false; // SD card initialized manually in setup()
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

// Audio event callback - called when audio starts/stops
void onAudioEvent(bool started)
{
    if (!started)
    {
        // Audio finished - restart input audio system and FFT processing
        setupAudioInput();
        Logger.println("🎤 Resumed audio input and FFT processing");
    }
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
    delay(2000); // Give serial time to initialize

    // Initialize logging system first
    Logger.addLogger(Serial);

    Logger.printf("=== Bowie Phone Starting ===\n");

    // Initialize SD_MMC in 1-bit mode (more reliable on some boards)
    Logger.println("🔧 Initializing SD_MMC (1-bit mode)...");
    if (!SD_MMC.begin("/sdcard", true)) {  // true = 1-bit mode
        Logger.println("❌ Failed to initialize SD_MMC");
    } else if (SD_MMC.cardType() == CARD_NONE) {
        Logger.println("❌ No SD card detected");
    } else {
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Logger.printf("✅ SD_MMC initialized (1-bit mode, %lluMB)\n", cardSize);
    }

    // Add more startup delay for system stabilization
    Logger.println("🔧 Allowing system to stabilize...");
    delay(3000);

    // Initialize WiFi with careful error handling
    Logger.println("🔧 Starting WiFi initialization...");
    initWiFi();

    // Initialize OTA updates (must be after WiFi)
    initOTA();

    // Initialize special commands system
    initializeSpecialCommands();
    
    // Initialize audio file manager
    initializeAudioFileManager(true); // true = use SD_MMC (already initialized)

    // Initialize Phone Service
    Phone.begin();
    Phone.setHookCallback([](bool isOffHook) {
        if (isOffHook) {
            // Handle off-hook event - play dial tone
            Logger.println("⚡ Event: Phone Off Hook - Playing Dial Tone");
            playAudioBySequence("dialtone");
        } else {
            // Handle on-hook event - stop audio, reset state
            Logger.println("⚡ Event: Phone On Hook");
            stopAudio();
            // Reset DTMF sequence
            sequenceIndex = 0;
            dtmfSequence[0] = '\0';
        }
    });

    AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning); // setup Audiokit
    
    // Initialize Audio Player with event callback
    initAudioPlayer(source, kit, decoder);
    setAudioEventCallback(onAudioEvent);
    
    // Setup audio input and FFT processing
    setupAudioInput();
    
    Logger.println("✅ Bowie Phone Ready!");
}

void loop()
{
    // Handle WiFi management (config portal and OTA)
    handleWiFiLoop();

    // Ensure audio catalog/downloads kick off once WiFi is available
    static bool audioDownloadComplete = false;
    static unsigned long lastAudioDownloadAttempt = 0;
    const unsigned long audioDownloadRetryMs = 30000; // retry every 30s if needed

    if (!audioDownloadComplete && WiFi.status() == WL_CONNECTED)
    {
        unsigned long now = millis();
        if (lastAudioDownloadAttempt == 0 || (now - lastAudioDownloadAttempt) > audioDownloadRetryMs)
        {
            lastAudioDownloadAttempt = now;
            if (downloadAudio())
            {
                audioDownloadComplete = true;
            }
        }
    }
    
    // Process Phone Service
    Phone.loop();
    
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

    // Only process if phone is off hook
    if (!Phone.isOffHook())
    {
        return; // Phone is on hook, nothing to do
    }

    // Handle audio playback
    if (isAudioActive())
    {
        processAudio();
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
            if (audioPath && SD_MMC.exists(audioPath))
            {
                playAudioPath(audioPath);
            }

            // Reset sequence buffer for next sequence
            sequenceIndex = 0;
            dtmfSequence[0] = '\0';
        }
    }
}