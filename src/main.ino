#include <config.h>
#include "dtmf_decoder.h"
#include "dtmf_goertzel.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT
#include "AudioTools/Disk/AudioSourceSD.h" // SPI SD mode
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "sequence_processor.h"
#include "special_command_processor.h"
#include "audio_file_manager.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "remote_logger.h"
#include "logging.h"
#include "phone_service.h"
#include "audio_player.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <ESPTelnetStream.h>

ESPTelnetStream telnet;  // Telnet server for remote logging
AudioBoardStream kit(AudioKitEs8388V1); // Audio source
AudioSource *source = nullptr;          // to be initialized in setup()
AudioRealFFT fft;                       // or AudioKissFFT
StreamCopy copier(fft, kit);            // copy mic to FFT
MultiDecoder multi_decoder;
MP3DecoderHelix mp3_decoder;
WAVDecoder wav_decoder;

// Goertzel-based DTMF detection (more efficient during dial tone)
GoertzelStream goertzel;                // Goertzel detector
StreamCopy goertzelCopier(goertzel, kit); // copy mic to Goertzel

// Key pins for AudioKit board (active LOW)
// These may conflict with other functions depending on DIP switch settings
const int KEY_PINS[] = {36, 13, 19, 23, 18, 5};  // KEY1-KEY6
const int NUM_KEYS = 6;

// Firmware update mode key - KEY3 (GPIO 19) is safest as it doesn't conflict with SD card
// Hold this key during boot to enter firmware update mode
const int FIRMWARE_UPDATE_KEY = 19;  // GPIO 19 = KEY3
// Flag to track if audio board was initialized
static bool audioKitInitialized = false;

void initDtmfDecoder();
// SD Card SPI pins for ESP32-A1S AudioKit
// Working switch config: 2,3,4 UP, 5 DOWN
#define SD_CS   13
#define SD_CLK  14
#define SD_MOSI 15
#define SD_MISO 2

const char *startFilePath="/audio";


// DTMF sequence checking moved to sequence_processor.cpp

void setup()
{
    Serial.begin(115200);
    delay(2000); // Give serial time to initialize

    // Initialize logging system first
    Logger.addLogger(Serial);

    Logger.printf("\n\n=== Bowie Phone Starting ===\n");
    
    // Check if firmware update key (KEY3 / GPIO19) is held during boot
    // This provides a hardware way to enter bootloader mode for flashing
    pinMode(FIRMWARE_UPDATE_KEY, INPUT_PULLUP);
    delay(50);  // Debounce
    if (digitalRead(FIRMWARE_UPDATE_KEY) == LOW) {
        Logger.println("🔧 KEY3 (GPIO19) held at boot - entering firmware update mode...");
        enterFirmwareUpdateMode();
        // Will not return - device restarts into bootloader
    }
    
    
    // Reduce AudioTools library logging to minimize noise
    AudioToolsLogger.begin(Logger, AUDIOTOOLS_LOG_LEVEL);

    // Initialize AudioKit FIRST with sd_active=false
    // This prevents AudioKit from interfering with our SPI SD card pins
    Logger.println("🔧 Initializing AudioKit (RXTX_MODE)...");
    auto cfg = kit.defaultConfig(RXTX_MODE);
    cfg.setAudioInfo(AUDIO_INFO_DEFAULT());
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
        audioKitInitialized = false;
    }
    else
    {
        Logger.println("✅ AudioKit initialized successfully");
        audioKitInitialized = true;
        
        // Set input volume/gain for DTMF detection
        // Volume range is 0-100 (percentage)
        kit.setInputVolume(AUDIOKIT_INPUT_VOLUME);
        Logger.printf("🔊 Input volume set to %d%%\n", AUDIOKIT_INPUT_VOLUME);
    }

    // Initialize audio file manager (handles SD card initialization internally)
    // Returns AudioSourceSD pointer if SD card is available
    source = initializeAudioFileManager(SD_CS, false, SD_CLK, SD_MOSI, SD_MISO, startFilePath);

    // Initialize Audio Player with event callback
    // Register MP3 decoder
    multi_decoder.addDecoder(mp3_decoder, "audio/mpeg");
    // Register WAV decoder with all common MIME types
    // WAV files can be detected as audio/wav, audio/wave, or audio/vnd.wave
    multi_decoder.addDecoder(wav_decoder, "audio/wav");
    multi_decoder.addDecoder(wav_decoder, "audio/vnd.wave");
    multi_decoder.addDecoder(wav_decoder, "audio/wave");
    
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
        initAudioUrlPlayer(kit, multi_decoder);
        Logger.println("✅ Audio player initialized (URL streaming mode)");
    }
#endif

    // Initialize DTMF decoder (FFT and copier configuration)
    initDtmfDecoder(fft, copier);
    
    // Initialize Goertzel-based DTMF decoder for efficient dial tone detection
    // Goertzel is O(n*k) vs FFT O(n log n), much faster for specific frequencies
    initGoertzelDecoder(goertzel, goertzelCopier);

    // Initialize WiFi with careful error handling
    Logger.println("🔧 Starting WiFi initialization...");
    // Check if Tailscale should be enabled based on boot key/saved state
    // This sets the internal flag in tailscale_manager (wifi_manager uses this)
    shouldEnableTailscale();

    initWiFi([]() {
        // This is called when WiFi successfully connects
        // Note: Tailscale/VPN is now initialized automatically by wifi_manager
        
        // Start telnet server for remote logging
        telnet.onConnect([](String ip) {
            Logger.printf("📡 Telnet client connected from: %s\n", ip.c_str());
            Logger.addLogger(telnet);  // Add telnet as a log output stream
        });
        telnet.onConnectionAttempt([](String ip) {
            Logger.printf("📡 Telnet connection attempt from: %s\n", ip.c_str());
        });
        telnet.onReconnect([](String ip) {
            Logger.printf("📡 Telnet client reconnected from: %s\n", ip.c_str());
        });
        telnet.onDisconnect([](String ip) {
            Logger.printf("📡 Telnet client disconnected from: %s\n", ip.c_str());
            Logger.removeLogger(telnet);  // Remove from logger streams
        });
        
        if (telnet.begin(23)) {
            Logger.println("✅ Telnet server started on port 23");
        } else {
            Logger.println("❌ Failed to start telnet server");
        }
        
        // Download audio catalog (non-critical, can fail)
        // Uses cached DNS if WireGuard broke public DNS
        Logger.println("🌐 Downloading audio catalog...");
        if (downloadAudio()) {
            Logger.println("✅ Audio catalog downloaded successfully");
        } else {
            Logger.println("⚠️ Audio catalog download failed - will retry later");
        }
    });
    
    // Skip Tailscale reconnection when phone is off hook (blocking call interrupts DTMF)
    setTailscaleSkipCallback([]() -> bool {
        return Phone.isOffHook();
    });

    // Initialize special commands system
    initializeSpecialCommands();

    // Initialize Phone Service
    Phone.begin();
    Phone.setHookCallback([](bool isOffHook) {
        if (isOffHook) {
            // Handle off-hook event - play dial tone
            Logger.println("⚡ Event: Phone Off Hook - Playing Dial Tone");
            playAudioKey("dialtone");
        } else {
            // Handle on-hook event - stop audio, reset state
            Logger.println("⚡ Event: Phone On Hook");
            stopAudio();
            resetDTMFSequence(); // Clear any partial DTMF sequence
        }
    });
    
    Logger.println("✅ Bowie Phone Ready!");
    
#ifdef DEBUG
    Logger.println("🔧 Serial Debug Mode ACTIVE - type 'help' for commands");
#endif
    
    // Check if phone is already off hook at boot - play dial tone
    if (Phone.isOffHook())
    {
        Logger.println("📞 Phone is off hook at boot - playing dial tone");
        playAudioKey("dialtone");
    }
}

void loop()
{
    // if(!Phone.isRinging())
    //     Phone.startRinging();

    // Process Phone Service
    Phone.loop();
    if (Phone.isOffHook())
    {
        // Handle audio playback FIRST - highest priority for smooth audio
        if (isAudioActive())
        {
            // For non-dial-tone audio, normal processing with DTMF detection
            processAudio();
        }
        
        // Adaptive DTMF detection:
        // - USE_GOERTZEL_ONLY: Use Goertzel for all DTMF detection (simpler, works better on some phones)
        // - Otherwise: Use Goertzel during dial tone, FFT otherwise
        static unsigned long lastDTMFCheck = 0;
        bool isDialTone = isAudioKeyPlaying("dialtone");
        bool isOtherAudio = isAudioActive() && !isDialTone;
        
#ifdef USE_GOERTZEL_ONLY
        // Use Goertzel for ALL DTMF detection (dial tone, idle, and during audio)
        if (!isOtherAudio)
        {
            goertzelCopier.copy();
            
            char goertzelKey = getGoertzelKey();
            if (goertzelKey != 0) {
                simulateDTMFDigit(goertzelKey);
                
                const char *audioPath = readDTMFSequence(true);
                if (audioPath)
                {
                    playAudioPath(audioPath);
                }
            }
        }
#else
        if (isDialTone)
        {
            // Use Goertzel for dial tone and idle - more efficient
            goertzelCopier.copy();
            
            // Check for Goertzel-detected key
            char goertzelKey = getGoertzelKey();
            if (goertzelKey != 0) {
                // Feed key to sequence processor
                simulateDTMFDigit(goertzelKey);
                
                // Skip FFT processing - we're using Goertzel during dial tone
                const char *audioPath = readDTMFSequence(true);
                if (audioPath)
                {
                    playAudioPath(audioPath);
                }
            }
        }
        else if (!isAudioActive())
        {
            // Use FFT for idle (no audio) - needed for summed frequency detection on some phones
            lastDTMFCheck = millis();
            copier.copy();

            const char *audioPath = readDTMFSequence();
            if (audioPath)
            {
                playAudioPath(audioPath);
            }
        }
#endif
    }

    // Handle telnet server (process incoming connections)
    telnet.loop();

#ifdef DEBUG
    // Process debug commands from Serial and Telnet
    processDebugInput(Serial);
    processDebugInput(telnet);
#endif

    // Handle WiFi management (config portal and OTA)
    handleWiFiLoop();

    // Handle Tailscale VPN keepalive/reconnection and remote logging
    handleTailscaleLoop();

    // Handle phone home periodic check-in (for remote OTA, status, etc.)
    handlePhoneHomeLoop();

    // Process audio download queue (non-blocking, rate-limited internally)
    processAudioDownloadQueue();
}

// or AudioKissFFT

// Initialize FFT and copier for DTMF detection
void initDtmfDecoder(AudioRealFFT &fft, StreamCopy &copier)
{
    // Setup FFT for DTMF detection
    // DTMF tones are 40-100ms, so we need fast time resolution
    // 2048 samples @ 44100Hz = 46ms window with ~21.5 Hz bin width
    // This is sufficient since DTMF frequencies are at least 70Hz apart
    auto tcfg = fft.defaultConfig();
    tcfg.length = 2048; // 46ms window - good for DTMF timing
    tcfg.stride = 1024;
    tcfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    tcfg.callback = &fftResult;
    // Window function selection - override via build flags:
    // -DFFT_WINDOW_HAMMING, -DFFT_WINDOW_HANN, -DFFT_WINDOW_BLACKMAN,
    // -DFFT_WINDOW_BLACKMAN_HARRIS (default), -DFFT_WINDOW_BLACKMAN_NUTTALL
#if defined(FFT_WINDOW_HAMMING)
    tcfg.window_function = new BufferedWindow(new Hamming());
    Logger.println("🎵 FFT Window: Hamming (-43dB side lobes)");
#elif defined(FFT_WINDOW_HANN)
    tcfg.window_function = new BufferedWindow(new Hann());
    Logger.println("🎵 FFT Window: Hann (-31dB side lobes)");
#elif defined(FFT_WINDOW_BLACKMAN)
    tcfg.window_function = new BufferedWindow(new Blackman());
    Logger.println("🎵 FFT Window: Blackman (-58dB side lobes)");
#elif defined(FFT_WINDOW_BLACKMAN_NUTTALL)
    tcfg.window_function = new BufferedWindow(new BlackmanNuttall());
    Logger.println("🎵 FFT Window: BlackmanNuttall (-98dB side lobes)");
#else // Default: FFT_WINDOW_BLACKMAN_HARRIS
    tcfg.window_function = new BufferedWindow(new BlackmanHarris());
    Logger.println("🎵 FFT Window: BlackmanHarris (-92dB side lobes)");
#endif
    fft.begin(tcfg);

    // Resize copier buffer for better audio throughput
    copier.resize(AUDIO_COPY_BUFFER_SIZE);
    Logger.printf("🎤 Copier buffer resized to %d bytes\n", AUDIO_COPY_BUFFER_SIZE);
}