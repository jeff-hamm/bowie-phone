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
#include "AudioTools/AudioLibs/AudioRealFFT.h"
#include "AudioTools/CoreAudio/StreamCopy.h"
#include "AudioTools/CoreAudio/GoerzelStream.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "dtmf_goertzel.h"
#include "esp_heap_caps.h"
#include "config.h"

// Forward declarations for FFT debug mode functions (from dtmf_decoder.cpp)
bool isFFTDebugEnabled();
void setFFTDebugEnabled(bool enabled);

// Forward declaration for audio capture
#ifdef DEBUG
void performAudioCapture(int durationSec);
void performSDCardDebug();
#endif

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

#ifdef DEBUG
// Serial debug mode buffer
static char debugInputBuffer[64];
static int debugInputPos = 0;

// Forward declaration
void processDebugCommand(const String& cmd);

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
    else if (cmd.equalsIgnoreCase("cpuload") || cmd.equalsIgnoreCase("perftest")) {
        // CPU load test - FFT-based DTMF detection
        performFFTCPULoadTest();
    }
    else if (cmd.equalsIgnoreCase("cpuload-goertzel") || cmd.equalsIgnoreCase("perftest-goertzel")) {
        // CPU load test - Goertzel-based DTMF detection
        performGoertzelCPULoadTest();
    }
    else if (cmd.equalsIgnoreCase("help") || cmd.equals("?")) {

        Logger.println("🔧 [DEBUG] Serial/Telnet Commands:");
        Logger.println("   hook          - Toggle hook state");
        Logger.println("   hook auto     - Reset to automatic hook detection");
        Logger.println("   cpuload       - Test CPU load (FFT DTMF + audio)");
        Logger.println("   cpuload-goertzel - Test CPU load (Goertzel DTMF + audio)");
        Logger.println("   level <0-2>   - Set log level (0=quiet, 1=normal, 2=debug)");
        Logger.println("   state         - Show current state");
        Logger.println("   debugaudio [s] - Capture raw audio (1-10s, default 2)");
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
        // Parse optional duration: "debugaudio 3" = 3 seconds
        int durationSec = 20; // default
        int spaceIdx = cmd.indexOf(' ');
        if (spaceIdx > 0) {
            int val = cmd.substring(spaceIdx + 1).toInt();
            if (val >= 1 && val <= 20) durationSec = val;
        }
        performAudioCapture(durationSec);
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
#endif

// ============================================================================
// SPECIAL COMMANDS - CONFIGURABLE STORAGE
// ============================================================================

// EEPROM storage configuration
#define EEPROM_SIZE 1024
#define EEPROM_MAGIC 0xB0E1  // Magic number to verify EEPROM data
#define EEPROM_VERSION 1
#define PREFERENCES_NAMESPACE "bowiephone"

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

// Preferences object for ESP32 NVS storage
static Preferences preferences;

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

#ifdef DEBUG
void performFFTCPULoadTest()
{
    // CPU load test - FFT-based DTMF detection during audio playback
    extern AudioRealFFT fft;
    extern StreamCopy copier;
    extern AudioBoardStream kit;
    
    Logger.println("🔬 CPU Load Test: FFT-based DTMF + Audio");
    Logger.println("============================================");
    Logger.println("Starting dial tone playback...");
    
    getExtendedAudioPlayer().playAudioKey("dialtone");
    delay(100);  // Let audio start
    
    if (!getExtendedAudioPlayer().isActive()) {
        Logger.println("❌ Failed to start audio - test aborted");
        return;
    }
    
    const int TEST_DURATION_MS = 5000;
    const int SAMPLE_INTERVAL_MS = 100;
    
    unsigned long testStart = millis();
    unsigned long loopCount = 0;
    unsigned long copyCount = 0;
    unsigned long maxLoopTime = 0;
    unsigned long minLoopTime = ULONG_MAX;
    unsigned long totalLoopTime = 0;
    unsigned long audioUnderrunCount = 0;
    unsigned long lastSample = millis();
    
    // I2S buffer tracking
    int minBufferAvailable = INT_MAX;
    int maxBufferAvailable = 0;
    unsigned long bufferEmptyCount = 0;
    unsigned long bufferSamples = 0;
    long totalBufferAvailable = 0;
    
    Logger.printf("Running for %d seconds...\n", TEST_DURATION_MS / 1000);
    
    while (millis() - testStart < TEST_DURATION_MS) {
        unsigned long loopStart = micros();
        
        // Process audio (what normally happens in loop)
        if (getExtendedAudioPlayer().isActive()) {
            getExtendedAudioPlayer().copy();
        } else {
            audioUnderrunCount++;
            // Restart dial tone if it stopped unexpectedly
            getExtendedAudioPlayer().playAudioKey("dialtone");
        }
        
        // Sample I2S output buffer status
        int bufAvail = kit.availableForWrite();
        if (bufAvail < minBufferAvailable) minBufferAvailable = bufAvail;
        if (bufAvail > maxBufferAvailable) maxBufferAvailable = bufAvail;
        totalBufferAvailable += bufAvail;
        bufferSamples++;
        if (bufAvail > (kit.defaultConfig().buffer_size * 0.9)) {
            bufferEmptyCount++;
        }
        
        // Process FFT DTMF detection
        size_t copied = copier.copy();
        if (copied > 0) {
            copyCount++;
        }
        
        // Check for DTMF sequences (but don't act on them)
        readDTMFSequence();
        
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
    
    // Stop audio
    getExtendedAudioPlayer().stop();
    
    // Calculate statistics
    unsigned long avgLoopTime = loopCount > 0 ? totalLoopTime / loopCount : 0;
    float loopsPerSecond = loopCount * 1000.0f / TEST_DURATION_MS;
    float copiesPerSecond = copyCount * 1000.0f / TEST_DURATION_MS;
    int avgBufferAvailable = bufferSamples > 0 ? (int)(totalBufferAvailable / bufferSamples) : 0;
    float expectedFFTRate = 44100.0f / 1024.0f;  // Based on stride
    
    Logger.println();
    Logger.println("============================================");
    Logger.println("📊 Results:");
    Logger.printf("   Test duration: %lu ms\n", TEST_DURATION_MS);
    Logger.printf("   Total loops: %lu (%.1f/sec)\n", loopCount, loopsPerSecond);
    Logger.printf("   FFT copier calls with data: %lu (%.1f/sec)\n", copyCount, copiesPerSecond);
    Logger.printf("   Expected FFT rate: %.1f frames/sec\n", expectedFFTRate);
    Logger.println();
    Logger.println("⏱️ Loop Timing (microseconds):");
    Logger.printf("   Min: %lu µs\n", minLoopTime);
    Logger.printf("   Max: %lu µs\n", maxLoopTime);
    Logger.printf("   Avg: %lu µs\n", avgLoopTime);
    Logger.println();
    Logger.println("🔊 I2S Output Buffer (availableForWrite):");
    Logger.printf("   Min available: %d bytes\n", minBufferAvailable);
    Logger.printf("   Max available: %d bytes\n", maxBufferAvailable);
    Logger.printf("   Avg available: %d bytes\n", avgBufferAvailable);
    Logger.printf("   Empty count: %lu / %lu samples (%.1f%%)\n", 
                  bufferEmptyCount, bufferSamples,
                  bufferSamples > 0 ? (bufferEmptyCount * 100.0f / bufferSamples) : 0.0f);
    Logger.println("   (Low values = buffer full = good)");
    Logger.println("   (High values = buffer empty = starving)");
    Logger.println();
    Logger.printf("⚠️ Audio restarts (underruns): %lu\n", audioUnderrunCount);
    Logger.println();
    
    // Assessment
    Logger.println("📋 Assessment:");
    if (maxLoopTime > 50000) {
        Logger.println("   ❌ FAIL: Max loop time > 50ms - will cause audio glitches");
    } else if (maxLoopTime > 23000) {
        Logger.println("   ⚠️ WARN: Max loop time > 23ms - may miss DTMF tones");
    } else {
        Logger.println("   ✅ PASS: Loop timing acceptable");
    }
    
    if (audioUnderrunCount > 0) {
        Logger.println("   ❌ FAIL: Audio underruns detected");
    } else {
        Logger.println("   ✅ PASS: No audio underruns");
    }
    
    float emptyPercent = bufferSamples > 0 ? (bufferEmptyCount * 100.0f / bufferSamples) : 0.0f;
    if (emptyPercent > 10.0f) {
        Logger.println("   ❌ FAIL: I2S buffer frequently starved - audio will stutter");
    } else if (emptyPercent > 1.0f) {
        Logger.println("   ⚠️ WARN: I2S buffer occasionally starved");
    } else {
        Logger.println("   ✅ PASS: I2S buffer staying saturated");
    }
    
    if (copiesPerSecond < expectedFFTRate * 0.8f) {
        Logger.println("   ⚠️ WARN: FFT processing may be falling behind");
    } else {
        Logger.println("   ✅ PASS: FFT processing keeping up");
    }
    
    Logger.printf("\n💾 Free heap: %u bytes\n", ESP.getFreeHeap());
    Logger.println("============================================");
}

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
    
    // I2S buffer tracking
    int minBufferAvailable = INT_MAX;
    int maxBufferAvailable = 0;
    unsigned long bufferEmptyCount = 0;
    unsigned long bufferSamples = 0;
    long totalBufferAvailable = 0;
    
    Logger.printf("Running for %d seconds (Goertzel on core 0)...\\n", TEST_DURATION_MS / 1000);
    
    while (millis() - testStart < TEST_DURATION_MS) {
        unsigned long loopStart = micros();
        
        // Process audio output ONLY (Goertzel runs in separate task)
        if (getExtendedAudioPlayer().isActive()) {
            getExtendedAudioPlayer().copy();
        } else {
            audioUnderrunCount++;
            // Restart dial tone if it stopped unexpectedly
            getExtendedAudioPlayer().playAudioKey("dialtone");
        }
        
        // Sample I2S output buffer status
        int bufAvail = kit.availableForWrite();
        if (bufAvail < minBufferAvailable) minBufferAvailable = bufAvail;
        if (bufAvail > maxBufferAvailable) maxBufferAvailable = bufAvail;
        totalBufferAvailable += bufAvail;
        bufferSamples++;
        if (bufAvail > (kit.defaultConfig().buffer_size * 0.9)) {
            bufferEmptyCount++;
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
    int avgBufferAvailable = bufferSamples > 0 ? (int)(totalBufferAvailable / bufferSamples) : 0;
    float expectedGoertzelRate = 44100.0f / 512.0f;  // Based on block_size=512
    
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
    Logger.println("🔊 I2S Output Buffer (availableForWrite):");
    Logger.printf("   Min available: %d bytes\n", minBufferAvailable);
    Logger.printf("   Max available: %d bytes\n", maxBufferAvailable);
    Logger.printf("   Avg available: %d bytes\n", avgBufferAvailable);
    Logger.printf("   Empty count: %lu / %lu samples (%.1f%%)\n", 
                  bufferEmptyCount, bufferSamples,
                  bufferSamples > 0 ? (bufferEmptyCount * 100.0f / bufferSamples) : 0.0f);
    Logger.println("   (Low values = buffer full = good)");
    Logger.println("   (High values = buffer empty = starving)");
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
    
    float emptyPercent = bufferSamples > 0 ? (bufferEmptyCount * 100.0f / bufferSamples) : 0.0f;
    if (emptyPercent > 10.0f) {
        Logger.println("   ❌ FAIL: I2S buffer frequently starved - audio will stutter");
    } else if (emptyPercent > 1.0f) {
        Logger.println("   ⚠️ WARN: I2S buffer occasionally starved");
    } else {
        Logger.println("   ✅ PASS: I2S buffer staying saturated");
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

#endif