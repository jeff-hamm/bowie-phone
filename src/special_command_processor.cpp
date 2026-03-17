#include "special_command_processor.h"
#include "logging.h"
#include "remote_logger.h"
#include "tailscale_manager.h"
#include "audio_file_manager.h"
#include "audio_key_registry.h"
#include "extended_audio_player.h"
#include "wifi_manager.h"
#include "phone_service.h"
#include "sequence_processor.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include <WiFi.h>
#include <EEPROM.h>
#include <Preferences.h>
#include <SD.h>
#include <SD_MMC.h>
#include <SPI.h>
#include "AudioTools/CoreAudio/GoerzelStream.h"
#include "AudioTools/CoreAudio/StreamCopy.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "dtmf_goertzel.h"
#include "dtmf_decoder.h"   // isFFTDebugEnabled / setFFTDebugEnabled stubs
#include "phone.h"
#include <HTTPClient.h>
#include "esp_heap_caps.h"
#include "config.h"

// ============================================================================
// GLOBAL CONFIGURATION
// ============================================================================

// Preferences namespace for ESP32 NVS storage
#define PREFERENCES_NAMESPACE "bowiephone"

// Preferences object for ESP32 NVS storage
static Preferences preferences;

// Forward declaration for audio capture
void performAudioCapture(int durationSec);
void performSDCardDebug();
void performAudioOutputTest();
void performDebugInput(const char* filename);

// ============================================================================
// SYSTEM FUNCTIONS
// ============================================================================

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

// Shut down audio for OTA updates - just stop playback, don't touch kit
void shutdownAudioForOTA() {
    Logger.println("🔇 Shutting down audio for OTA...");
    
    // Stop any playing audio
    getExtendedAudioPlayer().stop();
    delay(50);
    
    // Note: We intentionally do NOT call kit.end() here because:
    // 1. It can crash if SPI was already released elsewhere
    // 2. The OTA onStart already calls SD.end() and SPI.end()
    // 3. GPIO pins are reset separately
    
    Logger.println("✅ Audio stopped for OTA");
}

// Serial debug mode buffer
static char debugInputBuffer[64];
static int debugInputPos = 0;

// Audio capture on next off-hook state
// Can be set at build time with -DCAPTURE_ON_OFFHOOK=<seconds>
#ifdef CAPTURE_ON_OFFHOOK
static bool captureOnNextOffHook = true;
  #if CAPTURE_ON_OFFHOOK == 1
    // Flag defined without value, use default 20 seconds
    static int captureRequestedDuration = 20;
  #else
    // Use specified value from build flag
    static int captureRequestedDuration = CAPTURE_ON_OFFHOOK;
  #endif
#else
static bool captureOnNextOffHook = false;
static int captureRequestedDuration = 0;
#endif

// NVS keys for audio capture persistence
#define NVS_KEY_CAPTURE_ARMED "cap_armed"
#define NVS_KEY_CAPTURE_DURATION "cap_dur"

// Forward declaration
void processDebugCommand(const String& cmd);

// Save audio capture state to NVS
void saveAudioCaptureState() {
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Logger.println("⚠️  Failed to save audio capture state to NVS");
        return;
    }
    preferences.putBool(NVS_KEY_CAPTURE_ARMED, captureOnNextOffHook);
    preferences.putInt(NVS_KEY_CAPTURE_DURATION, captureRequestedDuration);
    preferences.end();
}

// Load audio capture state from NVS
void loadAudioCaptureState() {
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
        return;
    }
    
    bool hasNVSState = preferences.isKey(NVS_KEY_CAPTURE_ARMED);
    
    if (hasNVSState) {
        // Load from NVS (user has manually configured this before)
        captureOnNextOffHook = preferences.getBool(NVS_KEY_CAPTURE_ARMED, false);
        captureRequestedDuration = preferences.getInt(NVS_KEY_CAPTURE_DURATION, 20);
    }
    // else: keep static variable values (either defaults or build-flag initialization)
    
    preferences.end();
    
    if (captureOnNextOffHook) {
        if (hasNVSState) {
            Logger.printf("📋 Restored audio capture from NVS: %d seconds on next off-hook\n", captureRequestedDuration);
        } else {
#ifdef CAPTURE_ON_OFFHOOK
            Logger.printf("📋 Audio capture armed from build flag: %d seconds on next off-hook\n", captureRequestedDuration);
            // Save build flag defaults to NVS for persistence
            saveAudioCaptureState();
#endif
        }
    }
}

// Clear audio capture state from NVS
void clearAudioCaptureState() {
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        return;
    }
    preferences.remove(NVS_KEY_CAPTURE_ARMED);
    preferences.remove(NVS_KEY_CAPTURE_DURATION);
    preferences.end();
}

// Process input from a stream for debug commands (Serial or Telnet)
// Buffers characters until newline, then processes the command
void processDebugInput(Stream& input) {
    while (input.available()) {
        char c = input.read();
        
        // Handle newline/carriage return - process command
        if (c == '\n' || c == '\r') {
            if (debugInputPos > 0) {
                debugInputBuffer[debugInputPos] = '\0';
                String cmd = String(debugInputBuffer);
                cmd.trim();
                processDebugCommand(cmd);
                debugInputPos = 0;
            }
        }
        else if (debugInputPos < (int)sizeof(debugInputBuffer) - 1) {
            debugInputBuffer[debugInputPos++] = c;
        }
    }
}

// Initialize audio capture state (call during setup)
void initAudioCaptureState() {
    loadAudioCaptureState();
}

// Check if we need to trigger audio capture on this off-hook event
// Call this from the off-hook handler in main.ino
// Returns true if capture was triggered (so caller can skip normal behavior)
bool checkAndExecuteOffHookCapture() {
    if (captureOnNextOffHook) {
        Logger.printf("🎙️ Auto-triggering audio capture (%d seconds)...\n", captureRequestedDuration);
        performAudioCapture(captureRequestedDuration);
        
        // Clear the flag - one-time capture only
        captureOnNextOffHook = false;
        captureRequestedDuration = 0;
        clearAudioCaptureState();
        return true;
    }
    return false;
}

// Process a debug command string (from Serial or Telnet)
// Serial-only commands that require interactive input/output
// For phone-accessible commands, use special commands (*#xx#)
void processDebugCommand(const String& cmd) {
    if (cmd.equalsIgnoreCase("hook")) {
        bool newState = !Phone.isOffHook();
        Phone.setOffHook(newState);
        Logger.printf("🔧 [DEBUG] Hook toggled to: %s\n", newState ? "OFF HOOK" : "ON HOOK");
    }
    else if (cmd.equalsIgnoreCase("hook auto")) {
        Phone.resetDebugOverride();
        Logger.println("🔧 [DEBUG] Hook detection reset to automatic");
    }
    else if (cmd.equalsIgnoreCase("cpuload") || cmd.equalsIgnoreCase("perftest")
          || cmd.equalsIgnoreCase("cpuload-goertzel") || cmd.equalsIgnoreCase("perftest-goertzel")) {
        // CPU load test - Goertzel-based DTMF detection
        performGoertzelCPULoadTest();
    }
    else if (cmd.equalsIgnoreCase("help") || cmd.equals("?")) {

        Logger.println("🔧 [DEBUG] Serial/Telnet Commands:");
        Logger.println("   hook          - Toggle hook state");
        Logger.println("   hook auto     - Reset to automatic hook detection");
        Logger.println("   cpuload       - Test CPU load (Goertzel DTMF + audio)");
        Logger.println("   level <0-2>   - Set log level (0=quiet, 1=normal, 2=debug)");
        Logger.println("   state         - Show current state");
        Logger.println("   debugaudio [s] - Arm audio capture on next off-hook (1-60s, default 20)");
        Logger.println("   debuginput [f] - Full E2E test: hook→dialtone→Goertzel→sequence→timeout→reset");
        Logger.println("   audiotest      - Test audio output (verify I2S data flow)");
        Logger.println("   sddebug       - Test SD card initialization methods");
        Logger.println("   scan          - Scan for WiFi networks");
        Logger.println("   dns           - Test DNS resolution");
        Logger.println("   tailscale     - Toggle Tailscale VPN on/off");
        Logger.println("   pullota <url> - Pull firmware from URL");
        Logger.println("   update        - Enter firmware bootloader mode");
        Logger.println("   <digits>      - Simulate DTMF sequence");
        Logger.println();
        Logger.println("📱 Phone Commands (dial these):");
        Logger.println("   *123#  - System Status");
        Logger.println("   *789#  - Reboot Device");
        Logger.println("   *#06#  - Device Info");
        Logger.println("   clear-cache  - Clear Cache & Reboot");
        Logger.println("   *#07#  - Refresh Audio");
        Logger.println("   *#08#  - Prepare for OTA");
        Logger.println("   *#09#  - Phone Home Check-in");
        Logger.println("   *#88#  - Tailscale Status");
        Logger.println("   *#00#  - List All Commands");
    }
    else if (cmd.equalsIgnoreCase("scan") || cmd.equalsIgnoreCase("wifiscan")) {
        Logger.println("🔧 [DEBUG] Scanning for WiFi networks...");
        Logger.printf("   Current WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        if (WiFi.status() == WL_CONNECTED) {
            Logger.printf("   Connected to: %s\n", WiFi.SSID().c_str());
            Logger.printf("   Signal: %d dBm\n", WiFi.RSSI());
        }
        Logger.println();
        
        int n = WiFi.scanNetworks();
        Logger.printf("   Found %d networks:\n", n);
        Logger.println();
        
        for (int i = 0; i < n; i++) {
            Logger.printf("   %2d: %-32s | Ch:%2d | %4d dBm | %s\n",
                i + 1,
                WiFi.SSID(i).c_str(),
                WiFi.channel(i),
                WiFi.RSSI(i),
                WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open" : "Secure");
        }
        Logger.println();
        WiFi.scanDelete();
    }
    else if (cmd.equalsIgnoreCase("dns")) {
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
    }
    else if (cmd.equalsIgnoreCase("state")) {
        Logger.printf("🔧 [DEBUG] State: Hook=%s, Audio=%s\n", 
            Phone.isOffHook() ? "OFF_HOOK" : "ON_HOOK",
            getExtendedAudioPlayer().isActive() ? "PLAYING" : "IDLE");
        Logger.printf("   WiFi: %s, IP: %s\n",
            WiFi.isConnected() ? "Connected" : "Disconnected",
            WiFi.localIP().toString().c_str());
        if (isTailscaleConnected()) {
            Logger.printf("   VPN: %s\n", getTailscaleIP());
        }
        Logger.printf("   Tailscale: %s (saved state)\n", isTailscaleEnabled() ? "enabled" : "disabled");
    }
    else if (cmd.equalsIgnoreCase("tailscale") || cmd.equalsIgnoreCase("vpn")) {
        // Toggle Tailscale enabled state
        bool newState = toggleTailscaleEnabled();
        Logger.printf("🔐 Tailscale toggled to: %s\n", newState ? "ENABLED" : "DISABLED");
        Logger.println("   Reboot required for change to take effect");
    }
    else if (cmd.equalsIgnoreCase("fft") || cmd.equalsIgnoreCase("fftdebug")) {
        // Toggle FFT debug output
        bool newState = !isFFTDebugEnabled();
        setFFTDebugEnabled(newState);
        Logger.printf("🎵 FFT debug output: %s\n", newState ? "ENABLED" : "DISABLED");
    }
    else if (cmd.startsWith("debugaudio") || cmd.startsWith("debugAudio") || cmd.equalsIgnoreCase("audiodebug")) {
        // Parse optional duration: "debugaudio 30" = arm 30 second capture on next off-hook
        int durationSec = 20; // default
        int spaceIdx = cmd.indexOf(' ');
        if (spaceIdx > 0) {
            int val = cmd.substring(spaceIdx + 1).toInt();
            if (val >= 1 && val <= 60) {
                durationSec = val;
            } else {
                Logger.println("⚠️  Invalid duration (1-60 seconds), using default 20s");
            }
        }
        
        captureOnNextOffHook = true;
        captureRequestedDuration = durationSec;
        saveAudioCaptureState();
        Logger.printf("✅ Audio capture armed: will capture %d seconds on next off-hook\n", durationSec);
        Logger.println("   Pick up the phone to trigger capture");
        Logger.println("   (Saved to NVS - survives reboot)");
    }
    else if (cmd.equalsIgnoreCase("audiotest") || cmd.equalsIgnoreCase("atest")) {
        performAudioOutputTest();
    }
    else if (cmd.startsWith("debuginput") || cmd.startsWith("replayaudio")) {
        String arg = cmd.substring(cmd.indexOf(' ') + 1);
        arg.trim();
        if (arg.length() == 0 || arg == cmd) {
            performDebugInput("/debug_audio.raw");
        } else {
            performDebugInput(arg.c_str());
        }
    }
    else if (cmd.equalsIgnoreCase("sddebug") || cmd.equalsIgnoreCase("sdtest")) {
        performSDCardDebug();
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
        } else {
            Logger.println("❌ Usage: pullota <firmware_url>");
        }
    }
    else if (cmd.equalsIgnoreCase("bootloader") || cmd.equalsIgnoreCase("flash") || cmd.equalsIgnoreCase("update")) {
        enterFirmwareUpdateMode();
    }
    else if (cmd.startsWith("level ")) {
        int level = cmd.substring(6).toInt();
        if (level >= 0 && level <= 2) {
            Logger.setLogLevel((LogLevel)level);
            Logger.printf("🔧 [DEBUG] Log level set to: %d\n", level);
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
            for (size_t i = 0; i < cmd.length(); i++) {
                addDtmfDigit(cmd.charAt(i));
            }
        } else if (cmd.length() > 0) {
            Logger.printf("🔧 [DEBUG] Unknown command: %s (type 'help' for list)\n", cmd.c_str());
        }
    }
}

// ============================================================================
// SPECIAL COMMANDS - CONFIGURABLE STORAGE
// ============================================================================

// EEPROM storage configuration
#define EEPROM_SIZE 1024
#define EEPROM_MAGIC 0xB0E1  // Magic number to verify EEPROM data
#define EEPROM_VERSION 1

// EEPROM data structure for persistent storage
struct EEPROMCommandData {
    char sequence[16];      // DTMF sequence
    char description[32];   // Command description
    bool isActive;         // Whether command is active
};

struct EEPROMHeader {
    uint16_t magic;        // Magic number for validation
    uint8_t version;       // Data format version
    uint8_t commandCount;  // Number of stored commands
    uint32_t checksum;     // Data integrity checksum
};

// Runtime storage for special commands
static SpecialCommand specialCommands[MAX_SPECIAL_COMMANDS];
static int specialCommandCount = 0;

// Default special commands (can be overridden via build flags or ESPHome)
// Organized by function:
//   *xxx# = System operations
//   *#xx# = Status/info commands  
//   #xxx* = Reserved for custom
static const SpecialCommand DEFAULT_SPECIAL_COMMANDS[] = {
    // ========== SYSTEM OPERATIONS (*xxx#) ==========
#ifdef CUSTOM_COMMAND_1_SEQ
    {CUSTOM_COMMAND_1_SEQ, CUSTOM_COMMAND_1_DESC},
#else
    {"*123#", "System Status"},
#endif
    {"*789#", "Reboot Device"},
    {"*000#", "Factory Reset"},
    
    // ========== STATUS & INFO (*#xx#) ==========
    {"*#00#", "List Commands"},
    {"clear-cache", "Clear Cache & Reboot"},
    {"*#06#", "Device Info"},
    {"*#07#", "Refresh Audio"},
    {"*#08#", "Prepare for OTA"},
    {"*#09#", "Phone Home Check-in"},
    {"*#88#", "Tailscale Status"},
    
    // ========== EEPROM MANAGEMENT ==========
    {"*#01#", "Save to EEPROM"},  
    {"*#02#", "Load from EEPROM"},
    {"*#99#", "Erase EEPROM"},
};

// ============================================================================
// INITIALIZATION AND MANAGEMENT
// ============================================================================

void initializeSpecialCommands()
{
    Logger.printf("🔧 Initializing special commands system...\n");
    
    // Initialize audio capture state from NVS
    initAudioCaptureState();
    
    // Try to load from EEPROM first
    if (loadSpecialCommandsFromEEPROM())
    {
        Logger.printf("📥 Using commands from EEPROM storage\n");
        return;
    }
    
    // If no EEPROM data or loading failed, initialize with defaults
    Logger.printf("🔄 Initializing with default commands\n");
    clearSpecialCommands();

    int defaultCount = sizeof(DEFAULT_SPECIAL_COMMANDS) / sizeof(DEFAULT_SPECIAL_COMMANDS[0]);
    for (int i = 0; i < defaultCount && i < MAX_SPECIAL_COMMANDS; i++)
    {
        specialCommands[i] = DEFAULT_SPECIAL_COMMANDS[i];

        // Assign function pointers based on sequence
        assignDefaultHandler(i, DEFAULT_SPECIAL_COMMANDS[i].sequence);

        specialCommandCount++;
    }

    Logger.printf("✅ Initialized %d default special commands\n", specialCommandCount);
}

bool addSpecialCommand(const char *sequence, const char *description, void (*handler)(void))
{
    if (specialCommandCount >= MAX_SPECIAL_COMMANDS)
    {
        Logger.printf("Error: Special command table is full\n");
        return false;
    }

    // Create persistent copies of strings
    char* seqCopy = (char*)malloc(strlen(sequence) + 1);
    char* descCopy = (char*)malloc(strlen(description) + 1);
    
    if (!seqCopy || !descCopy)
    {
        Logger.printf("❌ Memory allocation failed for command\n");
        if (seqCopy) free(seqCopy);
        if (descCopy) free(descCopy);
        return false;
    }
    
    strcpy(seqCopy, sequence);
    strcpy(descCopy, description);

    specialCommands[specialCommandCount].sequence = seqCopy;
    specialCommands[specialCommandCount].description = descCopy;
    specialCommands[specialCommandCount].handler = handler;
    specialCommandCount++;

    Logger.printf("✅ Added special command: %s - %s\n", sequence, description);
    
    // Automatically save to EEPROM
    saveSpecialCommandsToEEPROM();
    
    return true;
}

int getSpecialCommandCount()
{
    return specialCommandCount;
}

void clearSpecialCommands()
{
    // Free dynamically allocated strings
    for (int i = 0; i < specialCommandCount; i++)
    {
        if (specialCommands[i].sequence)
        {
            free((void*)specialCommands[i].sequence);
        }
        if (specialCommands[i].description)
        {
            free((void*)specialCommands[i].description);
        }
    }
    
    specialCommandCount = 0;
    memset(specialCommands, 0, sizeof(specialCommands));
}

// ============================================================================
// EEPROM PERSISTENCE FUNCTIONS
// ============================================================================

uint32_t calculateChecksum(const EEPROMCommandData* data, int count)
{
    uint32_t checksum = 0;
    const uint8_t* bytes = (const uint8_t*)data;
    
    for (int i = 0; i < count * sizeof(EEPROMCommandData); i++)
    {
        checksum += bytes[i];
        checksum = (checksum << 1) | (checksum >> 31); // Rotate left
    }
    
    return checksum;
}

void saveSpecialCommandsToEEPROM()
{
    Logger.printf("💾 Saving special commands to EEPROM...\n");
    
    // Use ESP32 Preferences (NVS) for reliable storage
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Logger.printf("❌ Failed to initialize preferences\n");
        return;
    }
    
    // Prepare data for storage
    EEPROMCommandData eepromData[MAX_SPECIAL_COMMANDS];
    EEPROMHeader header;
    
    header.magic = EEPROM_MAGIC;
    header.version = EEPROM_VERSION;
    header.commandCount = specialCommandCount;
    
    // Copy commands to EEPROM format (only sequence and description, not function pointers)
    int validCommands = 0;
    for (int i = 0; i < specialCommandCount; i++)
    {
        if (specialCommands[i].sequence != nullptr && 
            strlen(specialCommands[i].sequence) < sizeof(eepromData[i].sequence))
        {
            strncpy(eepromData[validCommands].sequence, specialCommands[i].sequence, 
                   sizeof(eepromData[i].sequence) - 1);
            eepromData[validCommands].sequence[sizeof(eepromData[i].sequence) - 1] = '\0';
            
            strncpy(eepromData[validCommands].description, 
                   specialCommands[i].description ? specialCommands[i].description : "Custom Command", 
                   sizeof(eepromData[i].description) - 1);
            eepromData[validCommands].description[sizeof(eepromData[i].description) - 1] = '\0';
            
            eepromData[validCommands].isActive = true;
            validCommands++;
        }
    }
    
    header.commandCount = validCommands;
    header.checksum = calculateChecksum(eepromData, validCommands);
    
    // Save header and data
    preferences.putBytes("header", &header, sizeof(header));
    preferences.putBytes("commands", eepromData, validCommands * sizeof(EEPROMCommandData));
    
    preferences.end();
    
    Logger.printf("✅ Saved %d commands to EEPROM\n", validCommands);
}

bool loadSpecialCommandsFromEEPROM()
{
    Logger.printf("📖 Loading special commands from EEPROM...\n");
    
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) // Read-only mode
    {
        Logger.printf("❌ Failed to initialize preferences for reading\n");
        return false;
    }
    
    // Load header
    EEPROMHeader header;
    size_t headerSize = preferences.getBytes("header", &header, sizeof(header));
    
    if (headerSize != sizeof(header))
    {
        Logger.printf("📄 No valid EEPROM data found, using defaults\n");
        preferences.end();
        return false;
    }
    
    // Validate header
    if (header.magic != EEPROM_MAGIC)
    {
        Logger.printf("❌ Invalid EEPROM magic number: 0x%04X (expected 0x%04X)\n", 
                     header.magic, EEPROM_MAGIC);
        preferences.end();
        return false;
    }
    
    if (header.version != EEPROM_VERSION)
    {
        Logger.printf("⚠️  EEPROM version mismatch: %d (expected %d)\n", 
                     header.version, EEPROM_VERSION);
        preferences.end();
        return false;
    }
    
    if (header.commandCount > MAX_SPECIAL_COMMANDS)
    {
        Logger.printf("❌ Too many commands in EEPROM: %d (max %d)\n", 
                     header.commandCount, MAX_SPECIAL_COMMANDS);
        preferences.end();
        return false;
    }
    
    // Load command data
    EEPROMCommandData eepromData[MAX_SPECIAL_COMMANDS];
    size_t dataSize = preferences.getBytes("commands", eepromData, 
                                          header.commandCount * sizeof(EEPROMCommandData));
    
    preferences.end();
    
    if (dataSize != header.commandCount * sizeof(EEPROMCommandData))
    {
        Logger.printf("❌ EEPROM data size mismatch\n");
        return false;
    }
    
    // Verify checksum
    uint32_t calculatedChecksum = calculateChecksum(eepromData, header.commandCount);
    if (calculatedChecksum != header.checksum)
    {
        Logger.printf("❌ EEPROM checksum mismatch: 0x%08X vs 0x%08X\n", 
                     calculatedChecksum, header.checksum);
        return false;
    }
    
    // Clear current commands and load from EEPROM
    clearSpecialCommands();
    
    for (int i = 0; i < header.commandCount; i++)
    {
        if (eepromData[i].isActive)
        {
            // Allocate memory for strings (they need to persist)
            char* seqCopy = (char*)malloc(strlen(eepromData[i].sequence) + 1);
            char* descCopy = (char*)malloc(strlen(eepromData[i].description) + 1);
            
            if (seqCopy && descCopy)
            {
                strcpy(seqCopy, eepromData[i].sequence);
                strcpy(descCopy, eepromData[i].description);
                
                specialCommands[specialCommandCount].sequence = seqCopy;
                specialCommands[specialCommandCount].description = descCopy;
                
                // Assign function pointer based on sequence (for known commands)
                assignDefaultHandler(specialCommandCount, seqCopy);
                
                specialCommandCount++;
                
                Logger.printf("📥 Loaded command: %s - %s\n", seqCopy, descCopy);
            }
            else
            {
                Logger.printf("❌ Memory allocation failed for command %d\n", i);
                if (seqCopy) free(seqCopy);
                if (descCopy) free(descCopy);
            }
        }
    }
    
    Logger.printf("✅ Loaded %d commands from EEPROM\n", specialCommandCount);
    return true;
}

void assignDefaultHandler(int index, const char* sequence)
{
    // Assign function pointers for known default commands
    // System operations
    if (strcmp(sequence, "*123#") == 0)
        specialCommands[index].handler = executeSystemStatus;
    else if (strcmp(sequence, "*789#") == 0)
        specialCommands[index].handler = executeReboot;
    else if (strcmp(sequence, "*000#") == 0)
        specialCommands[index].handler = executeFactoryReset;
    // Status & info commands
    else if (strcmp(sequence, "*#00#") == 0)
        specialCommands[index].handler = executeListCommands;
    else if (strcmp(sequence, "clear-cache") == 0)
        specialCommands[index].handler = executeClearCacheAndReboot;
    else if (strcmp(sequence, "*#06#") == 0)
        specialCommands[index].handler = executeDeviceInfo;
    else if (strcmp(sequence, "*#07#") == 0)
        specialCommands[index].handler = executeRefreshAudio;
    else if (strcmp(sequence, "*#08#") == 0)
        specialCommands[index].handler = executePrepareOTA;
    else if (strcmp(sequence, "*#09#") == 0)
        specialCommands[index].handler = executePhoneHome;
    else if (strcmp(sequence, "*#88#") == 0)
        specialCommands[index].handler = executeTailscaleStatus;
    // EEPROM management commands
    else if (strcmp(sequence, "*#01#") == 0)
        specialCommands[index].handler = executeSaveEEPROM;
    else if (strcmp(sequence, "*#02#") == 0)
        specialCommands[index].handler = executeLoadEEPROM;
    else if (strcmp(sequence, "*#99#") == 0)
        specialCommands[index].handler = executeEraseEEPROM;
    else
        specialCommands[index].handler = nullptr; // Custom command, no default handler
}

void eraseSpecialCommandsFromEEPROM()
{
    Logger.printf("🗑️  Erasing special commands from EEPROM...\n");
    
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Logger.printf("❌ Failed to initialize preferences for clearing\n");
        return;
    }
    
    preferences.clear();
    preferences.end();
    
    Logger.printf("✅ EEPROM data cleared\n");
}

bool isSpecialCommand(const char *sequence)
{
    for (int i = 0; i < specialCommandCount; i++)
    {
        if (strcmp(sequence, specialCommands[i].sequence) == 0)
        {
            return true;
        }
    }
    return false;
}

void processSpecialCommand(const char *sequence)
{
    Logger.printf("⚙️  SPECIAL COMMAND DETECTED: %s\n", sequence);

    // Find and execute the command
    for (int i = 0; i < specialCommandCount; i++)
    {
        if (strcmp(sequence, specialCommands[i].sequence) == 0)
        {
            Logger.printf("🔧 Command: %s\n", specialCommands[i].description);

            // Execute command via function pointer if available
            if (specialCommands[i].handler != nullptr)
            {
                specialCommands[i].handler();
            }
            else
            {
                Logger.printf("⚠️  No handler assigned for command: %s\n", sequence);
            }

            return;
        }
    }

    Logger.printf("❌ Command not found: %s\n", sequence);
}

// ============================================================================
// DEFAULT COMMAND IMPLEMENTATIONS
// ============================================================================

void executeSystemStatus()
{
    Logger.printf("📊 System Status:\n");
    Logger.printf("   WiFi: %s\n", WiFi.isConnected() ? "Connected" : "Disconnected");
    Logger.printf("   IP: %s\n", WiFi.localIP().toString().c_str());
    Logger.printf("   Free Heap: %d bytes\n", ESP.getFreeHeap());
    Logger.printf("   Uptime: %lu seconds\n", millis() / 1000);
    Logger.printf("   Audio: %s\n", getExtendedAudioPlayer().isActive() ? "Playing" : "Idle");
    if (isTailscaleConnected()) {
        Logger.printf("   VPN: Connected (%s)\n", getTailscaleIP());
    }
}

void executeReboot()
{
    Logger.printf("🔄 Rebooting device in 2 seconds...\n");
    getExtendedAudioPlayer().stop();  // Stop any playing audio
    delay(2000);
    ESP.restart();
}

void executeFactoryReset()
{
    Logger.printf("⚠️  FACTORY RESET initiated!\n");
    Logger.printf("🗑️  Clearing all settings...\n");
    eraseSpecialCommandsFromEEPROM();
    Logger.printf("🔄 Restarting...\n");
    delay(2000);
    ESP.restart();
}

void executeDeviceInfo()
{
    Logger.printf("📱 Device Information:\n");
    Logger.printf("   MAC: %s\n", WiFi.macAddress().c_str());
    Logger.printf("   Chip Model: %s\n", ESP.getChipModel());
    Logger.printf("   Chip Revision: %d\n", ESP.getChipRevision());
    Logger.printf("   Flash Size: %d KB\n", ESP.getFlashChipSize() / 1024);
    Logger.printf("   Free Heap: %d bytes\n", ESP.getFreeHeap());
}

void executeClearCacheAndReboot()
{
    Logger.printf("🗑️  Clearing audio cache and rebooting...\n");
    invalidateAudioCache();
    Logger.printf("✅ Cache cleared. Rebooting in 2 seconds...\n");
    Logger.flush();
    getExtendedAudioPlayer().stop();
    delay(2000);
    ESP.restart();
}

void executeRefreshAudio()
{
    Logger.printf("🔄 Refreshing audio catalog...\n");
    invalidateAudioCache();  // Force fresh download
    if (downloadAudio()) {
        Logger.printf("✅ Audio catalog refreshed successfully\n");
        getAudioKeyRegistry().listKeys();  // Show what was loaded
    } else {
        Logger.printf("❌ Audio catalog refresh failed\n");
    }
}

void executePrepareOTA()
{
    Logger.printf("🔄 Preparing for OTA update...\n");
    
    // Stop any playing audio
    getExtendedAudioPlayer().stop();
    delay(100);
    
    // Release SD card
    SD.end();
    delay(100);
    
    // Set timeout - reboot if no OTA within 5 minutes
    setOtaPrepareTimeout();
    
    Logger.printf("✅ Ready for OTA - will reboot in 5 min if no OTA received\n");
    Logger.printf("   Use 'pullota <url>' via serial/telnet to start update\n");
}

void executePhoneHome()
{
    Logger.printf("📞 Manual phone home check-in...\n");
    if (phoneHome(nullptr)) {
        Logger.printf("✅ Phone home triggered OTA update\n");
    } else {
        Logger.printf("📞 Phone home status: %s\n", getPhoneHomeStatus());
    }
}

void executeListCommands()
{
    Logger.printf("📋 Special Commands List:\n");
    Logger.printf("   Total commands: %d / %d\n", specialCommandCount, MAX_SPECIAL_COMMANDS);
    
    for (int i = 0; i < specialCommandCount; i++)
    {
        Logger.printf("   %d: %s - %s %s\n", 
                     i + 1, 
                     specialCommands[i].sequence,
                     specialCommands[i].description,
                     specialCommands[i].handler ? "(active)" : "(custom)");
    }
    
    if (specialCommandCount == 0)
    {
        Logger.printf("   No commands configured\n");
    }
}

void executeSaveEEPROM()
{
    Logger.printf("💾 Manual EEPROM Save Command\n");
    saveSpecialCommandsToEEPROM();
}

void executeLoadEEPROM()
{
    Logger.printf("📥 Manual EEPROM Load Command\n");
    
    if (loadSpecialCommandsFromEEPROM())
    {
        Logger.printf("✅ Commands reloaded from EEPROM\n");
    }
    else
    {
        Logger.printf("❌ Failed to load from EEPROM, keeping current commands\n");
    }
}

void executeEraseEEPROM()
{
    Logger.printf("🗑️  Manual EEPROM Erase Command\n");
    eraseSpecialCommandsFromEEPROM();
    
    // Reload defaults after erasing
    Logger.printf("🔄 Reinitializing with defaults...\n");
    initializeSpecialCommands();
}

void executeTailscaleStatus()
{
    Logger.printf("🔐 Tailscale/WireGuard Status:\n");
    Logger.printf("   Status: %s\n", getTailscaleStatus());
    
    if (isTailscaleConnected()) {
        Logger.printf("   Tailnet IP: %s\n", getTailscaleIP());
        Logger.printf("   Connection: Active\n");
    } else {
        Logger.printf("   Connection: Inactive\n");
        Logger.printf("   Configure via WIREGUARD_* build flags\n");
    }
}

// ============================================================================
// CPU PERFORMANCE TESTING FUNCTIONS
// ============================================================================

// performFFTCPULoadTest() removed — FFT pipeline no longer exists.
// Use "cpuload" command which now runs the Goertzel test below.

void performGoertzelCPULoadTest()
{
    // CPU load test - Goertzel-based DTMF detection during audio playback
    // Uses FreeRTOS task on core 0 for Goertzel (audio on core 1)
    extern GoertzelStream goertzel;
    extern StreamCopy goertzelCopier;
    extern AudioBoardStream kit;
    
    Logger.println("🔬 CPU Load Test: Goertzel Task + Audio");
    Logger.println("============================================");
    
    // Reset Goertzel state to clear any stale detections
    resetGoertzelState();
    
    Logger.println("Starting Goertzel task on core 0...");
    startGoertzelTask(goertzelCopier);
    delay(50);  // Let task start
    
    Logger.println("Starting dial tone playback...");
    getExtendedAudioPlayer().playAudioKey("dialtone");
    delay(100);  // Let audio start
    
    if (!getExtendedAudioPlayer().isActive()) {
        Logger.println("❌ Failed to start audio - test aborted");
        stopGoertzelTask();
        return;
    }
    
    const int TEST_DURATION_MS = 5000;
    const int SAMPLE_INTERVAL_MS = 100;
    
    unsigned long testStart = millis();
    unsigned long loopCount = 0;
    unsigned long dtmfDetectCount = 0;
    unsigned long maxLoopTime = 0;
    unsigned long minLoopTime = ULONG_MAX;
    unsigned long totalLoopTime = 0;
    unsigned long audioUnderrunCount = 0;
    unsigned long lastSample = millis();
    
    // Audio throughput tracking
    size_t totalBytesWritten = 0;
    
    Logger.printf("Running for %d seconds (Goertzel on core 0)...\\n", TEST_DURATION_MS / 1000);
    
    while (millis() - testStart < TEST_DURATION_MS) {
        unsigned long loopStart = micros();
        
        // Process audio output ONLY (Goertzel runs in separate task)
        if (getExtendedAudioPlayer().isActive()) {
            getExtendedAudioPlayer().copy();
            totalBytesWritten += getExtendedAudioPlayer().getLastCopyBytes();
        } else {
            audioUnderrunCount++;
            // Restart dial tone if it stopped unexpectedly
            getExtendedAudioPlayer().playAudioKey("dialtone");
        }
        
        // Check for detected keys (Goertzel task handles the detection)
        char key = getGoertzelKey();
        if (key != 0) {
            dtmfDetectCount++;
        }
        
        unsigned long loopTime = micros() - loopStart;
        totalLoopTime += loopTime;
        loopCount++;
        
        if (loopTime > maxLoopTime) maxLoopTime = loopTime;
        if (loopTime < minLoopTime) minLoopTime = loopTime;
        
        // Progress dots
        if (millis() - lastSample >= SAMPLE_INTERVAL_MS * 10) {
            Logger.print(".");
            lastSample = millis();
        }
        
        yield();
    }
    
    // Stop Goertzel task and audio
    stopGoertzelTask();
    getExtendedAudioPlayer().stop();
    
    // Calculate statistics
    unsigned long avgLoopTime = loopCount > 0 ? totalLoopTime / loopCount : 0;
    float loopsPerSecond = loopCount * 1000.0f / TEST_DURATION_MS;
    float expectedGoertzelRate = 44100.0f / 512.0f;  // Based on block_size=512
    float expectedBytesPerSec = (float)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
    float expectedBytes = expectedBytesPerSec * TEST_DURATION_MS / 1000.0f;
    
    Logger.println();
    Logger.println("============================================");
    Logger.println("📊 Results:");
    Logger.printf("   Test duration: %lu ms\n", TEST_DURATION_MS);
    Logger.printf("   Total loops: %lu (%.1f/sec)\n", loopCount, loopsPerSecond);
    Logger.printf("   Goertzel: running on core 0 (separate task)\n");
    Logger.printf("   Expected Goertzel rate: %.1f blocks/sec\n", expectedGoertzelRate);
    Logger.printf("   DTMF keys detected: %lu (should be 0 for dial tone)\n", dtmfDetectCount);
    Logger.println();
    Logger.println("⏱️ Main Loop Timing (microseconds):");
    Logger.printf("   Min: %lu µs\n", minLoopTime);
    Logger.printf("   Max: %lu µs\n", maxLoopTime);
    Logger.printf("   Avg: %lu µs\n", avgLoopTime);
    Logger.println();
    Logger.println("🔊 Audio Throughput:");
    Logger.printf("   Bytes written: %u (%.1f KB/s)\n", 
                  (unsigned)totalBytesWritten,
                  totalBytesWritten * 1000.0f / TEST_DURATION_MS / 1024.0f);
    Logger.printf("   Expected:      %.0f (%.1f KB/s @ %dHz/%dch/%dbit)\n",
                  expectedBytes, expectedBytesPerSec / 1024.0f,
                  AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS_PER_SAMPLE);
    if (totalBytesWritten > 0) {
        Logger.printf("   Throughput:    %.1f%%\n", totalBytesWritten * 100.0f / expectedBytes);
    }
    Logger.println();
    Logger.printf("⚠️ Audio restarts (underruns): %lu\n", audioUnderrunCount);
    Logger.println();
    
    // Assessment
    Logger.println("📋 Assessment:");
    if (maxLoopTime > 50000) {
        Logger.println("   ❌ FAIL: Max loop time > 50ms - will cause audio glitches");
    } else if (maxLoopTime > 23000) {  // Audio buffer is ~23ms
        Logger.println("   ⚠️ WARN: Max loop time > 23ms - audio may stutter");
    } else {
        Logger.println("   ✅ PASS: Loop timing acceptable for audio");
    }
    
    if (audioUnderrunCount > 0) {
        Logger.println("   ❌ FAIL: Audio underruns detected");
    } else {
        Logger.println("   ✅ PASS: No audio underruns");
    }
    
    if (totalBytesWritten == 0) {
        Logger.println("   ❌ FAIL: Zero bytes written - no audio reaching DAC");
    } else if (totalBytesWritten < expectedBytes * 0.5f) {
        Logger.printf("   ⚠️ WARN: Low throughput (%.0f%%) - audio may stutter\n",
                      totalBytesWritten * 100.0f / expectedBytes);
    } else {
        Logger.printf("   ✅ PASS: Audio throughput %.0f%%\n",
                      totalBytesWritten * 100.0f / expectedBytes);
    }
    
    if (dtmfDetectCount > 0) {
        Logger.println("   ⚠️ WARN: False DTMF detections - increase threshold");
    } else {
        Logger.println("   ✅ PASS: No false DTMF detections");
    }
    
    Logger.printf("\n💾 Free heap: %u bytes\n", ESP.getFreeHeap());
    Logger.println("============================================");
}

// ============================================================================
// AUDIO CAPTURE FOR OFFLINE ANALYSIS
// ============================================================================

void performAudioCapture(int durationSec) {
    extern AudioBoardStream kit;
    
    // Clamp duration
    if (durationSec < 1) durationSec = 1;
    if (durationSec > 20) durationSec = 20;
    
    // Downsample factor: 44100 -> 22050 Hz (take every 2nd sample)
    const int DOWNSAMPLE = 2;
    const int EFFECTIVE_RATE = AUDIO_SAMPLE_RATE / DOWNSAMPLE; // 22050
    const size_t SAMPLES_NEEDED = EFFECTIVE_RATE * durationSec;
    const size_t BUFFER_BYTES = SAMPLES_NEEDED * sizeof(int16_t);
    
    Logger.println();
    Logger.println("============================================");
    Logger.println("🎙️ AUDIO CAPTURE FOR OFFLINE ANALYSIS");
    Logger.println("============================================");
    Logger.printf("   Duration: %d seconds\n", durationSec);
    Logger.printf("   Source rate: %d Hz -> Capture rate: %d Hz\n", AUDIO_SAMPLE_RATE, EFFECTIVE_RATE);
    Logger.printf("   Samples: %u  Buffer: %u KB\n", SAMPLES_NEEDED, BUFFER_BYTES / 1024);
    Logger.printf("   Free PSRAM: %u KB\n", ESP.getFreePsram() / 1024);
    Logger.printf("   Free heap: %u KB\n", ESP.getFreeHeap() / 1024);
    
    if (BUFFER_BYTES > ESP.getFreePsram()) {
        Logger.println("   ❌ Not enough PSRAM! Reduce duration.");
        return;
    }
    
    // Allocate capture buffer in PSRAM
    int16_t* captureBuf = (int16_t*)heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    if (!captureBuf) {
        Logger.println("   ❌ PSRAM allocation failed!");
        return;
    }
    
    // Stop competing tasks
    Logger.println("   Stopping Goertzel task...");
    stopGoertzelTask();
    getExtendedAudioPlayer().stop();
    
    // Disable remote logging to prevent uptime logs from contaminating audio dump
    Logger.println("   Disabling remote logger...");
    bool wasRemoteEnabled = RemoteLogger.isEnabled();
    RemoteLogger.setEnabled(false);
    
    delay(50);
    
    // Drain any stale data in I2S input buffer
    {
        uint8_t drain[1024];
        unsigned long drainStart = millis();
        while (kit.available() > 0 && (millis() - drainStart) < 200) {
            kit.readBytes(drain, min((int)sizeof(drain), kit.available()));
        }
    }
    
    Logger.println("   🔴 RECORDING...");
    Logger.flush();
    
    // Read buffer: small fixed size on stack to avoid overflow
    const size_t READ_CHUNK = 1024; // 512 16-bit samples per read
    uint8_t readBuf[READ_CHUNK];
    size_t capturedSamples = 0;
    size_t totalSourceSamples = 0;
    size_t sourceSkipCounter = 0;
    unsigned long captureStart = millis();
    unsigned long lastDot = captureStart;
    size_t readFailCount = 0;
    
    while (capturedSamples < SAMPLES_NEEDED) {
        int avail = kit.available();
        if (avail <= 0) {
            // Brief yield to let I2S DMA fill
            delayMicroseconds(100);
            readFailCount++;
            if (readFailCount > 100000) {
                Logger.printf("\n   ⚠️ I2S read stalled at %u samples\n", capturedSamples);
                break;
            }
            continue;
        }
        readFailCount = 0;
        
        size_t toRead = min((size_t)avail, READ_CHUNK);
        // Ensure even number of bytes (16-bit samples)
        toRead &= ~1;
        if (toRead == 0) continue;
        
        size_t bytesRead = kit.readBytes(readBuf, toRead);
        size_t samplesRead = bytesRead / sizeof(int16_t);
        int16_t* samples = (int16_t*)readBuf;
        
        for (size_t i = 0; i < samplesRead && capturedSamples < SAMPLES_NEEDED; i++) {
            totalSourceSamples++;
            if (sourceSkipCounter == 0) {
                captureBuf[capturedSamples++] = samples[i];
            }
            sourceSkipCounter++;
            if (sourceSkipCounter >= (size_t)DOWNSAMPLE) {
                sourceSkipCounter = 0;
            }
        }
        
        // Progress dots every second
        if (millis() - lastDot >= 1000) {
            Logger.print(".");
            lastDot = millis();
        }
    }
    // Re-enable remote logging if it was enabled
    if (wasRemoteEnabled)
    {
        Logger.println("   Re-enabling remote logger...");
        RemoteLogger.setEnabled(true);
    }

    unsigned long captureTime = millis() - captureStart;
    Logger.println();
    Logger.printf("   ✅ Captured %u samples in %lu ms\n", capturedSamples, captureTime);
    Logger.printf("   Source samples read: %u (expected ~%u)\n", 
                  totalSourceSamples, (unsigned)(AUDIO_SAMPLE_RATE * durationSec));
    
    // Compute basic stats for sanity check
    int32_t minVal = 32767, maxVal = -32768;
    int64_t sumAbs = 0;
    for (size_t i = 0; i < capturedSamples; i++) {
        int16_t s = captureBuf[i];
        if (s < minVal) minVal = s;
        if (s > maxVal) maxVal = s;
        sumAbs += abs(s);
    }
    int32_t avgAbs = capturedSamples > 0 ? (int32_t)(sumAbs / capturedSamples) : 0;
    
    Logger.println();
    Logger.println("📊 Signal Statistics:");
    Logger.printf("   Min: %d  Max: %d  Avg|x|: %d\n", minVal, maxVal, avgAbs);
    Logger.printf("   Peak: %.1f dBFS\n", 
                  maxVal > 0 ? 20.0f * log10f((float)max(abs(minVal), abs(maxVal)) / 32768.0f) : -96.0f);
    
    // Dump as CSV via writeRaw - bypasses log buffer to avoid flooding
    // Format: one line of header, then 20 samples per line for compactness
    // Total output: ~capturedSamples * ~4 chars = manageable over serial
    Logger.println();
    Logger.println("📤 Dumping audio data (CSV signed int16)...");
    Logger.println("   Copy between BEGIN/END markers.");
    Logger.println("   Python: np.loadtxt('file.csv', delimiter=',', dtype=np.int16)");
    Logger.flush();
    delay(100);
    
    // Markers for easy parsing
    Logger.writeRawLine("---BEGIN_AUDIO_CAPTURE---");
    
    // Header comment
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "# rate=%d,bits=16,channels=1,samples=%u,duration_ms=%lu",
             EFFECTIVE_RATE, capturedSamples, captureTime);
    Logger.writeRawLine(hdr);
    
    // Dump samples, 20 per line
    const int SAMPLES_PER_LINE = 20;
    char lineBuf[256]; // 20 samples * max "-32768," = 140 chars + safety
    size_t dumped = 0;
    
    while (dumped < capturedSamples) {
        int linePos = 0;
        int lineCount = 0;
        
        while (lineCount < SAMPLES_PER_LINE && dumped < capturedSamples) {
            if (lineCount > 0) {
                lineBuf[linePos++] = ',';
            }
            int written = snprintf(lineBuf + linePos, sizeof(lineBuf) - linePos, "%d", captureBuf[dumped]);
            linePos += written;
            dumped++;
            lineCount++;
        }
        lineBuf[linePos] = '\0';
        Logger.writeRawLine(lineBuf);
        
        // Yield every 100 lines to prevent watchdog reset
        if ((dumped / SAMPLES_PER_LINE) % 100 == 0) {
            yield();
            delay(1); // Let serial TX buffer drain
        }
    }
    
    Logger.writeRawLine("---END_AUDIO_CAPTURE---");
    
    Logger.printf("\n   ✅ Dumped %u samples (%u lines)\n", dumped, (dumped + SAMPLES_PER_LINE - 1) / SAMPLES_PER_LINE);
    
    // Free PSRAM
    heap_caps_free(captureBuf);
    Logger.printf("   💾 Buffer freed. Free PSRAM: %u KB\n", ESP.getFreePsram() / 1024);
    
    
    // Restart Goertzel
    Logger.println("   Restarting Goertzel task...");
    extern StreamCopy goertzelCopier;
    resetGoertzelState();
    startGoertzelTask(goertzelCopier);
    
    Logger.println("============================================");
}

// ============================================================================
// AUDIO OUTPUT TEST - Verify I2S data flow without physical speaker access
// ============================================================================

void performAudioOutputTest() {
    extern AudioBoardStream kit;
    ExtendedAudioPlayer& ap = getExtendedAudioPlayer();

    Logger.println();
    Logger.println("============================================");
    Logger.println("🔊 AUDIO OUTPUT TEST");
    Logger.println("============================================");

    // ── 1. Current player state ──────────────────────────────────────────
    Logger.println();
    Logger.println("📋 Player State:");
    Logger.printf("   Active:       %s\n", ap.isActive() ? "YES" : "no");
    Logger.printf("   Current key:  %s\n", ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(none)");
    Logger.printf("   Stream type:  %d\n", (int)ap.getCurrentStreamType());
    Logger.printf("   Queue depth:  %d\n", (int)ap.getQueueSize());
    Logger.printf("   Volume:       %.2f\n", ap.getVolume());
    Logger.printf("   Hook state:   %s\n", Phone.isOffHook() ? "OFF HOOK" : "ON HOOK");
    Logger.printf("   Download Q:   %d pending\n", getDownloadQueueCount());

    // ── 2. Registry contents ──────────────────────────────────────────────
    AudioKeyRegistry* reg = ap.getRegistry();
    if (!reg) {
        Logger.println("❌ No registry set on player!");
        Logger.println("============================================");
        return;
    }
    Logger.printf("   Registry:     %d keys\n", (int)reg->size());
    if (reg->size() == 0) {
        Logger.println("❌ Registry is empty - audio catalog not loaded");
        Logger.println("   Check SD card and WiFi connectivity");
        Logger.println("============================================");
        return;
    }
    // List all registered keys
    Logger.println();
    Logger.println("🔑 Registered keys:");
    for (auto it = reg->begin(); it != reg->end(); ++it) {
        Logger.printf("   %s -> %s (type:%d)\n", it->first.c_str(),
                      it->second.getPath() ? it->second.getPath() : "(no path)",
                      (int)it->second.type);
    }

    // ── 3. Check if dialtone key exists ──────────────────────────────────
    bool hasDT = ap.hasAudioKey("dialtone");
    Logger.printf("\n   dialtone key: %s\n", hasDT ? "registered" : "NOT FOUND");
    Logger.printf("   dialtone key: %s\n", hasDT ? "registered" : "NOT FOUND");
    if (!hasDT) {
        Logger.println("❌ Cannot test - 'dialtone' audioKey not registered");
        Logger.println("============================================");
        return;
    }

    // ── 4. Start dialtone playback ───────────────────────────────────────
    Logger.println();
    Logger.println("▶️  Starting dialtone playback...");
    bool started = ap.playAudioKey("dialtone");
    Logger.printf("   playAudioKey returned: %s\n", started ? "true" : "FALSE");
    if (!started) {
        Logger.println("❌ Failed to start dialtone - check logs above");
        Logger.println("============================================");
        return;
    }
    delay(50);  // let first DMA transfer populate

    Logger.printf("   Active after start: %s\n", ap.isActive() ? "YES" : "no");
    Logger.printf("   Playing key:        %s\n", ap.getCurrentAudioKey());

    // ── 5. I2S buffer flow test ──────────────────────────────────────────
    //   Measure kit.availableForWrite() before and after each copy().
    //   A decrease means the player wrote data into the I2S TX buffer.
    const int TEST_DURATION_MS = 3000;
    const int SAMPLE_INTERVAL_MS = 50;
    const int SAMPLES = TEST_DURATION_MS / SAMPLE_INTERVAL_MS;

    Logger.println();
    Logger.printf("🔬 Monitoring I2S output for %d ms...\n", TEST_DURATION_MS);

    int copyTrueCount = 0;
    int copyFalseCount = 0;
    int playerActiveCount = 0;
    int playerInactiveCount = 0;
    long totalBytesWritten = 0;   // estimated from buffer delta
    int maxAvail = 0;
    int minAvail = INT_MAX;
    long totalAvail = 0;
    int restartCount = 0;
    unsigned long testStart = millis();

    for (int i = 0; i < SAMPLES; i++) {
        int availBefore = kit.availableForWrite();

        bool copyResult = ap.copy();

        int availAfter = kit.availableForWrite();

        // If availableForWrite decreased, data was written
        int delta = availBefore - availAfter;
        if (delta > 0) totalBytesWritten += delta;

        if (copyResult) copyTrueCount++; else copyFalseCount++;
        if (ap.isActive()) playerActiveCount++; else playerInactiveCount++;

        if (availAfter < minAvail) minAvail = availAfter;
        if (availAfter > maxAvail) maxAvail = availAfter;
        totalAvail += availAfter;

        // Detect if player died and had to restart
        if (!ap.isActive() && i < SAMPLES - 1) {
            restartCount++;
            ap.playAudioKey("dialtone");
            delay(10);
        }

        delay(SAMPLE_INTERVAL_MS);
    }

    unsigned long elapsed = millis() - testStart;

    // ── 6. Results ───────────────────────────────────────────────────────
    Logger.println();
    Logger.println("📊 Results:");
    Logger.printf("   Duration:          %lu ms\n", elapsed);
    Logger.printf("   copy() true/false: %d / %d\n", copyTrueCount, copyFalseCount);
    Logger.printf("   Player active/not: %d / %d\n", playerActiveCount, playerInactiveCount);
    Logger.printf("   Restarts needed:   %d\n", restartCount);
    Logger.println();
    Logger.println("🔊 I2S TX Buffer (availableForWrite):");
    Logger.printf("   Min: %d  Max: %d  Avg: %ld\n", minAvail, maxAvail,
                  SAMPLES > 0 ? totalAvail / SAMPLES : 0);
    Logger.printf("   Est. bytes written: %ld\n", totalBytesWritten);

    float expectedBytesPerSec = (float)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
    float expectedBytes = expectedBytesPerSec * elapsed / 1000.0f;
    Logger.printf("   Expected bytes:     %.0f (%.0f B/s @ %dHz/%dch/%dbit)\n",
                  expectedBytes, expectedBytesPerSec,
                  AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS_PER_SAMPLE);

    if (totalBytesWritten > 0) {
        float ratio = totalBytesWritten / expectedBytes * 100.0f;
        Logger.printf("   Throughput ratio:   %.1f%%\n", ratio);
    }

    // ── 7. Final state ───────────────────────────────────────────────────
    Logger.println();
    Logger.printf("   Player still active: %s\n", ap.isActive() ? "YES" : "no");
    Logger.printf("   Current key:         %s\n",
                  ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(none)");

    // ── 8. Assessment ────────────────────────────────────────────────────
    Logger.println();
    Logger.println("📋 Assessment:");
    if (copyTrueCount == 0) {
        Logger.println("   ❌ FAIL: copy() never returned true - no audio pipeline activity");
    } else if (copyFalseCount > copyTrueCount) {
        Logger.println("   ⚠️  WARN: copy() mostly false - pipeline stalling");
    } else {
        Logger.printf("   ✅ PASS: copy() active %d/%d samples\n", copyTrueCount, SAMPLES);
    }

    if (totalBytesWritten == 0) {
        Logger.println("   ❌ FAIL: Zero bytes written to I2S - no audio reaching DAC");
    } else if (totalBytesWritten < expectedBytes * 0.3f) {
        Logger.printf("   ⚠️  WARN: Low throughput (%.0f%% of expected)\n",
                      totalBytesWritten / expectedBytes * 100.0f);
    } else {
        Logger.println("   ✅ PASS: Audio data flowing to I2S");
    }

    if (restartCount > 0) {
        Logger.printf("   ⚠️  WARN: Player stopped %d times during test\n", restartCount);
    } else {
        Logger.println("   ✅ PASS: Playback sustained for full test");
    }

    Logger.printf("\n💾 Free heap: %u bytes\n", ESP.getFreeHeap());
    Logger.println("============================================");

    // Stop dialtone unless phone is off hook
    if (!Phone.isOffHook()) {
        ap.stop();
        Logger.println("⏹️  Stopped dialtone (phone is on-hook)");
    } else {
        Logger.println("📞 Leaving dialtone running (phone is off-hook)");
    }
}

// ============================================================================
// DEBUG INPUT — Full E2E integration test of the phone call state machine
// ============================================================================
//
// Test sequence:
//   0. Load raw audio from SD (or download CSV from GitHub and convert)
//   1. Clean start (on-hook)
//   2. Off-hook → validate dialtone plays and writes bytes
//   3. Let live Goertzel run 1 s (no false DTMF from dialtone)
//   4. Stop Goertzel, feed test audio → collect DTMF detections
//   5. Feed digits via main-loop pump → validate sequence + audio
//   5b. Replay test audio while locked → verify digits rejected
//   6. Wait real OFF_HOOK_TIMEOUT_MS → validate warning tone
//   6b. Replay test audio again → still locked
//   7. On-hook → validate full reset
//   8. Digits rejected while on-hook
//   9. Wait 1 s, off-hook again
//  10. Enter 6969 → validate sequence plays
//  11. Hang up → validate cleanup
// ============================================================================

// GitHub raw base for fallback CSV download
#define GITHUB_RAW_BASE "https://raw.githubusercontent.com/jeff-hamm/bowie-phone/main/"

// Helper: pump the main-loop audio + state + off-hook-timeout logic.
// Faithfully replicates loop() off-hook path. Returns bytes pumped.
static bool s_pumpWarningPlayed = false;
static unsigned long s_pumpWarningTime = 0;

static size_t pumpMainLoop(unsigned long durationMs) {
    ExtendedAudioPlayer& ap = getExtendedAudioPlayer();
    size_t totalBytes = 0;

    unsigned long start = millis();
    while (millis() - start < durationMs) {
        if (!Phone.isOffHook()) break;

        // ── Off-hook timeout (mirrors main loop) ──
        unsigned long now = millis();
        unsigned long lastActivity = max(getLastDigitTime(), ap.getLastActive());
        if (!s_pumpWarningPlayed && lastActivity > 0 &&
            (now - lastActivity) >= OFF_HOOK_TIMEOUT_MS) {
            Logger.println("   ⏰ Off-hook timeout fired");
            ap.playAudioKey("off_hook");
            s_pumpWarningPlayed = true;
            s_pumpWarningTime = now;
        }
        if (s_pumpWarningPlayed) {
            if (getLastDigitTime() > s_pumpWarningTime) {
                s_pumpWarningPlayed = false;
            } else if (!ap.isActive() && (now - s_pumpWarningTime) > 2000) {
                s_pumpWarningPlayed = false;
            }
        }

        // ── Audio + Goertzel mute (mirrors main loop) ──
        if (ap.isActive()) {
            ap.copy();
            totalBytes += ap.getLastCopyBytes();
            bool playingDialtone = ap.isAudioKeyPlaying("dialtone");
            setGoertzelMuted(!playingDialtone);
        } else {
            setGoertzelMuted(isSequenceLocked());
        }

        // ── Goertzel key + sequence (mirrors main loop) ──
        char key = getGoertzelKey();
        if (key != 0) addDtmfDigit(key);
        if (isSequenceReady()) readDTMFSequence(true);

        yield();
        delay(1);
    }
    return totalBytes;
}

// Reset the pump's off-hook timeout tracking (call at start of each test phase)
static void resetPumpState() {
    s_pumpWarningPlayed = false;
    s_pumpWarningTime = 0;
}

// Helper: replay the test audio buffer through GoertzelStream.
// Returns number of DTMF digits detected.  Detected chars appended to |out|.
static int replayAudioThroughGoertzel(
    uint8_t* buffer, size_t fileSize,
    GoertzelStream& goertzel, const PhoneConfig& config,
    char* out, int outSize, int* outPos)
{
    MemoryStream memStream(buffer, fileSize, true, FLASH_RAM);
    memStream.begin();
    StreamCopy replayCopier(goertzel, memStream);
    replayCopier.resize(config.goertzelCopierBufferSize);

    size_t totalProcessed = 0;
    int detections = 0;

    while (totalProcessed < fileSize) {
        size_t copied = replayCopier.copy();
        if (copied == 0) break;
        totalProcessed += copied;
        processGoertzelBlock();

        char key = getGoertzelKey();
        if (key != 0) {
            detections++;
            float offsetSec = (float)totalProcessed / sizeof(int16_t) / AUDIO_SAMPLE_RATE;
            Logger.printf("   🎵 DTMF '%c' at %.3f s\n", key, offsetSec);
            if (out && *outPos < outSize - 1) {
                out[(*outPos)++] = key;
                out[*outPos] = '\0';
            }
        }
        if ((totalProcessed / 4096) % 50 == 0) yield();
    }
    // Check final pending
    char finalKey = getGoertzelKey();
    if (finalKey != 0) {
        detections++;
        Logger.printf("   🎵 DTMF '%c' at end\n", finalKey);
        if (out && *outPos < outSize - 1) {
            out[(*outPos)++] = finalKey;
            out[*outPos] = '\0';
        }
    }
    return detections;
}

// Helper: download CSV from GitHub, convert to raw int16 LE, save to SD.
// CSV format: # comment lines, ---BEGIN_AUDIO_CAPTURE--- marker,
// # rate=22050,... header, then comma-separated int16 values (20/line).
// Upsamples 2x (22050→44100) by duplicating each sample.
// Returns true on success.
static bool downloadAndConvertCSV(const char* rawPath) {
    // Construct URL: GITHUB_RAW_BASE + logs/<OTA_HOSTNAME>.csv
    String url = String(GITHUB_RAW_BASE) + "logs/" + OTA_HOSTNAME + ".csv";
    Logger.printf("   Downloading: %s\n", url.c_str());

    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(url)) {
        Logger.println("   ❌ HTTP begin failed");
        return false;
    }
    int httpCode = http.GET();
    if (httpCode != 200) {
        Logger.printf("   ❌ HTTP %d\n", httpCode);
        http.end();
        return false;
    }

    // Stream-parse CSV → binary file on SD
    File outFile = SD_MMC.open(rawPath, FILE_WRITE);
    if (!outFile) {
        Logger.printf("   ❌ Cannot create %s\n", rawPath);
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    bool inData = false;
    size_t samplesWritten = 0;
    char lineBuf[512];
    int linePos = 0;

    while (http.connected() || stream->available()) {
        if (!stream->available()) { delay(1); continue; }
        char c = stream->read();
        if (c == '\n' || c == '\r') {
            if (linePos == 0) continue;
            lineBuf[linePos] = '\0';
            linePos = 0;

            // Check for end marker
            if (strstr(lineBuf, "---END_AUDIO_CAPTURE---")) break;

            // Check for begin marker
            if (strstr(lineBuf, "---BEGIN_AUDIO_CAPTURE---")) {
                inData = true;
                continue;
            }

            // Skip comment lines
            if (lineBuf[0] == '#') continue;

            // Parse comma-separated int16 values (only after begin marker)
            if (!inData) continue;
            char* ptr = lineBuf;
            while (*ptr) {
                // Skip whitespace/commas
                while (*ptr == ',' || *ptr == ' ' || *ptr == '\t') ptr++;
                if (*ptr == '\0') break;
                int16_t val = (int16_t)atoi(ptr);
                // Upsample 2x: duplicate each sample (22050 → 44100)
                outFile.write((uint8_t*)&val, sizeof(val));
                outFile.write((uint8_t*)&val, sizeof(val));
                samplesWritten += 2;
                // Advance past number
                if (*ptr == '-') ptr++;
                while (*ptr >= '0' && *ptr <= '9') ptr++;
            }

            if (samplesWritten % 44100 == 0) yield();
        } else if (linePos < (int)sizeof(lineBuf) - 1) {
            lineBuf[linePos++] = c;
        }
    }

    outFile.close();
    http.end();

    Logger.printf("   ✅ Converted: %u samples (%.2f s at %d Hz) → %s\n",
                  (unsigned)samplesWritten,
                  (float)samplesWritten / AUDIO_SAMPLE_RATE,
                  AUDIO_SAMPLE_RATE, rawPath);
    return samplesWritten > 0;
}

void performDebugInput(const char* filename) {
    extern GoertzelStream goertzel;
    extern StreamCopy goertzelCopier;
    ExtendedAudioPlayer& ap = getExtendedAudioPlayer();

    int passed = 0, failed = 0;
    #define TEST_CHECK(label, cond) do { \
        if (cond) { Logger.printf("   ✅ PASS: %s\n", label); passed++; } \
        else      { Logger.printf("   ❌ FAIL: %s\n", label); failed++; } \
    } while(0)

    Logger.println();
    Logger.println("============================================");
    Logger.println("🔬 DEBUG INPUT — E2E INTEGRATION TEST");
    Logger.println("============================================");
    Logger.printf("   File: %s\n", filename);

    // ── Step 0: Load or download test audio ─────────────────────────────
    Logger.println();
    Logger.println("── Step 0: Load test audio ─────────────────");

    if (!SD_MMC.exists(filename)) {
        Logger.println("   File not on SD — attempting GitHub download...");
        if (!downloadAndConvertCSV(filename)) {
            Logger.println("   ❌ Download failed. Upload via: POST /upload");
            Logger.println("============================================");
            return;
        }
    }

    File file = SD_MMC.open(filename, FILE_READ);
    if (!file) {
        Logger.printf("   ❌ Cannot open %s\n", filename);
        Logger.println("============================================");
        return;
    }

    size_t fileSize = file.size();
    size_t sampleCount = fileSize / sizeof(int16_t);
    float fileDurationSec = (float)sampleCount / AUDIO_SAMPLE_RATE;

    Logger.printf("   Size: %u bytes (%u samples, %.2f s at %d Hz)\n",
                  (unsigned)fileSize, (unsigned)sampleCount, fileDurationSec, AUDIO_SAMPLE_RATE);

    if (fileSize > ESP.getFreePsram()) {
        Logger.println("   ❌ Not enough PSRAM!");
        file.close();
        Logger.println("============================================");
        return;
    }

    uint8_t* buffer = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        Logger.println("   ❌ PSRAM allocation failed!");
        file.close();
        Logger.println("============================================");
        return;
    }

    size_t bytesRead = file.read(buffer, fileSize);
    file.close();
    if (bytesRead != fileSize) {
        Logger.printf("   ❌ Read error: got %u of %u bytes\n", (unsigned)bytesRead, (unsigned)fileSize);
        heap_caps_free(buffer);
        Logger.println("============================================");
        return;
    }
    Logger.println("   ✅ File loaded into PSRAM");

    const PhoneConfig& config = getPhoneConfig();

    // ── Step 1: Ensure clean starting state (on-hook) ───────────────────
    Logger.println();
    Logger.println("── Step 1: Clean start (on-hook) ───────────");
    if (Phone.isOffHook()) {
        Phone.setOffHook(false);
        delay(50);
    }
    ap.stop();
    resetDTMFSequence();
    TEST_CHECK("Phone is on-hook", !Phone.isOffHook());
    TEST_CHECK("Player is idle", !ap.isActive());
    TEST_CHECK("Sequence not locked", !isSequenceLocked());

    // ── Step 2: Go off-hook → dialtone should start ─────────────────────
    Logger.println();
    Logger.println("── Step 2: Off-hook → dialtone ─────────────");
    resetPumpState();
    Phone.setOffHook(true);
    delay(50);  // Let hook callback fire

    TEST_CHECK("Phone is off-hook", Phone.isOffHook());
    TEST_CHECK("Dialtone playing", ap.isAudioKeyPlaying("dialtone"));

    // Pump audio for 200 ms to confirm bytes are flowing
    size_t dialtoneBytes = pumpMainLoop(200);
    TEST_CHECK("Dialtone writing bytes", dialtoneBytes > 0);
    Logger.printf("   (pumped %u bytes in 200 ms)\n", (unsigned)dialtoneBytes);

    // ── Step 3: Let live Goertzel run for 1 s ───────────────────────────
    Logger.println();
    Logger.println("── Step 3: Live Goertzel for 1 s ───────────");
    Logger.println("   Goertzel task still running on core 0...");

    // Drain any stale detection
    getGoertzelKey();

    int liveDetections = 0;
    unsigned long liveStart = millis();
    while (millis() - liveStart < 1000) {
        if (ap.isActive()) ap.copy();
        char key = getGoertzelKey();
        if (key != 0) {
            Logger.printf("   ⚠️  Live Goertzel detected '%c' during dialtone\n", key);
            liveDetections++;
        }
        delay(5);
    }
    TEST_CHECK("No false DTMF during dialtone", liveDetections == 0);

    // ── Step 4: Stop Goertzel task, feed test audio ─────────────────────
    Logger.println();
    Logger.println("── Step 4: Feed test audio → Goertzel ──────");
    stopGoertzelTask();
    delay(50);
    ap.stop();
    resetGoertzelState();
    setGoertzelMuted(false);

    Logger.printf("   Block: %d samples (%.1f ms), Threshold: %.1f, Floor: %.1f\n",
                  config.goertzelBlockSize,
                  config.goertzelBlockSize * 1000.0f / AUDIO_SAMPLE_RATE,
                  config.fundamentalMagnitudeThreshold,
                  config.minDetectionMagnitude);

    char detectedDigits[32];
    int digitPos = 0;
    detectedDigits[0] = '\0';
    unsigned long replayStart = millis();
    int detectionCount = replayAudioThroughGoertzel(
        buffer, fileSize, goertzel, config,
        detectedDigits, sizeof(detectedDigits), &digitPos);
    unsigned long replayElapsed = millis() - replayStart;

    Logger.printf("   Processed in %lu ms (%.1fx realtime)\n",
                  replayElapsed, fileDurationSec * 1000.0f / max(replayElapsed, 1UL));
    Logger.printf("   Detected digits: \"%s\" (%d total)\n", detectedDigits, detectionCount);
    TEST_CHECK("At least one DTMF detection", detectionCount > 0);

    // ── Step 5: Feed digits via main-loop pump ──────────────────────────
    // Uses pumpMainLoop() which faithfully replicates the off-hook path:
    // audio copy + Goertzel mute + sequence ready check + timeout logic.
    Logger.println();
    Logger.println("── Step 5: Sequence via main loop ──────────");

    // Restart Goertzel task (normal operation)
    resetGoertzelState();
    startGoertzelTask(goertzelCopier);

    // Re-play dialtone so first digit stops it (matching real flow)
    ap.playAudioKey("dialtone");
    pumpMainLoop(50);

    if (digitPos > 0) {
        Logger.printf("   Feeding %d digits: \"%s\"\n", digitPos, detectedDigits);
        for (int i = 0; i < digitPos; i++) {
            addDtmfDigit(detectedDigits[i]);
            pumpMainLoop(10);  // Let main loop process between digits
        }

        // Pump main loop up to 3 s — it will call readDTMFSequence when ready
        Logger.println("   Pumping main loop for sequence processing...");
        unsigned long seqStart = millis();
        while (millis() - seqStart < 3000) {
            pumpMainLoop(50);
            if (isSequenceLocked()) break;  // Sequence matched and locked
            if (!isReadingSequence() && !isSequenceReady()) break;  // Nothing pending
        }

        TEST_CHECK("Dialtone stopped after first digit", !ap.isAudioKeyPlaying("dialtone"));

        bool audioStarted = ap.isActive();
        TEST_CHECK("Audio started after sequence", audioStarted);
        if (audioStarted) {
            Logger.printf("   Playing: %s\n",
                          ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(playlist)");
        }
        TEST_CHECK("Sequence locked after match", isSequenceLocked());
    } else {
        Logger.println("   ⚠️  No digits to feed — skipping sequence test");
    }

    // ── Step 5b: Replay test audio while locked → no new sequences ──────
    Logger.println();
    Logger.println("── Step 5b: Replay while locked ────────────");
    // Pump audio to completion first
    Logger.println("   Finishing active audio...");
    unsigned long finishStart = millis();
    while (ap.isActive() && (millis() - finishStart < 15000)) {
        pumpMainLoop(100);
    }

    TEST_CHECK("Sequence still locked", isSequenceLocked());
    Logger.println("   Replaying test audio (should detect nothing usable)...");

    // Stop Goertzel task to feed replay data
    stopGoertzelTask();
    delay(50);
    resetGoertzelState();
    setGoertzelMuted(false);

    char lockedDigits[32];
    int lockedPos = 0;
    lockedDigits[0] = '\0';
    int lockedDetections = replayAudioThroughGoertzel(
        buffer, fileSize, goertzel, config,
        lockedDigits, sizeof(lockedDigits), &lockedPos);

    Logger.printf("   Goertzel detected %d digits: \"%s\"\n", lockedDetections, lockedDigits);
    // Even if Goertzel detects tones, addDtmfDigit should reject them
    const char* seqBefore5b = getSequence();
    for (int i = 0; i < lockedPos; i++) {
        addDtmfDigit(lockedDigits[i]);
    }
    TEST_CHECK("Digits rejected while locked", strlen(getSequence()) == strlen(seqBefore5b));

    // Restart Goertzel for next phase
    resetGoertzelState();
    startGoertzelTask(goertzelCopier);

    // ── Step 6: Wait real OFF_HOOK_TIMEOUT_MS for warning tone ──────────
    Logger.println();
    Logger.println("── Step 6: Off-hook timeout (real wait) ────");
    Logger.printf("   Waiting up to %d s for off-hook timeout...\n",
                  (OFF_HOOK_TIMEOUT_MS / 1000) + 5);
    Logger.println("   (lastActivity will age out naturally)");

    bool warningDetected = false;
    unsigned long timeoutStart = millis();
    unsigned long maxWait = OFF_HOOK_TIMEOUT_MS + 5000; // 5 s margin

    while (millis() - timeoutStart < maxWait) {
        pumpMainLoop(500);
        // Check if off_hook audio started
        if (ap.isAudioKeyPlaying("off_hook")) {
            warningDetected = true;
            Logger.printf("   ⏰ off_hook warning detected at +%lu ms\n",
                          millis() - timeoutStart);
            break;
        }
        // Print progress every 5 s
        unsigned long elapsed = millis() - timeoutStart;
        if (elapsed % 5000 < 500) {
            Logger.printf("   ... waiting (%lu / %lu ms)\n", elapsed, maxWait);
        }
    }
    TEST_CHECK("Off-hook warning tone played", warningDetected);

    // Pump warning audio to completion
    if (ap.isActive()) {
        Logger.println("   Pumping warning audio...");
        unsigned long warnPump = millis();
        while (ap.isActive() && (millis() - warnPump < 10000)) {
            pumpMainLoop(100);
        }
    }

    // ── Step 6b: Replay test audio again after timeout → still locked ───
    Logger.println();
    Logger.println("── Step 6b: Replay after timeout ───────────");
    TEST_CHECK("Still locked after timeout", isSequenceLocked());

    stopGoertzelTask();
    delay(50);
    resetGoertzelState();
    setGoertzelMuted(false);

    char timeoutDigits[32];
    int timeoutPos = 0;
    timeoutDigits[0] = '\0';
    int timeoutDetections = replayAudioThroughGoertzel(
        buffer, fileSize, goertzel, config,
        timeoutDigits, sizeof(timeoutDigits), &timeoutPos);

    Logger.printf("   Goertzel detected %d digits\n", timeoutDetections);
    const char* seqBefore6b = getSequence();
    for (int i = 0; i < timeoutPos; i++) {
        addDtmfDigit(timeoutDigits[i]);
    }
    TEST_CHECK("Digits still rejected (locked)", strlen(getSequence()) == strlen(seqBefore6b));

    resetGoertzelState();
    startGoertzelTask(goertzelCopier);

    // ── Step 7: On-hook → validate full reset ───────────────────────────
    Logger.println();
    Logger.println("── Step 7: On-hook → reset ─────────────────");
    Phone.setOffHook(false);
    delay(50);

    TEST_CHECK("Phone is on-hook", !Phone.isOffHook());
    TEST_CHECK("Player stopped", !ap.isActive());
    TEST_CHECK("Sequence reset (not locked)", !isSequenceLocked());
    TEST_CHECK("Sequence buffer clear", strlen(getSequence()) == 0);

    // ── Step 8: Digits rejected while on-hook ───────────────────────────
    Logger.println();
    Logger.println("── Step 8: Digits rejected on-hook ─────────");
    addDtmfDigit('5');
    delay(10);
    TEST_CHECK("No audio playing after on-hook digit", !ap.isActive());
    resetDTMFSequence();

    // ── Step 9: Wait 1 s, then pick up again ────────────────────────────
    Logger.println();
    Logger.println("── Step 9: Wait 1 s, off-hook again ────────");
    delay(1000);
    resetPumpState();
    Phone.setOffHook(true);
    delay(50);

    TEST_CHECK("Phone is off-hook again", Phone.isOffHook());
    TEST_CHECK("Dialtone restarted", ap.isAudioKeyPlaying("dialtone"));
    size_t dt2Bytes = pumpMainLoop(200);
    TEST_CHECK("Dialtone writing bytes", dt2Bytes > 0);

    // ── Step 10: Enter 6969 → validate sequence plays ───────────────────
    Logger.println();
    Logger.println("── Step 10: Dial 6969 ──────────────────────");
    Logger.println("   Feeding digits: 6-9-6-9");
    const char* dialSequence = "6969";
    for (int i = 0; dialSequence[i]; i++) {
        addDtmfDigit(dialSequence[i]);
        pumpMainLoop(10);
    }

    // Pump main loop — let it process the sequence
    Logger.println("   Pumping main loop for sequence...");
    unsigned long dialStart = millis();
    while (millis() - dialStart < 5000) {
        pumpMainLoop(50);
        if (isSequenceLocked()) break;
        if (!isReadingSequence() && !isSequenceReady()) break;
    }

    TEST_CHECK("Dialtone stopped", !ap.isAudioKeyPlaying("dialtone"));
    bool dialAudioPlaying = ap.isActive();
    TEST_CHECK("Audio playing after 6969", dialAudioPlaying);
    if (dialAudioPlaying) {
        Logger.printf("   Playing: %s\n",
                      ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(playlist)");
    }

    // Confirm bytes are being written
    size_t dialBytes = pumpMainLoop(500);
    TEST_CHECK("Audio writing bytes", dialBytes > 0);
    Logger.printf("   (pumped %u bytes in 500 ms)\n", (unsigned)dialBytes);

    // ── Step 11: Hang up → final cleanup ────────────────────────────────
    Logger.println();
    Logger.println("── Step 11: Hang up → cleanup ──────────────");
    Phone.setOffHook(false);
    delay(50);

    TEST_CHECK("Phone is on-hook", !Phone.isOffHook());
    TEST_CHECK("Player stopped", !ap.isActive());
    TEST_CHECK("Sequence reset (not locked)", !isSequenceLocked());
    TEST_CHECK("Sequence buffer clear", strlen(getSequence()) == 0);

    // ── Cleanup ─────────────────────────────────────────────────────────
    Logger.println();
    Logger.println("── Cleanup ─────────────────────────────────");
    heap_caps_free(buffer);
    Logger.printf("   PSRAM freed. Available: %u KB\n", ESP.getFreePsram() / 1024);

    Phone.resetDebugOverride();
    Logger.println("   Hook override cleared");

    // ── Summary ─────────────────────────────────────────────────────────
    Logger.println();
    Logger.println("============================================");
    Logger.printf("📊 E2E TEST RESULTS: %d passed, %d failed\n", passed, failed);
    if (failed == 0) {
        Logger.println("   🎉 ALL TESTS PASSED");
    } else {
        Logger.println("   ⚠️  SOME TESTS FAILED — review above");
    }
    Logger.println("============================================");

    #undef TEST_CHECK
}

// ============================================================================
// SD CARD DEBUG - Test various initialization methods
// ============================================================================

void performSDCardDebug() {
    Logger.println();
    Logger.println("============================================");
    Logger.println("💾 SD CARD INITIALIZATION DEBUG");
    Logger.println("============================================");
    
    // Stop any audio that might be using SD
    getExtendedAudioPlayer().stop();
    delay(100);
    
    // Show current build config
    Logger.printf("📋 Build Config: SD_USE_MMC=%d ", SD_USE_MMC);
    Logger.println(SD_USE_MMC ? "(compiled for SD_MMC)" : "(compiled for SPI)");
    Logger.printf("   Config pins: CS=%d CLK=%d MOSI=%d MISO=%d\n", 
                  SD_CS_PIN, SD_CLK_PIN, SD_MOSI_PIN, SD_MISO_PIN);
    Logger.println("   Testing ALL methods with pin variations...");
    Logger.println();
    
    // ========================================================================
    // SD_MMC TESTS - Hardware SDMMC peripheral
    // ========================================================================
    Logger.println("════════════════════════════════════════════");
    Logger.println("SD_MMC MODE TESTS (Hardware SDMMC)");
    Logger.println("════════════════════════════════════════════");
    Logger.println();
    
    // Test 1: SD_MMC 1-bit mode, no format
    Logger.println("Test 1: SD_MMC.begin(\"/sdcard\", true) - 1-bit, no format");
    if (SD_MMC.begin("/sdcard", true)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d ", cardType);
        switch(cardType) {
            case 0: Logger.println("(NONE)"); break;
            case 1: Logger.println("(MMC)"); break;
            case 2: Logger.println("(SD)"); break;
            case 3: Logger.println("(SDHC)"); break;
            default: Logger.println("(UNKNOWN)"); break;
        }
        if (cardType != 0) {
            uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Logger.printf("   Card Size: %llu MB\n", cardSize);
            uint64_t usedBytes = SD_MMC.usedBytes() / (1024 * 1024);
            Logger.printf("   Used: %llu MB\n", usedBytes);
        }
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);
    
    // Test 2: SD_MMC 1-bit mode, with format_if_mount_failed
    Logger.println();
    Logger.println("Test 2: SD_MMC.begin(\"/sdcard\", true, true) - 1-bit, format on fail");
    if (SD_MMC.begin("/sdcard", true, true)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0) {
            uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Logger.printf("   Card Size: %llu MB\n", cardSize);
        }
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);
    
    // Test 3: SD_MMC 4-bit mode
    Logger.println();
    Logger.println("Test 3: SD_MMC.begin(\"/sdcard\", false) - 4-bit mode");
    if (SD_MMC.begin("/sdcard", false)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0) {
            uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Logger.printf("   Card Size: %llu MB\n", cardSize);
        }
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);
    
    // Test 4: SD_MMC with different mount point
    Logger.println();
    Logger.println("Test 4: SD_MMC.begin(\"/sd\", true) - 1-bit, different mount");
    if (SD_MMC.begin("/sd", true)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0) {
            uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Logger.printf("   Card Size: %llu MB\n", cardSize);
        }
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);
    
    // Test 5: SD_MMC with max files parameter
    Logger.println();
    Logger.println("Test 5: SD_MMC.begin(\"/sdcard\", true, false, SDMMC_FREQ_DEFAULT, 5)");
    if (SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0) {
            uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Logger.printf("   Card Size: %llu MB\n", cardSize);
        }
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);
    
    // ========================================================================
    // SPI SD TESTS - Software SPI mode with PIN VARIATIONS
    // ========================================================================
    Logger.println();
    Logger.println("════════════════════════════════════════════");
    Logger.println("SPI SD MODE TESTS (Software SPI)");
    Logger.println("════════════════════════════════════════════");
    Logger.println("Testing multiple pin configurations...");
    Logger.println();
    
    // Common ESP32-A1S AudioKit SD pin configurations to try
    struct SPIPinConfig {
        const char* name;
        int cs;
        int clk;
        int mosi;
        int miso;
    };
    
    SPIPinConfig spiConfigs[] = {
        {"Config.h default", SD_CS_PIN, SD_CLK_PIN, SD_MOSI_PIN, SD_MISO_PIN},
        {"Alt 1 (13,14,15,2)", 13, 14, 15, 2},
        {"Alt 2 (VSPI)", 5, 18, 23, 19},
        {"Alt 3 (HSPI)", 15, 14, 13, 12},
        {"Alt 4 (MOSI/MISO swap)", SD_CS_PIN, SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN},
        {"Alt 5 (5,14,15,2)", 5, 14, 15, 2},
        {"Alt 6 (4,14,15,2)", 4, 14, 15, 2},
    };
    
    int testNum = 6;
    bool spiSuccess = false;
    
    for (int cfg = 0; cfg < 7; cfg++) {
        SPIPinConfig& pins = spiConfigs[cfg];
        
        Logger.printf("Test %d: %s - CS=%d CLK=%d MOSI=%d MISO=%d\n", 
                     testNum++, pins.name, pins.cs, pins.clk, pins.mosi, pins.miso);
        
        // Try at low speed first for maximum compatibility
        SPI.begin(pins.clk, pins.miso, pins.mosi, pins.cs);
        delay(100);
        
        if (SD.begin(pins.cs, SPI, 400000)) {
            uint8_t cardType = SD.cardType();
            Logger.printf("   ✅ SUCCESS! Card Type: %d ", cardType);
            switch(cardType) {
                case 0: Logger.println("(NONE)"); break;
                case 1: Logger.println("(MMC)"); break;
                case 2: Logger.println("(SD)"); break;
                case 3: Logger.println("(SDHC)"); break;
                default: Logger.println("(UNKNOWN)"); break;
            }
            if (cardType != 0) {
                uint64_t cardSize = SD.cardSize() / (1024 * 1024);
                Logger.printf("   Card Size: %llu MB\n", cardSize);
                uint64_t usedBytes = SD.usedBytes() / (1024 * 1024);
                Logger.printf("   Used: %llu MB\n", usedBytes);
                
                // Try to list root directory
                File root = SD.open("/");
                if (root && root.isDirectory()) {
                    Logger.println("   Root directory files:");
                    File file = root.openNextFile();
                    int fileCount = 0;
                    while (file && fileCount < 5) {
                        Logger.printf("      - %s (%llu bytes)\n", file.name(), file.size());
                        file = root.openNextFile();
                        fileCount++;
                    }
                    if (fileCount == 0) {
                        Logger.println("      (empty)");
                    }
                }
                
                Logger.println();
                Logger.println("   🎯 WORKING CONFIGURATION FOUND!");
                Logger.printf("   Use: CS=%d CLK=%d MOSI=%d MISO=%d\n", 
                            pins.cs, pins.clk, pins.mosi, pins.miso);
                spiSuccess = true;
            }
            SD.end();
        } else {
            Logger.println("   ❌ FAILED");
        }
        SPI.end();
        delay(500);
        
        if (spiSuccess) break;  // Found working config, no need to continue
    }
    
    if (!spiSuccess) {
        Logger.println();
        Logger.println("⚠️  No SPI pin configuration worked");
        Logger.println("   Trying additional diagnostics...");
        Logger.println();
        
        // Test if SPI bus is working at all
        Logger.println("Test: SPI bus basic functionality");
        SPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
        pinMode(SD_CS_PIN, OUTPUT);
        digitalWrite(SD_CS_PIN, HIGH);
        delay(10);
        digitalWrite(SD_CS_PIN, LOW);
        SPI.transfer(0xFF);
        digitalWrite(SD_CS_PIN, HIGH);
        Logger.println("   SPI transfer completed (bus functional)");
        SPI.end();
        delay(500);
    }
    
    // ========================================================================
    // AUDIOKIT SD_ACTIVE TEST
    // ========================================================================
    Logger.println();
    Logger.println("═══════════════════════════════════════════=");
    Logger.println("AUDIOKIT SD_ACTIVE TEST");
    Logger.println("═══════════════════════════════════════════=");
    Logger.println("Testing if AudioKit can initialize SD card...");
    Logger.println();
    
    extern AudioBoardStream kit;
    
    Logger.println("Test: AudioKit with cfg.sd_active = true");
    Logger.println("   Restarting AudioKit...");
    kit.end();
    delay(500);
    
    auto cfg = kit.defaultConfig(RXTX_MODE);
    cfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    cfg.sd_active = true;  // Let AudioKit initialize SD
    
    if (!kit.begin(cfg)) {
        Logger.println("   ❌ AudioKit init failed");
    } else {
        Logger.println("   AudioKit restarted");
        delay(1000);  // Give SD time to initialize
        
        // Check if SD is accessible
        bool sdWorks = false;
        
#if SD_USE_MMC
        if (SD_MMC.cardType() != CARD_NONE) {
            Logger.printf("   ✅ SD_MMC accessible! Card Type: %d\n", SD_MMC.cardType());
            uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Logger.printf("   Card Size: %llu MB\n", cardSize);
            sdWorks = true;
        }
#else
        if (SD.cardType() != CARD_NONE) {
            Logger.printf("   ✅ SD accessible! Card Type: %d\n", SD.cardType());
            uint64_t cardSize = SD.cardSize() / (1024 * 1024);
            Logger.printf("   Card Size: %llu MB\n", cardSize);
            sdWorks = true;
        }
#endif
        
        if (!sdWorks) {
            Logger.println("   ❌ SD card not accessible via AudioKit");
        }
    }
    
    // Restore AudioKit to normal config
    Logger.println();
    Logger.println("   Restoring AudioKit to sd_active=false...");
    kit.end();
    delay(500);
    cfg.sd_active = false;
    kit.begin(cfg);
    delay(500);
    SPI.end();
    delay(500);
    
    // Cross-mode test summary
    Logger.println();
    Logger.println("════════════════════════════════════════════");
    Logger.println("💡 ANALYSIS & RECOMMENDATIONS");
    Logger.println("════════════════════════════════════════════");
    Logger.println();
    Logger.println("Pin Information:");
    Logger.println("   SD_MMC hardware pins (ESP32 default):");
    Logger.println("      CLK=GPIO14, CMD=GPIO15, D0=GPIO2");
    Logger.println("      D1=GPIO4, D2=GPIO12, D3=GPIO13 (4-bit mode)");
    Logger.println("   SPI software pins (configurable):");
    Logger.printf("      Current config.h: CS=%d CLK=%d MOSI=%d MISO=%d\n",
                 SD_CS_PIN, SD_CLK_PIN, SD_MOSI_PIN, SD_MISO_PIN);
    Logger.println();
    Logger.println("DIP Switch Requirements:");
    Logger.println("   SD_MMC mode: Check ESP32-A1S schematic for switches");
    Logger.println("   SPI mode:    DIP switches 2,3,4 UP, 5 DOWN (typical)");
    Logger.println("   ⚠️  Wrong DIP switches = card not detected");
    Logger.println();
    Logger.println("Card Type Values:");
    Logger.println("   0 = No card detected (or wrong DIP/pins/power)");
    Logger.println("   1 = MMC (rare)");
    Logger.println("   2 = SD");
    Logger.println("   3 = SDHC (most common for >2GB cards)");
    Logger.println();
    Logger.println("Troubleshooting Steps:");
    Logger.println("   1. Verify card is properly seated in slot");
    Logger.println("   2. Check DIP switch settings match chosen mode");
    Logger.println("   3. Measure 3.3V on card socket (power issue?)");
    Logger.println("   4. Try different SD card (some are picky)");
    Logger.println("   5. Reformat card as FAT32 on computer");
    Logger.println("   6. Check board schematic for actual pin connections");
    Logger.println();
    if (spiSuccess) {
        Logger.println("✅ SPI mode working - update config.h with working pins!");
    } else {
        Logger.println("❌ No working configuration found");
        Logger.println("   Next steps:");
        Logger.println("   - Double-check DIP switches");
        Logger.println("   - Consult ESP32-A1S AudioKit schematic");
        Logger.println("   - Try different SD card");
        Logger.println("   - Check for cold solder joints on SD socket");
    }
    Logger.println();
    Logger.printf("Current Build: SD_USE_MMC=%d %s\n", 
                  SD_USE_MMC, 
                  SD_USE_MMC ? "(Using SD_MMC in production)" : "(Using SPI in production)");
    Logger.println();
    Logger.println("Build Flags:");
    Logger.println("   -DRUN_SD_DEBUG_FIRST  Run this test before all init");
    Logger.println("   -DSD_USE_MMC=1        Force SD_MMC mode");
    Logger.println("   -DSD_USE_MMC=0        Force SPI mode");
    Logger.println("════════════════════════════════════════════");
    Logger.println();
    Logger.println("⚠️  Reboot required to restore normal SD operation");
}
