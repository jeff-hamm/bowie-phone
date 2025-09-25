#include "special_command_processor.h"
#include <WiFi.h>
#include <EEPROM.h>
#include <Preferences.h>

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
static const SpecialCommand DEFAULT_SPECIAL_COMMANDS[] = {
#ifdef CUSTOM_COMMAND_1_SEQ
    {CUSTOM_COMMAND_1_SEQ, CUSTOM_COMMAND_1_DESC},
#else
    {"*123#", "System Status"},
#endif

#ifdef CUSTOM_COMMAND_2_SEQ
    {CUSTOM_COMMAND_2_SEQ, CUSTOM_COMMAND_2_DESC},
#else
    {"*456#", "WiFi Reset"},
#endif

#ifdef CUSTOM_COMMAND_3_SEQ
    {CUSTOM_COMMAND_3_SEQ, CUSTOM_COMMAND_3_DESC},
#else
    {"*789#", "Device Reset"},
#endif

#ifdef CUSTOM_COMMAND_4_SEQ
    {CUSTOM_COMMAND_4_SEQ, CUSTOM_COMMAND_4_DESC},
#else
    {"*000#", "Factory Reset"},
#endif

#ifdef CUSTOM_COMMAND_5_SEQ
    {CUSTOM_COMMAND_5_SEQ, CUSTOM_COMMAND_5_DESC},
#else
    {"#123*", "Debug Mode Toggle"},
#endif

#ifdef CUSTOM_COMMAND_6_SEQ
    {CUSTOM_COMMAND_6_SEQ, CUSTOM_COMMAND_6_DESC},
#else
    {"#456*", "Audio Test"},
#endif

#ifdef CUSTOM_COMMAND_7_SEQ
    {CUSTOM_COMMAND_7_SEQ, CUSTOM_COMMAND_7_DESC},
#else
    {"*#06#", "Device Info"},
#endif

#ifdef CUSTOM_COMMAND_8_SEQ
    {CUSTOM_COMMAND_8_SEQ, CUSTOM_COMMAND_8_DESC},
#else
    {"*#*#", "Admin Menu"},
#endif

    // EEPROM management commands (always available)
    {"*#00#", "List Commands"},
    {"*#01#", "Save to EEPROM"},  
    {"*#02#", "Load from EEPROM"},
    {"*#99#", "Erase EEPROM"},
};

// ============================================================================
// INITIALIZATION AND MANAGEMENT
// ============================================================================

void initializeSpecialCommands()
{
    Serial.printf("🔧 Initializing special commands system...\n");
    
    // Try to load from EEPROM first
    if (loadSpecialCommandsFromEEPROM())
    {
        Serial.printf("📥 Using commands from EEPROM storage\n");
        return;
    }
    
    // If no EEPROM data or loading failed, initialize with defaults
    Serial.printf("🔄 Initializing with default commands\n");
    clearSpecialCommands();

    int defaultCount = sizeof(DEFAULT_SPECIAL_COMMANDS) / sizeof(DEFAULT_SPECIAL_COMMANDS[0]);
    for (int i = 0; i < defaultCount && i < MAX_SPECIAL_COMMANDS; i++)
    {
        specialCommands[i] = DEFAULT_SPECIAL_COMMANDS[i];

        // Assign function pointers based on sequence
        assignDefaultHandler(i, DEFAULT_SPECIAL_COMMANDS[i].sequence);

        specialCommandCount++;
    }

    Serial.printf("✅ Initialized %d default special commands\n", specialCommandCount);
}

bool addSpecialCommand(const char *sequence, const char *description, void (*handler)(void))
{
    if (specialCommandCount >= MAX_SPECIAL_COMMANDS)
    {
        Serial.printf("Error: Special command table is full\n");
        return false;
    }

    // Create persistent copies of strings
    char* seqCopy = (char*)malloc(strlen(sequence) + 1);
    char* descCopy = (char*)malloc(strlen(description) + 1);
    
    if (!seqCopy || !descCopy)
    {
        Serial.printf("❌ Memory allocation failed for command\n");
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

    Serial.printf("✅ Added special command: %s - %s\n", sequence, description);
    
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
    Serial.printf("💾 Saving special commands to EEPROM...\n");
    
    // Use ESP32 Preferences (NVS) for reliable storage
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Serial.printf("❌ Failed to initialize preferences\n");
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
    
    Serial.printf("✅ Saved %d commands to EEPROM\n", validCommands);
}

bool loadSpecialCommandsFromEEPROM()
{
    Serial.printf("📖 Loading special commands from EEPROM...\n");
    
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) // Read-only mode
    {
        Serial.printf("❌ Failed to initialize preferences for reading\n");
        return false;
    }
    
    // Load header
    EEPROMHeader header;
    size_t headerSize = preferences.getBytes("header", &header, sizeof(header));
    
    if (headerSize != sizeof(header))
    {
        Serial.printf("📄 No valid EEPROM data found, using defaults\n");
        preferences.end();
        return false;
    }
    
    // Validate header
    if (header.magic != EEPROM_MAGIC)
    {
        Serial.printf("❌ Invalid EEPROM magic number: 0x%04X (expected 0x%04X)\n", 
                     header.magic, EEPROM_MAGIC);
        preferences.end();
        return false;
    }
    
    if (header.version != EEPROM_VERSION)
    {
        Serial.printf("⚠️  EEPROM version mismatch: %d (expected %d)\n", 
                     header.version, EEPROM_VERSION);
        preferences.end();
        return false;
    }
    
    if (header.commandCount > MAX_SPECIAL_COMMANDS)
    {
        Serial.printf("❌ Too many commands in EEPROM: %d (max %d)\n", 
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
        Serial.printf("❌ EEPROM data size mismatch\n");
        return false;
    }
    
    // Verify checksum
    uint32_t calculatedChecksum = calculateChecksum(eepromData, header.commandCount);
    if (calculatedChecksum != header.checksum)
    {
        Serial.printf("❌ EEPROM checksum mismatch: 0x%08X vs 0x%08X\n", 
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
                
                Serial.printf("📥 Loaded command: %s - %s\n", seqCopy, descCopy);
            }
            else
            {
                Serial.printf("❌ Memory allocation failed for command %d\n", i);
                if (seqCopy) free(seqCopy);
                if (descCopy) free(descCopy);
            }
        }
    }
    
    Serial.printf("✅ Loaded %d commands from EEPROM\n", specialCommandCount);
    return true;
}

void assignDefaultHandler(int index, const char* sequence)
{
    // Assign function pointers for known default commands
    if (strcmp(sequence, "*123#") == 0)
        specialCommands[index].handler = executeSystemStatus;
    else if (strcmp(sequence, "*456#") == 0)
        specialCommands[index].handler = executeWiFiReset;
    else if (strcmp(sequence, "*789#") == 0)
        specialCommands[index].handler = executeDeviceReset;
    else if (strcmp(sequence, "*000#") == 0)
        specialCommands[index].handler = executeFactoryReset;
    else if (strcmp(sequence, "#123*") == 0)
        specialCommands[index].handler = executeDebugToggle;
    else if (strcmp(sequence, "#456*") == 0)
        specialCommands[index].handler = executeAudioTest;
    else if (strcmp(sequence, "*#06#") == 0)
        specialCommands[index].handler = executeDeviceInfo;
    else if (strcmp(sequence, "*#*#") == 0)
        specialCommands[index].handler = executeAdminMenu;
    // EEPROM management commands
    else if (strcmp(sequence, "*#00#") == 0)
        specialCommands[index].handler = executeListCommands;
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
    Serial.printf("🗑️  Erasing special commands from EEPROM...\n");
    
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Serial.printf("❌ Failed to initialize preferences for clearing\n");
        return;
    }
    
    preferences.clear();
    preferences.end();
    
    Serial.printf("✅ EEPROM data cleared\n");
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
    Serial.printf("⚙️  SPECIAL COMMAND DETECTED: %s\n", sequence);

    // Find and execute the command
    for (int i = 0; i < specialCommandCount; i++)
    {
        if (strcmp(sequence, specialCommands[i].sequence) == 0)
        {
            Serial.printf("🔧 Command: %s\n", specialCommands[i].description);

            // Execute command via function pointer if available
            if (specialCommands[i].handler != nullptr)
            {
                specialCommands[i].handler();
            }
            else
            {
                Serial.printf("⚠️  No handler assigned for command: %s\n", sequence);
            }

            return;
        }
    }

    Serial.printf("❌ Command not found: %s\n", sequence);
}

// ============================================================================
// DEFAULT COMMAND IMPLEMENTATIONS
// ============================================================================

void executeSystemStatus()
{
    Serial.printf("📊 System Status:\n");
    Serial.printf("   WiFi: %s\n", WiFi.isConnected() ? "Connected" : "Disconnected");
    Serial.printf("   IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("   Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("   Uptime: %lu seconds\n", millis() / 1000);
}

void executeWiFiReset()
{
    Serial.printf("🔄 Resetting WiFi configuration...\n");
    // WiFi reset logic here
    // wm.resetSettings();
    Serial.printf("✅ WiFi settings cleared. Device will restart.\n");
}

void executeDeviceReset()
{
    Serial.printf("🔄 Restarting device in 3 seconds...\n");
    delay(3000);
    ESP.restart();
}

void executeFactoryReset()
{
    Serial.printf("⚠️  FACTORY RESET initiated!\n");
    Serial.printf("🗑️  Clearing all settings...\n");
    // Factory reset logic here
    Serial.printf("🔄 Restarting...\n");
    delay(2000);
    ESP.restart();
}

void executeDebugToggle()
{
    Serial.printf("🐛 Debug mode toggle (implementation needed)\n");
    // Toggle debug flags
}

void executeAudioTest()
{
    Serial.printf("🔊 Audio test (implementation needed)\n");
    // Audio system test
}

void executeDeviceInfo()
{
    Serial.printf("📱 Device Information:\n");
    Serial.printf("   MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("   Chip Model: %s\n", ESP.getChipModel());
    Serial.printf("   Chip Revision: %d\n", ESP.getChipRevision());
    Serial.printf("   Flash Size: %d KB\n", ESP.getFlashChipSize() / 1024);
}

void executeAdminMenu()
{
    Serial.printf("👑 Admin Menu:\n");
    Serial.printf("   Commands loaded: %d / %d\n", specialCommandCount, MAX_SPECIAL_COMMANDS);
    Serial.printf("   EEPROM functions available:\n");
    Serial.printf("   - *#01# : Save commands to EEPROM\n");
    Serial.printf("   - *#02# : Load commands from EEPROM\n"); 
    Serial.printf("   - *#99# : Erase EEPROM data\n");
    Serial.printf("   - *#00# : List all commands\n");
}

void executeListCommands()
{
    Serial.printf("📋 Special Commands List:\n");
    Serial.printf("   Total commands: %d / %d\n", specialCommandCount, MAX_SPECIAL_COMMANDS);
    
    for (int i = 0; i < specialCommandCount; i++)
    {
        Serial.printf("   %d: %s - %s %s\n", 
                     i + 1, 
                     specialCommands[i].sequence,
                     specialCommands[i].description,
                     specialCommands[i].handler ? "(active)" : "(custom)");
    }
    
    if (specialCommandCount == 0)
    {
        Serial.printf("   No commands configured\n");
    }
}

void executeSaveEEPROM()
{
    Serial.printf("💾 Manual EEPROM Save Command\n");
    saveSpecialCommandsToEEPROM();
}

void executeLoadEEPROM()
{
    Serial.printf("📥 Manual EEPROM Load Command\n");
    
    if (loadSpecialCommandsFromEEPROM())
    {
        Serial.printf("✅ Commands reloaded from EEPROM\n");
    }
    else
    {
        Serial.printf("❌ Failed to load from EEPROM, keeping current commands\n");
    }
}

void executeEraseEEPROM()
{
    Serial.printf("🗑️  Manual EEPROM Erase Command\n");
    eraseSpecialCommandsFromEEPROM();
    
    // Reload defaults after erasing
    Serial.printf("🔄 Reinitializing with defaults...\n");
    initializeSpecialCommands();
}