/**
 * @file sequence_processor.h
 * @brief DTMF Sequence Processing Library
 *
 * This library handles processing of complete DTMF sequences, including
 * phone number validation, command interpretation, and action execution.
 *
 * @author Bowie Phone Project
 * @date 2025
 */

#ifndef SEQUENCE_PROCESSOR_H
#define SEQUENCE_PROCESSOR_H

// ============================================================================
// INCLUDES
// ============================================================================
#include <Arduino.h>
#include "special_command_processor.h"
#include "audio_file_manager.h"

// ============================================================================
// CONSTANTS AND DEFINITIONS
// ============================================================================

/**
 * @brief Sequence processing configuration
 */
#define MAX_SPECIAL_COMMANDS 16    ///< Maximum number of configurable special commands

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Process a complete DTMF sequence
 * @param sequence Null-terminated string containing DTMF digits
 * @return File path for audio playback, or nullptr if not an audio sequence
 *
 * This is the main entry point for sequence processing. It analyzes the
 * sequence and determines the appropriate action (emergency, phone number,
 * command, etc.). Returns a file path if the sequence triggers audio playback.
 */
const char* processNumberSequence(const char *sequence);

/**
 * @brief Check if a sequence is a known number (phone number)
 * @param sequence Sequence to check
 * @return true if it's a known number format
 */
bool isKnownNumber(const char *sequence);

/**
 * @brief Process a known number sequence
 * @param sequence Known number sequence to process
 */
void processKnownNumber(const char *sequence);

/**
 * @brief Process an unknown/invalid sequence
 * @param sequence Unknown sequence
 */
void processUnknownSequence(const char *sequence);

#endif // SEQUENCE_PROCESSOR_H