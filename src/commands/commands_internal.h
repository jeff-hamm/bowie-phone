/**
 * @file commands_internal.h
 * @brief Internal shared header for src/commands/*.cpp only.
 *
 * Do NOT include this from public headers or from files outside src/commands/.
 * Public API lives in include/special_command_processor.h.
 */

#pragma once

// ============================================================================
// ALL INCLUDES (shared across all commands/*.cpp files)
// ============================================================================

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
#include "dtmf_decoder.h"
#include "phone.h"
#include <HTTPClient.h>
#include "esp_heap_caps.h"
#include "config.h"

// ============================================================================
// SHARED CONSTANTS
// ============================================================================

#define PREFERENCES_NAMESPACE "bowiephone"

// ============================================================================
// PRIVATE FORWARD DECLARATIONS
// (functions not exposed in the public header but called across commands/ files)
// ============================================================================

// audio_capture.cpp — called by processDebugCommand and checkAndExecuteOffHookCapture
void performAudioCapture(int durationSec);

// audio_output_test.cpp — called by processDebugCommand
void performAudioOutputTest();

// debug_input.cpp — called by processDebugCommand
void performDebugInput(const char* filename);
