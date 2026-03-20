#include <config.h>
#include "dtmf_goertzel.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#if SD_USE_MMC
  #include "AudioTools/Disk/AudioSourceSDMMC.h"
#else
  #include "AudioTools/Disk/AudioSourceSD.h"
#endif
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecAACHelix.h"
#include "AudioTools/AudioCodecs/ContainerM4A.h"
#include "AudioTools/CoreAudio/AudioMetaData/MimeDetector.h"
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
#include "tone_generators.h"
#include "crash_counter.h"

AudioBoardStream kit(AudioKitEs8388V1); // Audio source
AudioSource *source = nullptr;          // to be initialized in setup()
MultiDecoder multi_decoder;
MP3DecoderHelix mp3_decoder;
WAVDecoder wav_decoder;
AACDecoderHelix aac_decoder;       // Decodes AAC frames extracted from M4A
MultiDecoder m4a_inner_decoder;    // Routes demuxed AAC frames to aac_decoder
ContainerM4A m4a_decoder;          // Demuxes M4A/MP4 container
ExtendedAudioPlayer& audioPlayer = getExtendedAudioPlayer();
AudioKeyRegistry& audioKeyRegistry = getAudioKeyRegistry();
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


// DTMF sequence checking moved to sequence_processor.cpp

void setup()
{
    Serial.begin(115200);
    delay(2000); // Give serial time to initialize

    // Initialize logging system first
    Logger.addLogger(Serial);
    // Add remote logger early so pre-VPN boot logs are buffered and shipped later
    Logger.addLogger(RemoteLogger);
    addDebugStream(Serial);


    Logger.printf("\n\n=== Bowie Phone Starting ===\n");
    Logger.printf("🔧 Firmware: %s  Build: %s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);

    // === CRASH COUNTER — boot-loop safe mode ===
    inSafeMode = evaluateCrashCounter();
    if (inSafeMode) {
        Logger.printf("\n🛡️ SAFE MODE: %d consecutive crashes detected (threshold %d)\n",
                      rtcCrashCount, CRASH_SAFE_MODE_THRESHOLD);
        Logger.println("🛡️ Only WiFi + OTA active. Will retry normal boot in "
                       + String(SAFE_MODE_REBOOT_MS / 1000) + "s.");

        initNotifications();

        // Firmware update key still checked in safe mode
        pinMode(FIRMWARE_UPDATE_KEY, INPUT_PULLUP);
        delay(50);
        if (digitalRead(FIRMWARE_UPDATE_KEY) == LOW) {
            Logger.println("🔧 KEY3 held — entering firmware update mode...");
            rtcCrashCount = 0;  // BUG-5 fix: clear counter so next boot isn't trapped in safe mode
            enterFirmwareUpdateMode();
        }

        initWiFi();

        // BUG-1 fix: enable WDT in safe mode so a hang in WiFi/OTA doesn't brick the device
        esp_task_wdt_init(15, true);  // 15 s — generous for network ops
        esp_task_wdt_add(NULL);

        Logger.println("🛡️ Safe mode ready — OTA at /update, retry timer running");
        return; // Skip all heavy init (audio, phone, Goertzel, etc.)
    }
    
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

    setupAudioPlayer();

    // Initialize Goertzel-based DTMF decoder
    // Goertzel is O(n*k) for 8 DTMF frequencies — the only detector we need
    initGoertzelDecoder(goertzel, goertzelCopier, true);

    // Initialize WiFi with careful error handling
    Logger.println("🔧 Starting WiFi initialization...");


    initWiFi([]() {
        // Enqueue catalog download (non-blocking — tick() drives it)
        Logger.println("🌐 Requesting audio catalog download...");
        if (downloadAudio()) {
            Logger.println("✅ Audio catalog download enqueued");
        } else {
            Logger.println("⚠️ Audio catalog download failed to enqueue - will retry later");
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
            // Handle off-hook event
            Logger.println("📞 Phone picked up (OFF HOOK)");
            // Check if debugaudio command has armed a capture for this off-hook
            if (checkAndExecuteOffHookCapture()) {
                // Capture was triggered, skip normal dial tone for now
                // (capture function will handle audio)
                return;
            }
            Logger.println("⚡ Playing Dial Tone");
            audioPlayer.playAudioKey("dialtone");
        } else {
            // Handle on-hook event
            Logger.println("📞 Phone hung up (ON HOOK)");
            audioPlayer.stop();
            resetDTMFSequence(); // Clear any partial DTMF sequence
        }
    });
    
    Logger.println("✅ Bowie Phone Ready!");
    Logger.println("🔧 Serial Debug Mode ACTIVE - type 'help' for commands");
    
    // Enable task watchdog for the main loop (10 s timeout, panic on expiry).
    // This catches hard hangs — e.g. a decoder stuck in an infinite loop
    // or a bad pointer deref that doesn't trigger a normal panic.
    esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);  // Add current (loopTask) to WDT
    
    // Check if phone is already off hook at boot - play dial tone
    if (Phone.isOffHook())
    {
        Logger.println("📞 Phone is off hook at boot - playing dial tone");
        audioPlayer.playAudioKey("dialtone");
    }
}

void setupAudioPlayer()
{
    audioPlayer.setRegistry(&audioKeyRegistry);

    // Dial tone: 350 Hz + 440 Hz (North American standard)
    audioKeyRegistry.registerGenerator("dialtone",new DualToneGenerator(350.0f, 440.0f, 16000.0f));

    // Ringback: 440 Hz + 480 Hz with cadence
    audioKeyRegistry.registerGenerator("ringback",new DualToneGenerator(440.0f, 480.0f, 16000.0f),
                                                        RINGBACK_TONE_MS,
                                                        RINGBACK_SILENCE_MS);
    audioKeyRegistry.registerGenerator("off_hook",new MultiToneGenerator(
                                                        std::array<float, 4>{
                                                            1400.0f, 
                                                            2060.0f, 
                                                            2450.0f, 
                                                            2600.0f}, 
                                                        16000.0f),
                                                        OFF_HOOK_MS);

    // Register decoders with the audio player
    audioPlayer.addDecoder(mp3_decoder, "audio/mpeg");
    // Register WAV decoder with all common MIME types
    // WAV files can be detected as audio/wav, audio/wave, or audio/vnd.wave
    audioPlayer.addDecoder(wav_decoder, "audio/wav");
    audioPlayer.addDecoder(wav_decoder, "audio/vnd.wave");
    audioPlayer.addDecoder(wav_decoder, "audio/wave");
    // Register M4A decoder: ContainerM4A demuxes the MP4 box structure,
    // then m4a_inner_decoder routes the extracted AAC frames to aac_decoder.
    // checkM4A is inactive by default in MimeDetector — the 3-arg overload activates it.
    m4a_inner_decoder.addDecoder(aac_decoder, "audio/aac");
    m4a_decoder.setDecoder(m4a_inner_decoder);
    audioPlayer.addDecoder(m4a_decoder, "audio/m4a", MimeDetector::checkM4A);
    audioPlayer.begin(kit, source != nullptr);
    Logger.println("✅ Default tone generators registered");
}

void loop()
{
    // === Safe mode: minimal loop (WiFi + OTA + telnet only) ===
    if (tickSafeMode()) {
        esp_task_wdt_reset();  // Feed WDT in safe mode too
        static unsigned long lastCheck = 0;
        unsigned long now = millis();
        if (now - lastCheck >= 100) {
            lastCheck = now;
            handleNetworkLoop();
        }
        return;
    }

    // Feed the task watchdog — if we don't reach here within 10 s, the WDT resets the chip
    esp_task_wdt_reset();
    
    // Process Phone Service
    static unsigned long lastMaintenanceCheck = 0;
    Phone.loop();
    if (Phone.isOffHook())
    {
        // Check for off-hook timeout (play warning tone if inactive too long)
        static bool offHookWarningPlayed = false;
        static unsigned long warningPlayedTime = 0;
        unsigned long now = millis();
        unsigned long lastActivity = max(getLastDigitTime(), audioPlayer.getLastActive());
        // Suppress timeout while sequence audio (ringback/clip/wrong_number) is playing.
        // Dialtone is intentionally excluded — idle dialtone should still age out.
        bool audioInProgress = audioPlayer.isActive() && !audioPlayer.isAudioKeyPlaying("dialtone");
        if (!offHookWarningPlayed && !audioInProgress && lastActivity > 0 && (now - lastActivity) >= OFF_HOOK_TIMEOUT_MS) {
            Logger.println("⚠️ Off-hook timeout - playing warning tone");
            audioPlayer.playAudioKey("off_hook");
            offHookWarningPlayed = true;
            warningPlayedTime = now;
        }
        
        // Reset warning flag when new digit arrives or warning audio finishes
        if (offHookWarningPlayed) {
            if (getLastDigitTime() > warningPlayedTime) {
                offHookWarningPlayed = false;  // New digit → restart timeout
            } else if (!audioPlayer.isActive() && (now - warningPlayedTime) > 2000) {
                offHookWarningPlayed = false;  // Warning finished → allow re-trigger
            }
        }
        
        // Handle audio playback FIRST - highest priority for smooth audio
        if (audioPlayer.isActive())
        {
            audioPlayer.copy();
            bool playingDialtone = audioPlayer.isAudioKeyPlaying("dialtone");
            // Mute Goertzel during non-dialtone playback to suppress
            // false DTMF from ES8388 DAC→ADC internal loopback
            setGoertzelMuted(!playingDialtone);
            if (!playingDialtone) {
                return;
            }
        } else {
            // Mute Goertzel if sequence is locked (waiting for hangup)
            setGoertzelMuted(isSequenceLocked());
        }
        
        // Goertzel runs on separate task - just check for detected keys
        char goertzelKey = getGoertzelKey();
        if (goertzelKey != 0) {
            addDtmfDigit(goertzelKey);
        }
        
        // Process ready sequences (from Goertzel, simulated input, or telnet)
        if (isSequenceReady()) {
            readDTMFSequence(true);
        }

    }

    auto limit = isReadingSequence() ? 10 : 100;
    unsigned long now = millis();
        
    // Rate limit maintenance operations to every 100ms while not reading sequence
    if (now - lastMaintenanceCheck >= limit) {
        lastMaintenanceCheck = now;
        

        // Handle WiFi management (config portal and OTA)
        handleNetworkLoop();
        // Audio maintenance: catalog refresh (if stale) + download queue processing
        audioMaintenanceLoop();
        // Process debug commands from Serial and Telnet
        processDebugInput();
        // Handle Tailscale VPN keepalive/reconnection and remote logging

        tickCrashStabilityCheck();
    }
}