#include "commands_internal.h"

// ============================================================================
// EEPROM / NVS STORAGE — LOCAL CONSTANTS AND STRUCTS
// ============================================================================

#define EEPROM_SIZE    1024
#define EEPROM_MAGIC   0xB0E1   // Magic number to verify NVS data
#define EEPROM_VERSION 1

struct EEPROMCommandData {
    char sequence[16];
    char description[32];
    bool isActive;
};

struct EEPROMHeader {
    uint16_t magic;
    uint8_t  version;
    uint8_t  commandCount;
    uint32_t checksum;
};

// ============================================================================
// MODULE-PRIVATE STATE
// ============================================================================

static SpecialCommand specialCommands[MAX_SPECIAL_COMMANDS];
static int specialCommandCount = 0;

static Preferences preferences;

// Default special commands (can be overridden via build flags)
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
    {CLEAR_CACHE_SEQUENCE, "Clear Cache & Reboot"},
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
// EEPROM / NVS HELPERS (private)
// ============================================================================

static uint32_t calculateChecksum(const EEPROMCommandData* data, int count) {
    uint32_t checksum = 0;
    const uint8_t* bytes = (const uint8_t*)data;
    for (int i = 0; i < count * (int)sizeof(EEPROMCommandData); i++) {
        checksum += bytes[i];
        checksum = (checksum << 1) | (checksum >> 31);
    }
    return checksum;
}

// ============================================================================
// SPECIAL COMMAND MANAGEMENT
// ============================================================================

void initializeSpecialCommands() {
    Logger.printf("🔧 Initializing special commands system...\n");

    initAudioCaptureState();

    if (loadSpecialCommandsFromEEPROM()) {
        Logger.printf("📥 Using commands from EEPROM storage\n");
        return;
    }

    Logger.printf("🔄 Initializing with default commands\n");
    clearSpecialCommands();

    int defaultCount = sizeof(DEFAULT_SPECIAL_COMMANDS) / sizeof(DEFAULT_SPECIAL_COMMANDS[0]);
    for (int i = 0; i < defaultCount && i < MAX_SPECIAL_COMMANDS; i++) {
        specialCommands[i] = DEFAULT_SPECIAL_COMMANDS[i];
        assignDefaultHandler(i, DEFAULT_SPECIAL_COMMANDS[i].sequence);
        specialCommandCount++;
    }

    Logger.printf("✅ Initialized %d default special commands\n", specialCommandCount);
}

bool addSpecialCommand(const char* sequence, const char* description, void (*handler)(void)) {
    if (specialCommandCount >= MAX_SPECIAL_COMMANDS) {
        Logger.printf("Error: Special command table is full\n");
        return false;
    }

    char* seqCopy  = (char*)malloc(strlen(sequence)    + 1);
    char* descCopy = (char*)malloc(strlen(description) + 1);

    if (!seqCopy || !descCopy) {
        Logger.printf("❌ Memory allocation failed for command\n");
        if (seqCopy)  free(seqCopy);
        if (descCopy) free(descCopy);
        return false;
    }

    strcpy(seqCopy,  sequence);
    strcpy(descCopy, description);

    specialCommands[specialCommandCount].sequence    = seqCopy;
    specialCommands[specialCommandCount].description = descCopy;
    specialCommands[specialCommandCount].handler     = handler;
    specialCommandCount++;

    Logger.printf("✅ Added special command: %s - %s\n", sequence, description);
    saveSpecialCommandsToEEPROM();
    return true;
}

int getSpecialCommandCount() {
    return specialCommandCount;
}

void clearSpecialCommands() {
    for (int i = 0; i < specialCommandCount; i++) {
        if (specialCommands[i].sequence)    free((void*)specialCommands[i].sequence);
        if (specialCommands[i].description) free((void*)specialCommands[i].description);
    }
    specialCommandCount = 0;
    memset(specialCommands, 0, sizeof(specialCommands));
}

// ============================================================================
// EEPROM PERSISTENCE
// ============================================================================

void saveSpecialCommandsToEEPROM() {
    Logger.printf("💾 Saving special commands to EEPROM...\n");

    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Logger.printf("❌ Failed to initialize preferences\n");
        return;
    }

    EEPROMCommandData eepromData[MAX_SPECIAL_COMMANDS];
    EEPROMHeader header;

    header.magic   = EEPROM_MAGIC;
    header.version = EEPROM_VERSION;

    int validCommands = 0;
    for (int i = 0; i < specialCommandCount; i++) {
        if (specialCommands[i].sequence != nullptr &&
            strlen(specialCommands[i].sequence) < sizeof(eepromData[i].sequence)) {
            strncpy(eepromData[validCommands].sequence,
                    specialCommands[i].sequence,
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
    header.checksum     = calculateChecksum(eepromData, validCommands);

    preferences.putBytes("header",   &header,   sizeof(header));
    preferences.putBytes("commands", eepromData, validCommands * sizeof(EEPROMCommandData));
    preferences.end();

    Logger.printf("✅ Saved %d commands to EEPROM\n", validCommands);
}

bool loadSpecialCommandsFromEEPROM() {
    Logger.printf("📖 Loading special commands from EEPROM...\n");

    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
        Logger.printf("❌ Failed to initialize preferences for reading\n");
        return false;
    }

    EEPROMHeader header;
    size_t headerSize = preferences.getBytes("header", &header, sizeof(header));

    if (headerSize != sizeof(header)) {
        Logger.printf("📄 No valid EEPROM data found, using defaults\n");
        preferences.end();
        return false;
    }

    if (header.magic != EEPROM_MAGIC) {
        Logger.printf("❌ Invalid EEPROM magic number: 0x%04X (expected 0x%04X)\n",
                      header.magic, EEPROM_MAGIC);
        preferences.end();
        return false;
    }

    if (header.version != EEPROM_VERSION) {
        Logger.printf("⚠️  EEPROM version mismatch: %d (expected %d)\n",
                      header.version, EEPROM_VERSION);
        preferences.end();
        return false;
    }

    if (header.commandCount > MAX_SPECIAL_COMMANDS) {
        Logger.printf("❌ Too many commands in EEPROM: %d (max %d)\n",
                      header.commandCount, MAX_SPECIAL_COMMANDS);
        preferences.end();
        return false;
    }

    EEPROMCommandData eepromData[MAX_SPECIAL_COMMANDS];
    size_t dataSize = preferences.getBytes("commands", eepromData,
                                           header.commandCount * sizeof(EEPROMCommandData));
    preferences.end();

    if (dataSize != header.commandCount * sizeof(EEPROMCommandData)) {
        Logger.printf("❌ EEPROM data size mismatch\n");
        return false;
    }

    uint32_t calculatedChecksum = calculateChecksum(eepromData, header.commandCount);
    if (calculatedChecksum != header.checksum) {
        Logger.printf("❌ EEPROM checksum mismatch: 0x%08X vs 0x%08X\n",
                      calculatedChecksum, header.checksum);
        return false;
    }

    clearSpecialCommands();

    for (int i = 0; i < header.commandCount; i++) {
        if (eepromData[i].isActive) {
            char* seqCopy  = (char*)malloc(strlen(eepromData[i].sequence)    + 1);
            char* descCopy = (char*)malloc(strlen(eepromData[i].description) + 1);

            if (seqCopy && descCopy) {
                strcpy(seqCopy,  eepromData[i].sequence);
                strcpy(descCopy, eepromData[i].description);

                specialCommands[specialCommandCount].sequence    = seqCopy;
                specialCommands[specialCommandCount].description = descCopy;
                assignDefaultHandler(specialCommandCount, seqCopy);
                specialCommandCount++;

                Logger.printf("📥 Loaded command: %s - %s\n", seqCopy, descCopy);
            } else {
                Logger.printf("❌ Memory allocation failed for command %d\n", i);
                if (seqCopy)  free(seqCopy);
                if (descCopy) free(descCopy);
            }
        }
    }

    Logger.printf("✅ Loaded %d commands from EEPROM\n", specialCommandCount);
    return true;
}

void assignDefaultHandler(int index, const char* sequence) {
    if      (strcmp(sequence, "*123#")      == 0) specialCommands[index].handler = executeSystemStatus;
    else if (strcmp(sequence, "*789#")      == 0) specialCommands[index].handler = executeReboot;
    else if (strcmp(sequence, "*000#")      == 0) specialCommands[index].handler = executeFactoryReset;
    else if (strcmp(sequence, "*#00#")      == 0) specialCommands[index].handler = executeListCommands;
    else if (strcmp(sequence, "*#06#")      == 0) specialCommands[index].handler = executeDeviceInfo;
    else if (strcmp(sequence, CLEAR_CACHE_SEQUENCE)      == 0) specialCommands[index].handler = executeRefreshAudio;
    else if (strcmp(sequence, "*#08#")      == 0) specialCommands[index].handler = executePrepareOTA;
    else if (strcmp(sequence, "*#09#")      == 0) specialCommands[index].handler = executePhoneHome;
    else if (strcmp(sequence, "*#88#")      == 0) specialCommands[index].handler = executeTailscaleStatus;
    else if (strcmp(sequence, "*#01#")      == 0) specialCommands[index].handler = executeSaveEEPROM;
    else if (strcmp(sequence, "*#02#")      == 0) specialCommands[index].handler = executeLoadEEPROM;
    else if (strcmp(sequence, "*#99#")      == 0) specialCommands[index].handler = executeEraseEEPROM;
    else                                          specialCommands[index].handler = nullptr;
}

void eraseSpecialCommandsFromEEPROM() {
    Logger.printf("🗑️  Erasing special commands from EEPROM...\n");

    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Logger.printf("❌ Failed to initialize preferences for clearing\n");
        return;
    }
    preferences.clear();
    preferences.end();

    Logger.printf("✅ EEPROM data cleared\n");
}

// ============================================================================
// COMMAND LOOKUP AND DISPATCH
// ============================================================================

bool isSpecialCommand(const char* sequence) {
    for (int i = 0; i < specialCommandCount; i++) {
        if (strcmp(sequence, specialCommands[i].sequence) == 0) return true;
    }
    return false;
}

void processSpecialCommand(const char* sequence) {
    Logger.printf("⚙️  SPECIAL COMMAND DETECTED: %s\n", sequence);

    for (int i = 0; i < specialCommandCount; i++) {
        if (strcmp(sequence, specialCommands[i].sequence) == 0) {
            Logger.printf("🔧 Command: %s\n", specialCommands[i].description);
            if (specialCommands[i].handler != nullptr) {
                specialCommands[i].handler();
            } else {
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

void executeSystemStatus() {
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

void executeReboot() {
    Logger.printf("🔄 Rebooting device in 2 seconds...\n");
    getExtendedAudioPlayer().stop();
    delay(2000);
    ESP.restart();
}

void executeFactoryReset() {
    Logger.printf("⚠️  FACTORY RESET initiated!\n");
    Logger.printf("🗑️  Clearing all settings...\n");
    eraseSpecialCommandsFromEEPROM();
    Logger.printf("🔄 Restarting...\n");
    delay(2000);
    ESP.restart();
}

void executeDeviceInfo() {
    Logger.printf("📱 Device Information:\n");
    Logger.printf("   MAC: %s\n", WiFi.macAddress().c_str());
    Logger.printf("   Chip Model: %s\n", ESP.getChipModel());
    Logger.printf("   Chip Revision: %d\n", ESP.getChipRevision());
    Logger.printf("   Flash Size: %d KB\n", ESP.getFlashChipSize() / 1024);
    Logger.printf("   Free Heap: %d bytes\n", ESP.getFreeHeap());
}


void executeRefreshAudio() {
    Logger.printf("🔄 Refreshing audio catalog...\n");
    invalidateAudioCache();
    if (downloadAudio()) {
        Logger.printf("✅ Audio catalog refreshed successfully\n");
        getAudioKeyRegistry().listKeys();
    } else {
        Logger.printf("❌ Audio catalog refresh failed\n");
    }
}

void executePrepareOTA() {
    Logger.printf("🔄 Preparing for OTA update...\n");

    getExtendedAudioPlayer().stop();
    delay(100);

    SD.end();
    delay(100);

    setOtaPrepareTimeout();

    Logger.printf("✅ Ready for OTA - will reboot in 5 min if no OTA received\n");
    Logger.printf("   Use 'pullota <url>' via serial/telnet to start update\n");
}

void executePhoneHome() {
    Logger.printf("📞 Manual phone home check-in...\n");
    if (checkForRemoteUpdates(nullptr)) {
        Logger.printf("✅ Phone home triggered OTA update\n");
    } else {
        Logger.printf("📞 Phone home status: %s\n", getPhoneHomeStatus());
    }
}

void executeListCommands() {
    Logger.printf("📋 Special Commands List:\n");
    Logger.printf("   Total commands: %d / %d\n", specialCommandCount, MAX_SPECIAL_COMMANDS);

    for (int i = 0; i < specialCommandCount; i++) {
        Logger.printf("   %d: %s - %s %s\n",
                      i + 1,
                      specialCommands[i].sequence,
                      specialCommands[i].description,
                      specialCommands[i].handler ? "(active)" : "(custom)");
    }

    if (specialCommandCount == 0) {
        Logger.printf("   No commands configured\n");
    }
}

void executeSaveEEPROM() {
    Logger.printf("💾 Manual EEPROM Save Command\n");
    saveSpecialCommandsToEEPROM();
}

void executeLoadEEPROM() {
    Logger.printf("📥 Manual EEPROM Load Command\n");
    if (loadSpecialCommandsFromEEPROM()) {
        Logger.printf("✅ Commands reloaded from EEPROM\n");
    } else {
        Logger.printf("❌ Failed to load from EEPROM, keeping current commands\n");
    }
}

void executeEraseEEPROM() {
    Logger.printf("🗑️  Manual EEPROM Erase Command\n");
    eraseSpecialCommandsFromEEPROM();
    Logger.printf("🔄 Reinitializing with defaults...\n");
    initializeSpecialCommands();
}

void executeTailscaleStatus() {
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
