/**
 * @file audio_key_registry.h
 * @brief Audio Key Registry for mapping audioKeys to resources
 * 
 * Provides a decoupled registry for mapping audio keys to:
 * - File paths
 * - URLs
 * - Tone generators
 * 
 * Uses a unified AudioEntry structure that can hold either a path or a generator.
 * 
 * @date 2025
 */

#pragma once

#include <config.h>
#include "AudioTools.h"
#include "AudioTools/CoreAudio/AudioEffects/SoundGenerator.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

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
// AUDIO ENTRY — discriminated union for all audio resource types
// ============================================================================


/**
 * @brief Reusable playback timing metadata
 */
struct AudioTiming {
    unsigned long durationMs = 0;   ///< Playback duration in ms (0 = play to end)
    unsigned long gapBefore  = 0;   ///< Silence before playback in ms
    unsigned long gapAfter   = 0;   ///< Silence after playback in ms
    unsigned long loop       = 0;   ///< Repeat count (0 = no loop, >0 = repeat N times)
};

struct AudioLink {
    std::string audioKey;            ///< Audio key name (owned copy)
    AudioTiming timing;              ///< Playback timing metadata

    AudioLink() = default;
    AudioLink(const char* key, AudioTiming t = {})
        : audioKey(key ? key : ""), timing(t) {}
};

/**
 * @brief Type-specific data for file/URL-based audio entries
 */
struct FileData {
    std::string path;            ///< Local file path (for FILE_STREAM)
    std::string alternatePath;   ///< Original URL for streaming fallback
    std::string ext;             ///< File extension (e.g., "wav", "mp3")
};

/**
 * @brief Unified entry for audio keys — holds file, URL, or generator data
 *
 * Discriminated by `type`:
 * - FILE_STREAM / URL_STREAM → `file` sub-struct is valid
 * - GENERATOR → `generator` pointer is valid
 */
struct AudioEntry {
    std::string audioKey;           ///< The key name
    AudioStreamType type;           ///< Discriminant
    AudioTiming timing;              ///< Playback timing metadata
    uint32_t contentHash = 0;        ///< Hash of source JSON for change detection
    AudioLink* previous = nullptr;   ///< Audio to play before this entry (owned, nullable)
    AudioLink* next     = nullptr;   ///< Audio to play after this entry (owned, nullable)

    union {
        FileData* file;                     ///< Owned. Valid when type is FILE_STREAM or URL_STREAM
        SoundGenerator<int16_t>* generator; ///< Not owned. Valid when type is GENERATOR
    };

    AudioEntry() : type(AudioStreamType::NONE), file(nullptr) {}

    /**
     * @brief Construct a path-based entry (file or URL)
     */
    AudioEntry(const char* key, const char* pathStr, AudioStreamType streamType, const char* urlStr = nullptr)
        : audioKey(key ? key : "")
        , type(streamType)
        , file(new FileData{pathStr ? pathStr : "", urlStr ? urlStr : "", ""}) {}

    /**
     * @brief Construct a generator-based entry from a pre-built generator
     */
    AudioEntry(const char* key, SoundGenerator<int16_t>* gen)
        : audioKey(key ? key : "")
        , type(AudioStreamType::GENERATOR)
        , generator(gen) {}

    ~AudioEntry() {
        delete previous; delete next;
        if (type != AudioStreamType::GENERATOR) delete file;
    }

    // Copy
    AudioEntry(const AudioEntry& o)
        : audioKey(o.audioKey), type(o.type), timing(o.timing), contentHash(o.contentHash)
        , previous(o.previous ? new AudioLink(*o.previous) : nullptr)
        , next(o.next ? new AudioLink(*o.next) : nullptr) {
        if (type == AudioStreamType::GENERATOR) generator = o.generator;
        else file = o.file ? new FileData(*o.file) : nullptr;
    }
    AudioEntry& operator=(const AudioEntry& o) {
        if (this == &o) return *this;
        delete previous; delete next;
        if (type != AudioStreamType::GENERATOR) delete file;
        audioKey = o.audioKey; type = o.type; timing = o.timing; contentHash = o.contentHash;
        previous = o.previous ? new AudioLink(*o.previous) : nullptr;
        next = o.next ? new AudioLink(*o.next) : nullptr;
        if (type == AudioStreamType::GENERATOR) generator = o.generator;
        else file = o.file ? new FileData(*o.file) : nullptr;
        return *this;
    }

    // Move
    AudioEntry(AudioEntry&& o) noexcept
        : audioKey(std::move(o.audioKey)), type(o.type), timing(o.timing), contentHash(o.contentHash)
        , previous(o.previous), next(o.next) {
        o.previous = nullptr; o.next = nullptr;
        if (type == AudioStreamType::GENERATOR) { generator = o.generator; o.generator = nullptr; }
        else { file = o.file; o.file = nullptr; }
        o.type = AudioStreamType::NONE;
    }
    AudioEntry& operator=(AudioEntry&& o) noexcept {
        if (this == &o) return *this;
        delete previous; delete next;
        if (type != AudioStreamType::GENERATOR) delete file;
        audioKey = std::move(o.audioKey); type = o.type; timing = o.timing; contentHash = o.contentHash;
        previous = o.previous; next = o.next;
        o.previous = nullptr; o.next = nullptr;
        if (type == AudioStreamType::GENERATOR) { generator = o.generator; o.generator = nullptr; }
        else { file = o.file; o.file = nullptr; }
        o.type = AudioStreamType::NONE;
        return *this;
    }

    // — Convenience accessors (gate on type) ——————————————————

    bool isGenerator() const { return type == AudioStreamType::GENERATOR && generator != nullptr; }
    bool isFile()      const { return type != AudioStreamType::GENERATOR && file && !file->path.empty(); }
    bool hasUrl()      const { return type != AudioStreamType::GENERATOR && file && !file->alternatePath.empty(); }

    FileData* getFile()          const { return type != AudioStreamType::GENERATOR ? file : nullptr; }
    SoundGenerator<int16_t>* getGenerator() const { return isGenerator() ? generator : nullptr; }
};

// Backward-compatible alias — remove once all call-sites are migrated
using KeyEntry = AudioEntry;

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
    static AudioKeyRegistry instance;

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
     * @brief Register a generator-based audioKey (registry takes ownership)
     * @param audioKey The key name (e.g., "dialtone", "ringback")
     * @param generator Heap-allocated SoundGenerator — registry owns and deletes it
     */
    virtual void registerGenerator(const char *audioKey, SoundGenerator<int16_t> *generator,unsigned long repeatMs=0);
    /**
     * @brief Register a generator-based audioKey (registry takes ownership)
     * @param audioKey The key name (e.g., "dialtone", "ringback")
     * @param generator Heap-allocated SoundGenerator — registry owns and deletes it
     */
    virtual void registerGenerator(const char *audioKey, SoundGenerator<int16_t> *generator,unsigned long toneMs, unsigned long silenceMs);

    /**
     * @brief Register a fully-built AudioEntry, taking ownership via move
     * 
     * For file/URL entries, handles URL detection and local-path conversion.
     * For generators, adds the pointer to ownedGenerators.
     * Skips registration if an identical entry already exists for the key.
     */
    virtual void registerEntry(AudioEntry&& entry);
    
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
     * @brief Get an AudioEntry by audioKey (const)
     * @return Pointer to the entry, or nullptr if not found
     */
    virtual const AudioEntry* getEntry(const char* audioKey) const;
    
    /**
     * @brief Get a mutable AudioEntry by audioKey
     * 
     * Use to set timing/links after registration.
     * @return Pointer to the entry, or nullptr if not found
     */
    AudioEntry* getEntryMutable(const char* audioKey);
    
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
    std::map<std::string, AudioEntry>::const_iterator begin() const { return registry.begin(); }
    
    /**
     * @brief Get iterator to end of registry
     */
    std::map<std::string, AudioEntry>::const_iterator end() const { return registry.end(); }
    
    /**
     * @brief List all registered keys to serial output
     * 
     * Useful for debugging and configuration verification.
     */
    void listKeys() const;
    
protected:
    // Unified registry - single map for all entry types
    std::map<std::string, AudioEntry> registry;
    
    // Owns dynamically-created generators (from JSON config).
    // Static generators (dialtone, ringback) are NOT in this list.
    std::vector<std::unique_ptr<SoundGenerator<int16_t>>> ownedGenerators;
    
    // Dynamic resolution callbacks (fallback when key not in registry)
    AudioKeyResolverCallback keyResolver = nullptr;
    AudioKeyExistsCallback keyExistsCallback = nullptr;
};

AudioKeyRegistry &getAudioKeyRegistry();
