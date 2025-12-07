/**
 * @file special_command_processor.h
 * @brief Special Command Processing Library
 *
 * This library handles all special DTMF command processing, including
 * configurable commands, default handlers, and system functions.
 *
 * @author Bowie Phone Project
 * @date 2025
 */

#ifndef SPECIAL_COMMAND_PROCESSOR_H
#define SPECIAL_COMMAND_PROCESSOR_H

// ============================================================================
// INCLUDES
// ============================================================================
#include <Arduino.h>

// ============================================================================
// CONSTANTS AND DEFINITIONS
// ============================================================================

/**
 * @brief Maximum number of configurable special commands
 */
#define MAX_SPECIAL_COMMANDS 16

/**
 * @brief Structure for configurable special commands
 */
struct SpecialCommand
{
    const char *sequence;
    const char *description;
    void (*handler)(void);  ///< Function pointer to command handler
};

// ============================================================================
// SPECIAL COMMAND MANAGEMENT
// ============================================================================

/**
 * @brief Initialize special commands with default or configured values
 */
void initializeSpecialCommands();

/**
 * @brief Add a custom special command
 * @param sequence DTMF sequence (e.g., "*123#")
 * @param description Human-readable description
 * @param handler Function to call when command is executed
 * @return true if added successfully, false if command table is full
 */
bool addSpecialCommand(const char *sequence, const char *description, void (*handler)(void));

/**
 * @brief Get the number of registered special commands
 * @return Number of commands currently registered
 */
int getSpecialCommandCount();

/**
 * @brief Clear all special commands
 */
void clearSpecialCommands();

/**
 * @brief Check if sequence is a special command
 * @param sequence DTMF sequence to check
 * @return true if special command, false otherwise
 */
bool isSpecialCommand(const char *sequence);

/**
 * @brief Process a special command
 * @param sequence Command sequence
 */
void processSpecialCommand(const char *sequence);

// ============================================================================
// DEFAULT COMMAND HANDLERS
// ============================================================================

/**
 * @brief Display system status information
 */
void executeSystemStatus();

/**
 * @brief Reset WiFi configuration
 */
void executeWiFiReset();

/**
 * @brief Restart the device
 */
void executeDeviceReset();

/**
 * @brief Perform factory reset
 */
void executeFactoryReset();

/**
 * @brief Toggle debug mode
 */
void executeDebugToggle();

/**
 * @brief Run audio system test
 */
void executeAudioTest();

/**
 * @brief Display device information
 */
void executeDeviceInfo();

/**
 * @brief Access admin menu
 */
void executeAdminMenu();

/**
 * @brief List all configured commands
 */
void executeListCommands();

/**
 * @brief Manually save commands to EEPROM
 */
void executeSaveEEPROM();

/**
 * @brief Manually load commands from EEPROM
 */
void executeLoadEEPROM();

/**
 * @brief Manually erase EEPROM and reload defaults
 */
void executeEraseEEPROM();

/**
 * @brief Display Tailscale/WireGuard VPN status
 */
void executeTailscaleStatus();

// ============================================================================
// EEPROM PERSISTENCE FUNCTIONS
// ============================================================================

/**
 * @brief Save current special commands to EEPROM
 */
void saveSpecialCommandsToEEPROM();

/**
 * @brief Load special commands from EEPROM
 * @return true if commands were loaded successfully, false otherwise
 */
bool loadSpecialCommandsFromEEPROM();

/**
 * @brief Erase all special commands from EEPROM
 */
void eraseSpecialCommandsFromEEPROM();

/**
 * @brief Assign default handler based on command sequence
 * @param index Command index in array
 * @param sequence DTMF sequence
 */
void assignDefaultHandler(int index, const char* sequence);

#endif // SPECIAL_COMMAND_PROCESSOR_H