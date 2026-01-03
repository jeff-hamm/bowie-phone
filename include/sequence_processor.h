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
 * @brief Read and process DTMF input, returning audio path when ready
 * @return Audio file path to play, or nullptr if no sequence ready or no audio
 *
 * This function:
 * - Analyzes DTMF input every 50ms
 * - Builds up a sequence buffer
 * - Stops dial tone on first digit
 * - Checks for matching audio keys in real-time
 * - '*' key completes the current sequence (excluding the '*')
 * - Processes complete sequences
 * - Resets internal state after processing
 * - Returns audio path ready for playback (may be URL or SD path)
 *
 * Usage in main loop:
 *   const char* audioPath = readDTMFSequence();
 *   if (audioPath) {
 *     playAudioPath(audioPath);
 *   }
 */
const char* readDTMFSequence();

/**
 * @brief Reset the DTMF sequence buffer
 *
 * Clears the current DTMF sequence. Should be called when the phone
 * goes on-hook or when you need to discard the current sequence.
 */
void resetDTMFSequence();

/**
 * @brief Simulate a DTMF digit for debug/testing purposes
 * @param digit The DTMF digit to simulate (0-9, *, #)
 *
 * This function injects a digit directly into the sequence buffer
 * as if it were detected from audio input. Useful for testing
 * sequence processing without actual DTMF audio.
 */
void simulateDTMFDigit(char digit);

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