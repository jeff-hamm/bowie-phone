/**
 * @file audio_key_registry.h
 * @brief Audio Key Registry for mapping audioKeys to resources
 * 
 * Provides a decoupled registry for mapping audio keys to:
 * - File paths
 * - URLs
 * - Tone generators
 * 
 * Uses a unified KeyEntry structure that can hold either a path or a generator.
 * 
 * @date 2025
 */

#pragma once

#include <config.h>
#include "AudioTools.h"
#include "AudioTools/CoreAudio/AudioEffects/SoundGenerator.h"
#include <map>
#include <string>

using namespace audio_tools;

// ============================================================================
// STREAM TYPE DEFINITIONS
// ============================================================================

/**
 * @brief Types of audio streams supported
 */
enum class AudioStreamType {
    NONE,
    GENERATOR,      // Synthesized tones
    URL_STREAM,     // HTTP/HTTPS streaming
    FILE_STREAM     // SD card file
};

// ============================================================================
// KEY ENTRY STRUCTURE
// ============================================================================

/**
 * @brief Unified entry for audio keys - contains either a path or a generator
 * 
 * Each KeyEntry holds:
 * - The audioKey name
 * - The stream type
 * - Either a path (for files/URLs) or a generator pointer (for tones)
 * - Optional metadata (description, extension)
 */
struct KeyEntry {
    std::string audioKey;           ///< The key name
    AudioStreamType type;           ///< Type of audio stream
    
    // Union-like storage: either path or generator (only one is valid based on type)
    std::string path;               ///< Local file path (for FILE_STREAM)
    std::string alternatePath;       ///< Original URL for streaming fallback
    SoundGenerator<int16_t>* generator = nullptr;  ///< Generator pointer (for GENERATOR type)
    
    // Metadata
    std::string description;        ///< Human-readable description
    std::string ext;                ///< File extension (e.g., "wav", "mp3")
    
    KeyEntry() : type(AudioStreamType::NONE) {}
    
    /**
     * @brief Construct a path-based entry (file or URL)
     * @param key The audio key name
     * @param pathStr Local file path
     * @param streamType The stream type
     * @param urlStr Optional streaming URL for fallback
     */
    KeyEntry(const char* key, const char* pathStr, AudioStreamType streamType, const char* urlStr = nullptr)
        : audioKey(key ? key : "")
        , type(streamType)
        , path(pathStr ? pathStr : "")
        , alternatePath(urlStr ? urlStr : "")
        , generator(nullptr) {}
    
    /**
     * @brief Construct a generator-based entry
     */
    KeyEntry(const char* key, SoundGenerator<int16_t>* gen)
        : audioKey(key ? key : "")
        , type(AudioStreamType::GENERATOR)
        , path("")
        , generator(gen) {}
    
    /**
     * @brief Check if this entry represents a generator
     */
    bool isGenerator() const { return type == AudioStreamType::GENERATOR && generator != nullptr; }
    
    /**
     * @brief Check if this entry represents a path (file or URL)
     */
    bool isPath() const { return type != AudioStreamType::GENERATOR && !path.empty(); }
    
    /**
     * @brief Check if this entry has a streaming URL
     */
    bool hasUrl() const { return !alternatePath.empty(); }

    /**
     * @brief Get the path or nullptr if this is a generator
     */
    const char* getPath() const { return isPath() ? path.c_str() : nullptr; }
    
    /**
     * @brief Get the streaming URL or nullptr if not set
     */
    const char* getUrl() const { return hasUrl() ? alternatePath.c_str() : nullptr; }
    
    /**
     * @brief Get the generator or nullptr if this is a path
     */
    SoundGenerator<int16_t>* getGenerator() const { return isGenerator() ? generator : nullptr; }
    
    /**
     * @brief Get the description
     */
    const char* getDescription() const { return description.empty() ? nullptr : description.c_str(); }
    
    /**
     * @brief Get the file extension
     */
    const char* getExt() const { return ext.empty() ? nullptr : ext.c_str(); }
};

// ============================================================================
// CALLBACK TYPES
// ============================================================================

/**
 * @brief Callback type for resolving audioKeys to resource paths
 * @param audioKey The key to resolve
 * @return The resolved path/URL, or nullptr if not found
 */
typedef const char* (*AudioKeyResolverCallback)(const char* audioKey);

/**
 * @brief Callback type for checking if an audioKey exists
 * @param audioKey The key to check
 * @return true if the key exists
 */
typedef bool (*AudioKeyExistsCallback)(const char* audioKey);

// ============================================================================
// AUDIO KEY REGISTRY
// ============================================================================

/**
 * @brief Registry for mapping audioKeys to audio resources
 * 
 * Uses a single unified map where each entry can represent either:
 * - A file path (FILE_STREAM)
 * - A URL (URL_STREAM)  
 * - A tone generator (GENERATOR)
 * 
 * Can be subclassed to provide custom resolution logic.
 */
class AudioKeyRegistry {
public:
    AudioKeyRegistry() = default;
    virtual ~AudioKeyRegistry() = default;
    
    // ========================================================================
    // KEY REGISTRATION
    // ========================================================================
    
    /**
     * @brief Register a path-based audioKey (file or URL)
     * @param audioKey The key name
     * @param path The local file path
     * @param type The stream type (FILE_STREAM or URL_STREAM)
     * @param alternatePath Optional streaming URL for fallback
     */
    virtual void registerKey(const char* audioKey, const char* path, AudioStreamType type, const char* alternatePath = nullptr);
    
    /**
     * @brief Register a path-based audioKey with automatic stream type detection
     * 
     * Auto-detects stream type based on path:
     * - Paths starting with http:// or https:// -> URL_STREAM
     * - All other paths -> FILE_STREAM
     * 
     * If primaryPath is a URL and localPath is provided, the entry will use
     * localPath as the primary path (FILE_STREAM) with primaryPath stored
     * as the streaming fallback URL.
     * 
     * @param audioKey The key name
     * @param primaryPath The primary path (local file or URL)
     * @param localPath Optional local path - if provided and primaryPath is a URL,
     *                  localPath becomes primary and primaryPath becomes streaming fallback
     */
    virtual void registerKey(const char* audioKey, const char* primaryPath, const char* ext = nullptr);
    
    /**
     * @brief Register a generator-based audioKey
     * @param audioKey The key name (e.g., "dialtone", "ringback")
     * @param generator Pointer to the SoundGenerator (caller retains ownership)
     */
    virtual void registerGenerator(const char* audioKey, SoundGenerator<int16_t>* generator);
    
    /**
     * @brief Unregister an audioKey (path or generator)
     */
    virtual void unregisterKey(const char* audioKey);
    
    /**
     * @brief Clear all registered keys
     */
    virtual void clearKeys();
    
    // ========================================================================
    // KEY LOOKUP
    // ========================================================================
    
    /**
     * @brief Check if an audioKey is registered
     */
    virtual bool hasKey(const char* audioKey) const;
    
    /**
     * @brief Check if any audioKey starts with the given prefix
     * @param prefix Prefix to check (e.g., "91" would match "911", "912", etc.)
     * @return true if any key starts with prefix
     */
    virtual bool hasKeyWithPrefix(const char* prefix) const;
    
    /**
     * @brief Get a KeyEntry by audioKey
     * @return Pointer to the entry, or nullptr if not found
     */
    virtual const KeyEntry* getEntry(const char* audioKey) const;
    
    /**
     * @brief Check if the key is a registered generator
     */
    virtual bool hasGenerator(const char* audioKey) const;
    
    /**
     * @brief Get a registered generator by name
     * @return The generator, or nullptr if not found or not a generator type
     */
    virtual SoundGenerator<int16_t>* getGenerator(const char* audioKey) const;
    
    /**
     * @brief Resolve an audioKey to its resource path
     * @return The path/URL, or nullptr if not found or is a generator
     */
    virtual const char* resolveKey(const char* audioKey) const;
    
    /**
     * @brief Get the stream type for an audioKey
     */
    virtual AudioStreamType getKeyType(const char* audioKey) const;
    
    // ========================================================================
    // DYNAMIC RESOLUTION CALLBACKS
    // ========================================================================
    
    /**
     * @brief Set callback for dynamic audioKey resolution (fallback)
     */
    void setKeyResolver(AudioKeyResolverCallback resolver) { keyResolver = resolver; }
    
    /**
     * @brief Set callback for checking if a key exists (fallback)
     */
    void setKeyExistsCallback(AudioKeyExistsCallback callback) { keyExistsCallback = callback; }
    
    // ========================================================================
    // ITERATION
    // ========================================================================
    
    /**
     * @brief Get the number of registered keys
     */
    size_t size() const { return registry.size(); }
    
    /**
     * @brief Get iterator to beginning of registry
     */
    std::map<std::string, KeyEntry>::const_iterator begin() const { return registry.begin(); }
    
    /**
     * @brief Get iterator to end of registry
     */
    std::map<std::string, KeyEntry>::const_iterator end() const { return registry.end(); }
    
    /**
     * @brief List all registered keys to serial output
     * 
     * Useful for debugging and configuration verification.
     */
    void listKeys() const;
    
protected:
    // Unified registry - single map for all entry types
    std::map<std::string, KeyEntry> registry;
    
    // Dynamic resolution callbacks (fallback when key not in registry)
    AudioKeyResolverCallback keyResolver = nullptr;
    AudioKeyExistsCallback keyExistsCallback = nullptr;
};
