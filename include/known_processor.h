/**
 * @file known_processor.h
 * @brief Known Sequence Processor Header
 * 
 * This library handles downloading, caching, and processing of known DTMF 
 * sequences from a remote server. Sequences are cached on SD card for offline use.
 * 
 * @author Bowie Phone Project
 * @date 2025
 */

#ifndef KNOWN_PROCESSOR_H
#define KNOWN_PROCESSOR_H

// ============================================================================
// INCLUDES
// ============================================================================
#include <Arduino.h>

// ============================================================================
// STRUCTURES
// ============================================================================

/**
 * @brief Structure representing a known DTMF sequence
 */
struct KnownSequence
{
    const char *sequence;    ///< DTMF sequence (e.g., "123", "*67#")
    const char *description; ///< Human-readable description
    const char *type;        ///< Sequence type (e.g., "phone", "service", "shortcut", "url")
    const char *path;        ///< Additional path/URL information
};

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Initialize the known sequence processor
 * 
 * Loads cached sequences from SD card if available.
 * Call this during setup().
 */
void initializeKnownProcessor();

/**
 * @brief Download known sequences from remote server
 * @return true if download successful, false otherwise
 * 
 * Makes HTTP GET request to configured URL to download sequence definitions.
 * Only downloads if cache is stale and WiFi is connected.
 * Automatically saves to SD card for caching.
 * 
 * Expected JSON format:
 * {
 *   "<DTMF code>": {
 *     "description": "<description>",
 *     "type": "<type>",
 *     "path": "<path>"
 *   }
 * }
 */
bool downloadKnownSequences();

/**
 * @brief Check if a sequence is in the known sequences list
 * @param sequence DTMF sequence to check
 * @return true if sequence is known, false otherwise
 */
bool isKnownSequence(const char *sequence);

/**
 * @brief Process a known DTMF sequence
 * @param sequence Known sequence to process
 * @return File path for audio playback, or nullptr if not an audio sequence
 * 
 * Looks up the sequence in the known sequences list and executes
 * appropriate action based on the sequence type. For audio sequences,
 * returns the local file path if available.
 */
const char* processKnownSequence(const char *sequence);

/**
 * @brief List all known sequences to serial output
 * 
 * Useful for debugging and configuration verification.
 */
void listKnownSequences();

/**
 * @brief Get the number of loaded known sequences
 * @return Number of sequences currently loaded
 */
int getKnownSequenceCount();

/**
 * @brief Clear all known sequences from memory and SD card
 * 
 * Frees allocated memory and clears cached data.
 */
void clearKnownSequences();

// ============================================================================
// DOWNLOAD QUEUE MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * @brief Process next item in audio download queue
 * @return true if item was processed, false if queue empty or error
 * 
 * Call this function periodically in main loop to download audio files
 * in the background. Non-blocking operation.
 */
bool processAudioDownloadQueue();

/**
 * @brief Get number of items remaining in download queue
 * @return Number of items not yet processed
 */
int getDownloadQueueCount();

/**
 * @brief Get total number of items ever added to download queue
 * @return Total queue size (including processed items)
 */
int getTotalDownloadQueueSize();

/**
 * @brief List all items in download queue to serial output
 * 
 * Shows pending, in-progress, and completed downloads.
 */
void listDownloadQueue();

/**
 * @brief Clear all items from download queue
 * 
 * Resets queue to empty state. Does not delete downloaded files.
 */
void clearDownloadQueue();

/**
 * @brief Check if download queue is empty
 * @return true if no items remain to process, false otherwise
 */
bool isDownloadQueueEmpty();

#endif // KNOWN_PROCESSOR_H