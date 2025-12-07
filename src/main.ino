#include <config.h>
#include "dtmf_decoder.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT
#include "AudioTools/Disk/AudioSourceSD.h" // SPI SD mode
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "sequence_processor.h"
#include "special_command_processor.h"
#include "audio_file_manager.h"
#include "wifi_manager.h"
#include "logging.h"
#include "phone_service.h"
#include "audio_player.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>

AudioBoardStream kit(AudioKitEs8388V1); // Audio source
AudioRealFFT fft;                       // or AudioKissFFT
StreamCopy copier(fft, kit);            // copy mic to tfl
int channels = 2;
int samples_per_second = 44100;
int bits_per_sample = 16;

// Audio playback components

// SD Card SPI pins for ESP32-A1S AudioKit
// Working switch config: 2,3,4 UP, 5 DOWN
#define SD_CS   13
#define SD_CLK  14
#define SD_MOSI 15
#define SD_MISO 2

const char *startFilePath="/";
const char* ext="mp3";
// AudioSourceSD will be created in setup() after SPI is initialized
AudioSourceSD *pSource = nullptr;
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

// Flag to track if audio kit has been initialized
static bool audioKitInitialized = false;

// Helper function to setup audio input and FFT (called after audio playback ends)
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

    Logger.printf("\n\n=== Bowie Phone Starting ===\n");
    AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

    // Initialize AudioKit FIRST with sd_active=false
    // This prevents AudioKit from interfering with our SPI SD card pins
    Logger.println("🔧 Initializing AudioKit (TX_MODE)...");
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
    cfg.sd_active = false; // Don't let AudioKit touch SD pins - we'll handle SD ourselves
    if (!kit.begin(cfg))
    {
        Logger.println("❌ Failed to initialize AudioKit");
    }
    else
    {
        Logger.println("✅ AudioKit initialized successfully");
    }

    // Now initialize SD card in SPI mode AFTER AudioKit
    // Working switch config: 2,3,4 UP, 5 DOWN
    Logger.println("🔧 Initializing SD card (SPI mode)...");
    Logger.printf("   Pins: CS=%d, CLK=%d, MOSI=%d, MISO=%d\n", SD_CS, SD_CLK, SD_MOSI, SD_MISO);
    
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    
    bool sdInitialized = false;
    for (int attempt = 1; attempt <= 3 && !sdInitialized; attempt++) {
        Logger.printf("🔧 SD SPI initialization attempt %d/3...\n", attempt);
        delay(attempt * 300);
        
        if (SD.begin(SD_CS, SPI)) {
            uint8_t cardType = SD.cardType();
            if (cardType != CARD_NONE) {
                uint64_t cardSize = SD.cardSize() / (1024 * 1024);
                Logger.printf("✅ SD card initialized (SPI mode, %lluMB)\n", cardSize);
                sdInitialized = true;
            } else {
                Logger.println("❌ No SD card detected");
            }
        } else {
            Logger.println("❌ SD.begin() failed");
        }
    }
    
    if (!sdInitialized) {
        Logger.println("⚠️ SD initialization failed - continuing without SD card");
    } else {
        // Create AudioSourceSD now that SPI is initialized
        // Pass custom SPI instance for ESP32-A1S AudioKit pins
        pSource = new AudioSourceSD(startFilePath, ext, SD_CS, SPI);
        Logger.println("✅ AudioSourceSD created");
    }

    // Initialize audio file manager (SPI mode, SD already initialized)
    initializeAudioFileManager(SD_CS, false, true); // CS pin, false=use SPI (not SD_MMC), true=SD already init

    // Initialize Audio Player with event callback (only if SD initialized)
    if (pSource != nullptr) {
        initAudioPlayer(*pSource, kit, decoder);
        setAudioEventCallback(onAudioEvent);
        Logger.println("✅ Audio player initialized");
    } else {
        Logger.println("⚠️ Audio player not initialized - no SD card");
    }

    // Setup FFT for DTMF detection
    auto tcfg = fft.defaultConfig();
    tcfg.length = 8192;
    tcfg.channels = channels;
    tcfg.sample_rate = samples_per_second;
    tcfg.bits_per_sample = bits_per_sample;
    tcfg.callback = &fftResult;
    fft.begin(tcfg);

    Logger.println("🎤 Audio system ready!");

    // Initialize WiFi with careful error handling
    Logger.println("🔧 Starting WiFi initialization...");
    initWiFi();

    // Initialize OTA updates (must be after WiFi)
    initOTA();

    // Initialize special commands system
    initializeSpecialCommands();

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
    if(!Phone.isRinging())
        Phone.startRinging();

    // Process Phone Service
    Phone.loop();
    
    // Skip other processing if in config mode
    // if (isConfigMode)
    // {
    //     return;
    // }

    // Process audio download queue (non-blocking)
    static unsigned long lastDownloadCheck = 0;
    if (millis() - lastDownloadCheck > 1000) // Check every second
    {
        processAudioDownloadQueue();
        lastDownloadCheck = millis();
    }

    // Only process if phone is off hook
    // if (!Phone.isOffHook())
    // {
    //     return; // Phone is on hook, nothing to do
    // }

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
            if (audioPath && SD.exists(audioPath))
            {
                playAudioPath(audioPath);
            }

            // Reset sequence buffer for next sequence
            sequenceIndex = 0;
            dtmfSequence[0] = '\0';
        }
    }
}