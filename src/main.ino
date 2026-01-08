#include <config.h>
#include "dtmf_decoder.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "driver/gpio.h"
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

// Key pins for AudioKit board (active LOW)
// These may conflict with other functions depending on DIP switch settings
const int KEY_PINS[] = {36, 13, 19, 23, 18, 5};  // KEY1-KEY6
const int NUM_KEYS = 6;

// Firmware update mode key - KEY3 (GPIO 19) is safest as it doesn't conflict with SD card
// Hold this key during boot to enter firmware update mode
const int FIRMWARE_UPDATE_KEY = 19;  // GPIO 19 = KEY3

// Tailscale enable flag - set at boot based on key press
static bool tailscaleEnabled = false;

// Enter USB download/bootloader mode for firmware flashing
// This forces the ESP32 into the ROM bootloader
void enterFirmwareUpdateMode() {
    Logger.println();
    Logger.println("============================================");
    Logger.println("🔧 ENTERING FIRMWARE UPDATE MODE");
    Logger.println("============================================");
    Logger.println("   The device will now restart into bootloader.");
    Logger.println("   You can now upload new firmware.");
    Logger.println();
    Logger.println("   After upload, device will boot normally.");
    Logger.println("============================================");
    Logger.flush();
    delay(500);  // Let serial flush completely
    
    // Disable WiFi to ensure clean state
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    delay(100);
    
    // Use USB_SERIAL_JTAG to request bootloader mode on restart
    // This works on ESP32-S2/S3/C3 but for classic ESP32 we just restart
    // and rely on the upload tool using RTS/DTR to enter bootloader
    Logger.println("   Restarting... Press upload now!");
    Logger.flush();
    delay(200);
    
    esp_restart();
}

// Flag to track if audio board was initialized
static bool audioKitInitialized = false;

// Shut down audio for OTA updates - just stop playback, don't touch kit
void shutdownAudioForOTA() {
    Logger.println("🔇 Shutting down audio for OTA...");
    
    // Stop any playing audio
    stopAudio();
    delay(50);
    
    // Note: We intentionally do NOT call kit.end() here because:
    // 1. It can crash if SPI was already released elsewhere
    // 2. The OTA onStart already calls SD.end() and SPI.end()
    // 3. GPIO pins are reset separately
    
    Logger.println("✅ Audio stopped for OTA");
}

// Scan all keys and return a bitmask of pressed keys (active LOW)
uint8_t scanKeys() {
    uint8_t pressed = 0;
    for (int i = 0; i < NUM_KEYS; i++) {
        if (KEY_PINS[i] >= 0) {
            pinMode(KEY_PINS[i], INPUT_PULLUP);
            if (digitalRead(KEY_PINS[i]) == LOW) {
                pressed |= (1 << i);
            }
        }
    }
    return pressed;
}

// Print which keys are pressed
void printKeyState() {
    Logger.println("🔧 [DEBUG] Scanning keys (active LOW):");
    for (int i = 0; i < NUM_KEYS; i++) {
        if (KEY_PINS[i] >= 0) {
            pinMode(KEY_PINS[i], INPUT_PULLUP);
            int state = digitalRead(KEY_PINS[i]);
            Logger.printf("   KEY%d (GPIO%d): %s\n", i + 1, KEY_PINS[i], state == LOW ? "PRESSED" : "released");
        } else {
            Logger.printf("   KEY%d: not available\n", i + 1);
        }
    }
}

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
                else if (cmd.equalsIgnoreCase("help") || cmd.equals("?")) {
                    Logger.println("🔧 [DEBUG] Serial Debug Commands:");
                    Logger.println("   hook         - Toggle hook state");
                    Logger.println("   level <0-2>  - Set log level (0=quiet, 1=normal, 2=debug)");
                    Logger.println("   <digits>     - Simulate DTMF sequence (e.g., 6969, 888)");
                    Logger.println("   <freq>hz     - FFT test tone (e.g., 697hz)");
                    Logger.println("   state        - Show current state");
                    Logger.println("   dns          - Test DNS resolution");
                    Logger.println("   keys         - Scan hardware keys");
                    Logger.println("   vpn          - Show VPN/Tailscale status");
                    Logger.println("   refresh      - Re-download audio catalog");
                    Logger.println("   update       - Enter firmware update/bootloader mode");
                    Logger.println("   help         - Show this help");
                    Logger.println();
                    Logger.println("   TIP: Hold KEY3 (GPIO19) during boot to enter update mode");
                }
                else if (cmd.equalsIgnoreCase("refresh")) {
                    // Force re-download of audio catalog
                    Logger.println("🔧 [DEBUG] Refreshing audio catalog...");
                    invalidateAudioCache();  // Force fresh download
                    if (downloadAudio()) {
                        Logger.println("✅ Audio catalog refreshed successfully");
                        listAudioKeys();  // Show what was loaded
                    } else {
                        Logger.println("❌ Audio catalog refresh failed");
                    }
                }
                else if (cmd.equalsIgnoreCase("dns")) {
                    // Test DNS resolution
                    Logger.println("🔧 [DEBUG] Testing DNS resolution...");
                    Logger.printf("   WiFi status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
                    Logger.printf("   Local IP: %s\n", WiFi.localIP().toString().c_str());
                    Logger.printf("   DNS1: %s\n", WiFi.dnsIP(0).toString().c_str());
                    Logger.printf("   DNS2: %s\n", WiFi.dnsIP(1).toString().c_str());
                    
                    IPAddress resolved;
                    Logger.print("   Resolving www.googleapis.com... ");
                    if (WiFi.hostByName("www.googleapis.com", resolved)) {
                        Logger.printf("OK -> %s\n", resolved.toString().c_str());
                    } else {
                        Logger.println("FAILED");
                    }
                    Logger.print("   Resolving google.com... ");
                    if (WiFi.hostByName("google.com", resolved)) {
                        Logger.printf("OK -> %s\n", resolved.toString().c_str());
                    } else {
                        Logger.println("FAILED");
                    }
                }
                else if (cmd.equalsIgnoreCase("keys")) {
                    // Scan and print key states
                    printKeyState();
                }
                else if (cmd.equalsIgnoreCase("vpn")) {
                    // Show VPN status and allow toggling
                    Logger.printf("🔧 [DEBUG] Tailscale: enabled=%s, connected=%s\n",
                        tailscaleEnabled ? "YES" : "NO",
                        isTailscaleConnected() ? "YES" : "NO");
                    if (isTailscaleConnected()) {
                        Logger.printf("   Tailscale IP: %s\n", getTailscaleIP() ? getTailscaleIP() : "unknown");
                    }
                }
                else if (cmd.equalsIgnoreCase("state")) {
                    Logger.printf("🔧 [DEBUG] State: Hook=%s, Audio=%s\n", 
                        Phone.isOffHook() ? "OFF_HOOK" : "ON_HOOK",
                        isAudioActive() ? "PLAYING" : "IDLE");
                }
                else if (cmd.equalsIgnoreCase("prepareota") || cmd.equalsIgnoreCase("otaprep")) {
                    Logger.println("🔄 Preparing for OTA update...");
                    shutdownAudioForOTA();
                    delay(100);
                    SD.end();
                    SPI.end();
                    delay(100);
                    
                    // Reset SD card GPIO pins (NOT GPIO 2 - it's used by internal flash!)
                    gpio_reset_pin(GPIO_NUM_13);  // SD_CS
                    gpio_reset_pin(GPIO_NUM_14);  // SD_CLK  
                    gpio_reset_pin(GPIO_NUM_15);  // SD_MOSI
                    // GPIO 2 is shared with internal flash - DO NOT reset it!
                    
                    // Set as inputs with pull-ups
                    gpio_set_direction(GPIO_NUM_13, GPIO_MODE_INPUT);
                    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_INPUT);
                    gpio_set_direction(GPIO_NUM_15, GPIO_MODE_INPUT);
                    gpio_pullup_en(GPIO_NUM_13);
                    gpio_pullup_en(GPIO_NUM_14);
                    gpio_pullup_en(GPIO_NUM_15);
                    
                    delay(200);
                    
                    // Set timeout - reboot if no OTA within 5 minutes
                    setOtaPrepareTimeout();
                    
                    Logger.println("✅ Ready for OTA - will reboot in 5 min if no OTA received");
                }
                else if (cmd.startsWith("pullota ") || cmd.startsWith("otapull ")) {
                    // Pull-based OTA - device fetches firmware from URL (works over VPN!)
                    String url = cmd.substring(cmd.indexOf(' ') + 1);
                    url.trim();
                    if (url.length() > 0) {
                        Logger.printf("📥 Starting pull OTA from: %s\n", url.c_str());
                        if (!performPullOTA(url.c_str())) {
                            Logger.println("❌ Pull OTA failed");
                        }
                        // If successful, device reboots automatically
                    } else {
                        Logger.println("❌ Usage: pullota <firmware_url>");
                        Logger.println("   Example: pullota http://10.253.0.1:8080/firmware.bin");
                    }
                }
                else if (cmd.equalsIgnoreCase("phonehome") || cmd.equalsIgnoreCase("checkin")) {
                    // Manual phone home check-in
                    Logger.println("📞 Manual phone home check-in...");
                    if (phoneHome(nullptr)) {
                        Logger.println("✅ Phone home triggered OTA update");
                    } else {
                        Logger.printf("📞 Phone home status: %s\n", getPhoneHomeStatus());
                    }
                }
                else if (cmd.equalsIgnoreCase("bootloader") || cmd.equalsIgnoreCase("flash") || cmd.equalsIgnoreCase("update")) {
                    enterFirmwareUpdateMode();
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
    
    // Check if Tailscale should be enabled based on boot key press
    tailscaleEnabled = shouldEnableTailscale();
    
    // Reduce AudioTools library logging to Warning level to minimize noise
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
        audioKitInitialized = false;
    }
    else
    {
        Logger.println("✅ AudioKit initialized successfully");
        audioKitInitialized = true;
        
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
        source = new AudioSourceSD(startFilePath, "wav", SD_CS, SPI);
        Logger.println("✅ AudioSourceSD created");
   }

    // Initialize audio file manager
    // Pass sdCardAvailable to indicate whether SD storage is accessible
    initializeAudioFileManager(SD_CS, false, sdCardAvailable);

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
        initAudioPlayerURLMode(kit, multi_decoder);
        Logger.println("✅ Audio player initialized (URL streaming mode)");
    }
#endif

    // Initialize synthesized dial tone generator (350 + 440 Hz)
    initDialToneGenerator(kit);

    // Setup FFT for DTMF detection
    // DTMF tones are 40-100ms, so we need fast time resolution
    // 2048 samples @ 44100Hz = 46ms window with ~21.5 Hz bin width
    // This is sufficient since DTMF frequencies are at least 70Hz apart
    auto tcfg = fft.defaultConfig();
    tcfg.length = 2048;  // 46ms window - good for DTMF timing
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
        
        // PRE-CACHE DNS: Resolve hostnames BEFORE WireGuard starts
        // WireGuard with full tunnel (0.0.0.0/0) may break DNS to public servers
        Logger.println("🌐 Pre-caching DNS resolutions before VPN...");
        preCacheDNS();
        
        // PRIORITY: Initialize Tailscale VPN FIRST (if enabled) to ensure remote access
        // This ensures we can always reach the device via WireGuard for OTA updates
        if (tailscaleEnabled) {
            Logger.println("🔐 WiFi connected - initializing Tailscale VPN...");
            initTailscaleFromConfig();
            Logger.println("✅ Tailscale VPN initialized - device should be reachable");
        } else {
            Logger.println("🌐 Tailscale skipped (not enabled at boot)");
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
    
#ifdef DEBUG
    Logger.println("🔧 Serial Debug Mode ACTIVE - type 'help' for commands");
#endif
    
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

    // Handle Tailscale VPN keepalive/reconnection (only if enabled)
    if (tailscaleEnabled) {
        handleTailscaleLoop();
    }
    
    // Handle phone home periodic check-in (for remote OTA, status, etc.)
    handlePhoneHomeLoop();

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

    // Handle audio playback FIRST - highest priority for smooth audio
    if (isAudioActive())
    {
        // During synthesized dial tone, just output smoothly
        // DTMF detection during dial tone is problematic because the speaker output
        // feeds back into the mic, corrupting the FFT analysis
        if (isSynthDialTonePlaying())
        {
            // Process dial tone output - use StreamCopy for smooth playback
            processDialTone();
            processDialTone();
            processDialTone();
            processDialTone();
            
            // For now, skip DTMF detection during dial tone
            // The mic picks up the dial tone output, making detection unreliable
            // TODO: Add acoustic echo cancellation or use hardware DTMF decoder
            return;
        }
        
        // For non-dial-tone audio, normal processing with DTMF detection
        processAudio();
        processAudio();
        processAudio();
        processAudio();
        
        // Periodically check for DTMF input during other audio playback
        static unsigned long lastDTMFCheck = 0;
        if (Phone.isOffHook() && millis() - lastDTMFCheck > 100)
        {
            lastDTMFCheck = millis();
            copier.copy();
            
            const char* audioPath = readDTMFSequence();
            if (audioPath)
            {
                playAudioPath(audioPath);
            }
        }
        
        // Skip download queue processing during audio playback
        return;
    }

    // Process audio download queue (non-blocking) - only when not playing audio
    static unsigned long lastDownloadCheck = 0;
    if (millis() - lastDownloadCheck > 1000) // Check every second
    {
        processAudioDownloadQueue();
        lastDownloadCheck = millis();
    }

    // Only process DTMF if phone is off hook
    if (!Phone.isOffHook())
    {
        return; // Phone is on hook, nothing to do
    }

    // Process audio input for DTMF detection (only when not playing audio)
    size_t bytesCopied = copier.copy();
    if (bytesCopied > 0) {
        Logger.debugf("🎤 Copied %u bytes to FFT\n", bytesCopied);
    }

    // Check for complete DTMF sequences and play audio if ready
    const char* audioPath = readDTMFSequence();
    if (audioPath)
    {
        playAudioPath(audioPath);
    }
}