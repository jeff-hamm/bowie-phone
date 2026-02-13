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
// SYSTEM FUNCTIONS
// ============================================================================

/**
 * @brief Enter firmware update/bootloader mode
 */
void enterFirmwareUpdateMode();

/**
 * @brief Shutdown audio for OTA updates
 */
void shutdownAudioForOTA();

/**
 * @brief Process debug input from a stream (Serial or Telnet)
 * @param input Stream to read debug commands from
 */
void processDebugInput(Stream& input);

/**
 * @brief CPU load test for Goertzel-based DTMF detection during audio playback
 */
void performGoertzelCPULoadTest();

/**
 * @brief Initialize audio capture state from NVS
 * Called automatically during initializeSpecialCommands().
 * Restores any previously armed audio captures that survived reboot.
 */
void initAudioCaptureState();

/**
 * @brief Check if audio capture should be triggered on this off-hook event
 * Should be called from the off-hook handler in main loop.
 * If capture was previously armed via "debugaudio" command, this will execute
 * the capture and clear the flag for one-time operation.
 * @return true if capture was triggered, false if no capture was armed
 */
bool checkAndExecuteOffHookCapture();

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

/**
 * @brief Perform comprehensive SD card initialization debugging
 * Tests multiple initialization methods and pin configurations
 * to diagnose SD card issues. Can be run early via -DRUN_SD_DEBUG_FIRST.
 */
void performSDCardDebug();

// ============================================================================
// DEFAULT COMMAND HANDLERS
// ============================================================================

/**
 * @brief Display system status information
 */
void executeSystemStatus();

/**
 * @brief Reboot the device
 */
void executeReboot();

/**
 * @brief Perform factory reset (erase settings and reboot)
 */
void executeFactoryReset();

/**
 * @brief Display device information
 */
void executeDeviceInfo();

/**
 * @brief Refresh audio catalog from server
 */
void executeRefreshAudio();

/**
 * @brief Prepare device for OTA update
 */
void executePrepareOTA();

/**
 * @brief Trigger manual phone home check-in
 */
void executePhoneHome();

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