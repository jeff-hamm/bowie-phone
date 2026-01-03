#include <config.h>
#include "dtmf_decoder.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT
#include "AudioTools/Disk/AudioSourceSD.h" // SPI SD mode
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#define DEBUG
#include "sequence_processor.h"
#include "special_command_processor.h"
#include "audio_file_manager.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "logging.h"
#include "phone_service.h"
#include "audio_player.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>

AudioBoardStream kit(AudioKitEs8388V1); // Audio source
AudioSourceSD *source = nullptr;        // to be initialized in setup()
AudioRealFFT fft;                       // or AudioKissFFT
StreamCopy copier(fft, kit);            // copy mic to tfl
MultiDecoder multi_decoder;
MP3DecoderHelix mp3_decoder;
WAVDecoder wav_decoder;
int channels = 2;
int samples_per_second = 44100;
int bits_per_sample = 16;

// Serial debug mode buffer
#ifdef DEBUG
static char serialDebugBuffer[64];
static int serialDebugPos = 0;

// Process serial input for debug commands
// Supported commands:
//   digits (0-9, #, *) -> simulate DTMF sequences
//   <number>hz -> inject FFT test tone (e.g., "697hz")
//   hook -> toggle hook state
//   level <0|1|2> -> set log level (quiet/normal/debug)
void processSerialDebugInput() {
    while (Serial.available()) {
        char c = Serial.read();
        
        // Handle newline/carriage return - process command
        if (c == '\n' || c == '\r') {
            if (serialDebugPos > 0) {
                serialDebugBuffer[serialDebugPos] = '\0';
                String cmd = String(serialDebugBuffer);
                cmd.trim();
                
                if (cmd.equalsIgnoreCase("hook")) {
                    // Toggle hook state
                    bool newState = !Phone.isOffHook();
                    Phone.setOffHook(newState);
                    Logger.printf("🔧 [DEBUG] Hook toggled to: %s\n", newState ? "OFF HOOK" : "ON HOOK");
                }
                else if (cmd.startsWith("level ")) {
                    // Set log level
                    int level = cmd.substring(6).toInt();
                    if (level >= 0 && level <= 2) {
                        Logger.setLogLevel((LogLevel)level);
                        Logger.printf("🔧 [DEBUG] Log level set to: %d\n", level);
                    }
                }
                else if (cmd.endsWith("hz") || cmd.endsWith("Hz") || cmd.endsWith("HZ")) {
                    // Parse frequency for FFT test (e.g., "697hz")
                    String freqStr = cmd.substring(0, cmd.length() - 2);
                    int freq = freqStr.toInt();
                    if (freq > 0 && freq < 20000) {
                        Logger.printf("🔧 [DEBUG] FFT test tone: %d Hz (not yet implemented)\n", freq);
                        // TODO: Generate synthetic FFT result for this frequency
                    }
                }
                else {
                    // Treat as DTMF digit sequence
                    bool validSequence = true;
                    for (size_t i = 0; i < cmd.length() && validSequence; i++) {
                        char digit = cmd.charAt(i);
                        if (!((digit >= '0' && digit <= '9') || digit == '#' || digit == '*')) {
                            validSequence = false;
                        }
                    }
                    
                    if (validSequence && cmd.length() > 0) {
                        Logger.printf("🔧 [DEBUG] Simulating DTMF sequence: %s\n", cmd.c_str());
                        // Inject each digit into the sequence processor
                        for (size_t i = 0; i < cmd.length(); i++) {
                            simulateDTMFDigit(cmd.charAt(i));
                        }
                    } else if (cmd.length() > 0) {
                        Logger.printf("🔧 [DEBUG] Unknown command: %s\n", cmd.c_str());
                        Logger.println("🔧 [DEBUG] Available commands: hook, level <0-2>, <digits>, <freq>hz");
                    }
                }
                
                serialDebugPos = 0;
            }
        }
        else if (serialDebugPos < (int)sizeof(serialDebugBuffer) - 1) {
            serialDebugBuffer[serialDebugPos++] = c;
        }
    }
}
#endif

// Audio playback components

// SD Card SPI pins for ESP32-A1S AudioKit
// Working switch config: 2,3,4 UP, 5 DOWN
#define SD_CS   13
#define SD_CLK  14
#define SD_MOSI 15
#define SD_MISO 2

const char *startFilePath="/audio";
static bool audioKitInitialized = false;


// DTMF sequence checking moved to sequence_processor.cpp

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
    Logger.println("🔧 Initializing AudioKit (RXTX_MODE)...");
    auto cfg = kit.defaultConfig(RXTX_MODE);
    cfg.channels = channels;
    cfg.sample_rate = samples_per_second;
    cfg.bits_per_sample = bits_per_sample;
    // Configure input device - can be set via build flags
#ifdef AUDIO_INPUT_DEVICE
    cfg.input_device = AUDIO_INPUT_DEVICE;
#else
    cfg.input_device = ADC_INPUT_ALL; // Default: both microphone and line in
#endif

    cfg.sd_active = false; // SD card initialized manually in setup()
    //    cfg.sd_active = false; // Don't let AudioKit touch SD pins - we'll handle SD ourselves
    if (!kit.begin(cfg))
    {
        Logger.println("❌ Failed to initialize AudioKit");
    }
    else
    {
        Logger.println("✅ AudioKit initialized successfully");
        
        // Set input volume/gain to maximum for DTMF detection
        // Volume range is 0-100 (percentage)
        kit.setInputVolume(100);
        Logger.println("🔊 Input volume set to 100%");
        
        // Set output volume to maximum
        kit.setVolume(100);
        Logger.println("🔊 Output volume set to 100%");
    }

    // // Now initialize SD card in SPI mode AFTER AudioKit
    // // Working switch config: 2,3,4 UP, 5 DOWN
    // Logger.println("🔧 Initializing SD card (SPI mode)...");
    // Logger.printf("   Pins: CS=%d, CLK=%d, MOSI=%d, MISO=%d\n", SD_CS, SD_CLK, SD_MOSI, SD_MISO);
    
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    
    bool sdCardAvailable = false;
    for (int attempt = 1; attempt <= 3 && !sdCardAvailable; attempt++) {
        Logger.printf("🔧 SD SPI initialization attempt %d/3...\n", attempt);
        delay(attempt * 300);
        
        if (SD.begin(SD_CS, SPI)) {
            uint8_t cardType = SD.cardType();
            if (cardType != CARD_NONE) {
                uint64_t cardSize = SD.cardSize() / (1024 * 1024);
                Logger.printf("✅ SD card initialized (SPI mode, %lluMB)\n", cardSize);
                sdCardAvailable = true;
            } else {
                Logger.println("❌ No SD card detected");
            }
        } else {
            Logger.println("❌ SD.begin() failed");
        }
    }
    
    if (!sdCardAvailable) {
        Logger.println("⚠️ SD initialization failed - continuing without SD card");
    } else {
//        Create AudioSourceSD now that SPI is initialized
//        Pass custom SPI instance for ESP32-A1S AudioKit pins
        source = new AudioSourceSD(startFilePath, "na", SD_CS, SPI);
        Logger.println("✅ AudioSourceSD created");
   }

    // Initialize audio file manager
    // Pass sdCardAvailable to indicate whether SD storage is accessible
    initializeAudioFileManager(SD_CS, false, sdCardAvailable);

    // Initialize Audio Player with event callback
    multi_decoder.addDecoder(mp3_decoder, "audio/mpeg");
    multi_decoder.addDecoder(wav_decoder, "audio/wav");
    
#if FORCE_URL_STREAMING
    // Force URL streaming mode even if SD card is available
    Logger.println("🌐 FORCE_URL_STREAMING enabled - using URL streaming mode");
    initAudioPlayerURLMode(kit, multi_decoder);
    Logger.println("✅ Audio player initialized (URL streaming mode - forced)");
#else
    if (source != nullptr) {
        initAudioPlayer(*source, kit, multi_decoder);
        //        setAudioEventCallback(onAudioEvent);
        Logger.println("✅ Audio player initialized (SD card mode)");
    } else {
        // SD card not available - use URL streaming mode
        Logger.println("🌐 SD card not available - using URL streaming mode");
        initAudioPlayerURLMode(kit, multi_decoder);
        Logger.println("✅ Audio player initialized (URL streaming mode)");
    }
#endif

    // // Setup FFT for DTMF detection
    auto tcfg = fft.defaultConfig();
    tcfg.length = 8192;
    tcfg.channels = channels;
    tcfg.sample_rate = samples_per_second;
    tcfg.bits_per_sample = bits_per_sample;
    tcfg.callback = &fftResult;
    fft.begin(tcfg);

    // Resize copier buffer for better audio throughput
    copier.resize(AUDIO_COPY_BUFFER_SIZE);
    Logger.printf("🎤 Copier buffer resized to %d bytes\n", AUDIO_COPY_BUFFER_SIZE);

    Logger.println("🎤 Audio system ready!");

    // Initialize WiFi with careful error handling
    Logger.println("🔧 Starting WiFi initialization...");
    // Pass callback to connect Tailscale after WiFi connects
    initWiFi([]() {
        // This is called when WiFi successfully connects
        // Download sequences BEFORE Tailscale - DNS works at this point
        Logger.println("🌐 Downloading audio catalog before VPN...");
        if (downloadAudio()) {
            Logger.println("✅ Audio catalog downloaded successfully");
        } else {
            Logger.println("⚠️ Audio catalog download failed - will retry later");
        }
        
        // Now initialize Tailscale VPN
        Logger.println("🔐 WiFi connected - initializing Tailscale...");
        initTailscaleFromConfig();
    });
    
    // Skip Tailscale reconnection when phone is off hook (blocking call interrupts DTMF)
    setTailscaleSkipCallback([]() -> bool {
        return Phone.isOffHook();
    });

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
            resetDTMFSequence(); // Clear any partial DTMF sequence
        }
    });
    
    Logger.println("✅ Bowie Phone Ready!");
    
    // Check if phone is already off hook at boot - play dial tone
    if (Phone.isOffHook())
    {
        Logger.println("📞 Phone is off hook at boot - playing dial tone");
        playAudioBySequence("dialtone");
    }
}

void loop()
{
    // Handle WiFi management (config portal and OTA)
    handleWiFiLoop();

    // Handle Tailscale VPN keepalive/reconnection
    handleTailscaleLoop();

#ifdef DEBUG
    // Process serial debug commands
    processSerialDebugInput();
#endif

    // Audio catalog is downloaded in WiFi callback before Tailscale starts
    // This retry logic is for edge cases where initial download failed
    static bool audioDownloadComplete = false;
    static unsigned long lastAudioDownloadAttempt = 0;
    const unsigned long audioDownloadRetryMs = 60000; // retry every 60s if needed

    // Only retry if we have no audio files loaded and Tailscale is not connected
    // (DNS won't work through WireGuard without extra config)
    if (!audioDownloadComplete && getAudioKeyCount() == 0 && 
        WiFi.status() == WL_CONNECTED && !isTailscaleConnected())
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
    
    // Mark complete if we have audio files loaded (from callback or SD cache)
    if (!audioDownloadComplete && getAudioKeyCount() > 0) {
        audioDownloadComplete = true;
    }
    // if(!Phone.isRinging())
    //     Phone.startRinging();

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
    if (!Phone.isOffHook())
    {
        return; // Phone is on hook, nothing to do
    }

    // Always process audio input for DTMF detection (even during playback in RXTX_MODE)
    size_t bytesCopied = copier.copy();
    if (bytesCopied > 0) {
        Logger.debugf("🎤 Copied %u bytes to FFT\n", bytesCopied);
    }

    // Handle audio playback
    if (isAudioActive())
    {
        processAudio();
    }

    // Check for complete DTMF sequences and play audio if ready
    const char* audioPath = readDTMFSequence();
    if (audioPath)
    {
        playAudioPath(audioPath);
    }
}