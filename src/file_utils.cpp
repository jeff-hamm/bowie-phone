/**
 * @file file_utils.cpp
 * @brief File utility functions implementation
 * 
 * @author Bowie Phone Project
 * @date 2025
 */

#include "file_utils.h"
#include "config.h"
#include <cstring>
#include <cctype>

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

bool isUrl(const char* str)
{
    if (!str) {
        return false;
    }
    return (strncmp(str, "http://", 7) == 0 || strncmp(str, "https://", 8) == 0);
}

// ============================================================================
// URL TO FILENAME CONVERSION
// ============================================================================

bool urlToBaseFilename(const char* url, char* filename, const char* ext)
{
    if (!url || !filename)
    {
        return false;
    }
    
    // Determine extension to use (default to wav if not specified)
    const char* extension = (ext && strlen(ext) > 0) ? ext : DEFAULT_EXTENSION;
    
    // Extract filename from URL path or generate from URL hash
    const char* lastSlash = strrchr(url, '/');
    const char* urlFilename = lastSlash ? (lastSlash + 1) : url;
    
    // If we have a proper filename with extension, use it
    if (strlen(urlFilename) > 0 && strchr(urlFilename, '.'))
    {
        // Clean the filename - replace invalid characters
        int j = 0;
        for (int i = 0; urlFilename[i] && j < MAX_FILENAME_LENGTH - 1; i++)
        {
            char c = urlFilename[i];
            // Allow alphanumeric, dots, hyphens, underscores
            if (isalnum(c) || c == '.' || c == '-' || c == '_')
            {
                filename[j++] = c;
            }
            else if (c == ' ')
            {
                filename[j++] = '_';
            }
            // Skip other invalid characters
        }
        filename[j] = '\0';
        
        if (strlen(filename) > 0)
        {
            return true;
        }
    }
    
    // Generate filename from URL hash if no suitable filename found
    unsigned long hash = 5381;
    for (int i = 0; url[i]; i++)
    {
        hash = ((hash << 5) + hash) + url[i];
    }
    
    // Just use the base filename without collision avoidance
    snprintf(filename, MAX_FILENAME_LENGTH, "audio_%08lx.%s", hash, extension);
    return true;
}

bool getLocalPathForUrl(const char* url, char* localPath, const char* ext, const char* baseDir)
{
    if (!url || !localPath)
    {
        return false;
    }
    
    // Use AUDIO_FILES_DIR if no base directory specified
    const char* dir = (baseDir && strlen(baseDir) > 0) ? baseDir : AUDIO_FILES_DIR;
    
    char filename[MAX_FILENAME_LENGTH];
    // Use base filename (without collision avoidance) for path lookups
    if (!urlToBaseFilename(url, filename, ext))
    {
        return false;
    }
    
    snprintf(localPath, 128, "%s/%s", dir, filename);
    return true;
}

const char* asLocalPath(const char* path, const char* ext, const char* baseDir)
{
    if (!path)
    {
        return nullptr;
    }
    
    if (isUrl(path))
    {
        // Convert URL to local path using internal static buffer
        static char buffer[128];
        if (getLocalPathForUrl(path, buffer, ext, baseDir))
        {
            return buffer;
        }
        return nullptr;
    }
    
    // Already a local path, return as-is
    return path;
}
