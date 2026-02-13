#include <config.h>
#include "dtmf_decoder.h"
#include "dtmf_goertzel.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h" // or AudioKissFFT
#if SD_USE_MMC
  #include "AudioTools/Disk/AudioSourceSDMMC.h"
#else
  #include "AudioTools/Disk/AudioSourceSD.h"
#endif
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "sequence_processor.h"
#include "special_command_processor.h"
#include "audio_file_manager.h"
#include "audio_key_registry.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "remote_logger.h"
#include "logging.h"
#include "phone_service.h"
#include "extended_audio_player.h"
#include "notifications.h"
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
ExtendedAudioPlayer audioPlayer = getExtendedAudioPlayer();

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


// DTMF sequence checking moved to sequence_processor.cpp

void setup()
{
    Serial.begin(115200);
    delay(2000); // Give serial time to initialize

    // Initialize logging system first
    Logger.addLogger(Serial);

    Logger.printf("\n\n=== Bowie Phone Starting ===\n");
    
#ifdef RUN_SD_DEBUG_FIRST
    // Build flag to run SD debug before ANY other initialization
    // This helps diagnose SD issues without interference from other systems
    // Build with: -DRUN_SD_DEBUG_FIRST
    Logger.println("🔧 RUN_SD_DEBUG_FIRST enabled - running SD diagnostics...");
    Logger.println("   Waiting 3 seconds for serial connection...");
    delay(3000);
    
    // Minimal AudioKit init for SD testing
    AudioBoardStream testKit(AudioKitEs8388V1);
    auto testCfg = testKit.defaultConfig(RXTX_MODE);
    testCfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    testCfg.sd_active = false;
    if (testKit.begin(testCfg)) {
        Logger.println("   AudioKit initialized for testing");
    }
    
    performSDCardDebug();
    
    Logger.println();
    Logger.println("═══════════════════════════════════════════");
    Logger.println("SD DEBUG COMPLETE - Device will halt");
    Logger.println("Review results above, then:");
    Logger.println("  1. Remove -DRUN_SD_DEBUG_FIRST flag");
    Logger.println("  2. Update config.h with working pins");
    Logger.println("  3. Rebuild and flash");
    Logger.println("═══════════════════════════════════════════");
    while (true) { delay(1000); }  // Halt here
#endif
    
    // Initialize notification system early (before WiFi so we can show status)
    initNotifications();
        
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
    source = initializeAudioFileManager();

    // Initialize Audio Player with event callback
    // Register MP3 decoder

    audioPlayer.setRegistry(&getAudioKeyRegistry());
    
    // Register decoders with the audio player
    audioPlayer.addDecoder(mp3_decoder, "audio/mpeg");
    // Register WAV decoder with all common MIME types
    // WAV files can be detected as audio/wav, audio/wave, or audio/vnd.wave
    audioPlayer.addDecoder(wav_decoder, "audio/wav");
    audioPlayer.addDecoder(wav_decoder, "audio/vnd.wave");
    audioPlayer.addDecoder(wav_decoder, "audio/wave");
    audioPlayer.begin(kit, source != nullptr);
    // Initialize DTMF decoder (FFT and copier configuration)
    initDtmfDecoder(fft, copier);
    
    // Initialize Goertzel-based DTMF decoder for efficient dial tone detection
    // Goertzel is O(n*k) vs FFT O(n log n), much faster for specific frequencies
    initGoertzelDecoder(goertzel, goertzelCopier);
    
    // Start Goertzel processing on separate FreeRTOS task (core 0)
    // This prevents blocking the main loop (audio runs on core 1)
    startGoertzelTask(goertzelCopier);

    // Initialize WiFi with careful error handling
    Logger.println("🔧 Starting WiFi initialization...");


    initWiFi([]() {
        // This is called when WiFi successfully connects
        // Note: WiFi/Tailscale LED notifications are handled in wifi_manager/tailscale_manager
        
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
            audioPlayer.playAudioKey("dialtone");
        } else {
            // Handle on-hook event - stop audio, reset state
            Logger.println("⚡ Event: Phone On Hook");
            audioPlayer.stop();
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
        audioPlayer.playAudioKey("dialtone");
    }
}

void loop()
{
    // if(!Phone.isRinging())
    //     Phone.startRinging();

    // Process Phone Service
    static unsigned long lastMaintenanceCheck = 0;
    Phone.loop();
    if (Phone.isOffHook())
    {
        // Check for off-hook timeout (play warning tone if inactive too long)
        static bool offHookWarningPlayed = false;
        unsigned long now = millis();
        unsigned long lastActivity = max(getLastDigitTime(), audioPlayer.getLastActive());
        
        if (!offHookWarningPlayed && lastActivity > 0 && (now - lastActivity) >= OFF_HOOK_TIMEOUT_MS) {
            Logger.println("⚠️ Off-hook timeout - playing warning tone");
            audioPlayer.playAudioKey("off_hook");
            offHookWarningPlayed = true;
        }
        
        // Reset warning flag when there's activity
        if (getLastDigitTime() > lastActivity || audioPlayer.isActive()) {
            offHookWarningPlayed = false;
        }
        
        // Handle audio playback FIRST - highest priority for smooth audio
        if (audioPlayer.isActive())
        {
            // For non-dial-tone audio, normal processing with DTMF detection
            audioPlayer.copy();
            if (!audioPlayer.isAudioKeyPlaying("dialtone")) {
                return;
            }
        }
        
#ifdef USE_GOERTZEL_ONLY
        // Goertzel runs on separate task - just check for detected keys
        char goertzelKey = getGoertzelKey();
        if (goertzelKey != 0) {
            addDtmfDigit(goertzelKey);
            
            // readDTMFSequence now handles playback internally
            readDTMFSequence(true);
        }
#else
        static unsigned long lastDTMFCheck = 0;
        if (isDialTone)
        {
            // Goertzel runs on separate task - just check for detected keys
            char goertzelKey = getGoertzelKey();
            if (goertzelKey != 0) {
                // Feed key to sequence processor
                addDtmfDigit(goertzelKey);
                
                // Skip FFT processing - readDTMFSequence handles playback internally
                readDTMFSequence(true);
            }
        }
        else if (!player.isActive())
        {
            // Use FFT for idle (no audio) - needed for summed frequency detection on some phones
            lastDTMFCheck = millis();
            copier.copy();

            // readDTMFSequence handles playback internally via PlaylistRegistry
            readDTMFSequence();
        }
#endif  


    } else {
        // Handle phone home periodic check-in (for remote OTA, status, etc.)
        handlePhoneHomeLoop();
    }

    auto limit = isReadingSequence() ? 10 : 100;
    unsigned long now = millis();
        
    // Rate limit maintenance operations to every 100ms while not reading sequence
    if (now - lastMaintenanceCheck >= limit) {
        lastMaintenanceCheck = now;
        
        // Handle telnet server (process incoming connections)
        telnet.loop();

        // Handle WiFi management (config portal and OTA)
        handleWiFiLoop();
#ifdef DEBUG
        // Process debug commands from Serial and Telnet
        processDebugInput(Serial);
        processDebugInput(telnet);
#endif
        // Handle Tailscale VPN keepalive/reconnection and remote logging
        handleTailscaleLoop();
    }
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