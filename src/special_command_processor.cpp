#include "special_command_processor.h"
#include "logging.h"
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
#include "AudioTools/AudioLibs/AudioRealFFT.h"
#include "AudioTools/CoreAudio/StreamCopy.h"
#include "AudioTools/CoreAudio/GoerzelStream.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "dtmf_goertzel.h"

// Forward declarations for FFT debug mode functions (from dtmf_decoder.cpp)
bool isFFTDebugEnabled();
void setFFTDebugEnabled(bool enabled);

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
#endif