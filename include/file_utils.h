/**
 * @file file_utils.h
 * @brief File utility functions for path manipulation and URL-to-filename conversion
 * 
 * @author Bowie Phone Project
 * @date 2025
 */

#ifndef FILE_UTILS_H
#define FILE_UTILS_H

#include <Arduino.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#ifndef MAX_FILENAME_LENGTH
#define MAX_FILENAME_LENGTH 64      ///< Maximum length for generated filenames
#endif

#ifndef AUDIO_FILES_DIR
#define AUDIO_FILES_DIR "/audio"    ///< Default directory for audio files
#endif

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Check if a string is a URL (starts with http:// or https://)
 * 
 * @param str String to check
 * @return true if the string is a URL, false otherwise
 */
bool isUrl(const char* str);

/**
 * @brief Convert URL to filesystem-safe filename (base hash only, no collision avoidance)
 * 
 * Extracts filename from URL if present, otherwise generates a hash-based filename.
 * Invalid characters are replaced or removed.
 * 
 * @param url Original URL
 * @param filename Output buffer for filename (should be at least MAX_FILENAME_LENGTH)
 * @param ext File extension to use (e.g., "wav", "mp3") - can be NULL to use default "mp3"
 * @return true if conversion successful, false otherwise
 */
bool urlToBaseFilename(const char* url, char* filename, const char* ext = nullptr);

/**
 * @brief Get local file path for a URL (uses base filename for lookups)
 * 
 * Converts a URL to a local filesystem path by generating a filename and
 * prepending the base directory.
 * 
 * @param url Original URL
 * @param localPath Output buffer for local path (should be at least 128 bytes)
 * @param ext File extension to use (e.g., "wav", "mp3") - can be NULL
 * @param baseDir Base directory for files (defaults to AUDIO_FILES_DIR)
 * @return true if path generated successfully, false otherwise
 */
bool getLocalPathForUrl(const char* url, char* localPath, const char* ext = nullptr, 
                        const char* baseDir = AUDIO_FILES_DIR);

/**
 * @brief Convert path to local path representation
 * 
 * If the path is a URL, converts it to a local SD card path using an internal static buffer.
 * If the path is already a local path, returns it unchanged.
 * 
 * @param path Original path (URL or local file path)
 * @param ext File extension for URL conversion (can be NULL)
 * @param baseDir Base directory for URL conversion (defaults to AUDIO_FILES_DIR)
 * @return Pointer to local path (either internal buffer or original path), or nullptr on error
 * @note Uses internal static buffer - return value valid until next call
 */
const char* asLocalPath(const char* path, const char* ext = nullptr,
                        const char* baseDir = AUDIO_FILES_DIR);

#endif // FILE_UTILS_H
