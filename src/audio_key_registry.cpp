/**
 * @file audio_key_registry.cpp
 * @brief Audio Key Registry Implementation
 * 
 * Implements the unified audio key registry using a single map structure.
 * 
 * @date 2025
 */

#include "audio_key_registry.h"
#include "file_utils.h"
#include "logging.h"
#include <cstring>

// ============================================================================
// KEY REGISTRATION
// ============================================================================

void AudioKeyRegistry::registerKey(const char* audioKey, const char* path, AudioStreamType type, const char* alternatePath) {
    if (!audioKey || !path) return;
    
    KeyEntry entry(audioKey, path, type, alternatePath);
    registry[std::string(audioKey)] = entry;
    
    if (alternatePath && strlen(alternatePath) > 0) {
        Logger.printf("🔑 Registered audioKey: %s -> %s (streaming: %s)\n", 
                      audioKey, path, alternatePath);
    } else {
        Logger.printf("🔑 Registered audioKey: %s -> %s (type=%d)\n", 
                      audioKey, path, static_cast<int>(type));
    }
}

void AudioKeyRegistry::registerKey(const char* audioKey, const char* primaryPath, const char* ext) {
    if (!audioKey || !primaryPath) return;
    
    // Check if primaryPath is a URL
    if (isUrl(primaryPath)) {
        // Generate local path if this is a URL
        const char *localPath = asLocalPath(primaryPath, ext);
        if (localPath && strlen(localPath) == 0)
          localPath = nullptr;
        // URL with local path: use local as primary, URL as streaming fallback
        registerKey(audioKey, localPath, AudioStreamType::FILE_STREAM, primaryPath);
    } else {
        // Local path only
        registerKey(audioKey, primaryPath, AudioStreamType::FILE_STREAM, nullptr);
    }
}

void AudioKeyRegistry::registerGenerator(const char* audioKey, SoundGenerator<int16_t>* generator) {
    if (!audioKey || !generator) return;
    
    KeyEntry entry(audioKey, generator);
    registry[std::string(audioKey)] = entry;
    
    Logger.printf("🎵 Registered generator: %s\n", audioKey);
}

void AudioKeyRegistry::unregisterKey(const char* audioKey) {
    if (!audioKey) return;
    registry.erase(std::string(audioKey));
    Logger.printf("🔑 Unregistered audioKey: %s\n", audioKey);
}

void AudioKeyRegistry::clearKeys() {
    registry.clear();
    Logger.println("🔑 Cleared all audioKeys");
}

// ============================================================================
// KEY LOOKUP
// ============================================================================

bool AudioKeyRegistry::hasKey(const char* audioKey) const {
    if (!audioKey) return false;
    
    // Check unified registry
    if (registry.find(std::string(audioKey)) != registry.end()) {
        return true;
    }
    
    // Check dynamic callback
    if (keyExistsCallback && keyExistsCallback(audioKey)) {
        return true;
    }
    
    // Try resolver as fallback
    if (keyResolver && keyResolver(audioKey) != nullptr) {
        return true;
    }
    
    return false;
}

bool AudioKeyRegistry::hasKeyWithPrefix(const char* prefix) const {
    if (!prefix) return false;
    
    size_t prefixLen = strlen(prefix);
    
    for (const auto& pair : registry) {
        if (pair.first.length() >= prefixLen && 
            pair.first.compare(0, prefixLen, prefix) == 0) {
            return true;
        }
    }
    
    return false;
}

const KeyEntry* AudioKeyRegistry::getEntry(const char* audioKey) const {
    if (!audioKey) return nullptr;
    
    auto it = registry.find(std::string(audioKey));
    if (it != registry.end()) {
        return &it->second;
    }
    
    return nullptr;
}

bool AudioKeyRegistry::hasGenerator(const char* audioKey) const {
    if (!audioKey) return false;
    
    auto it = registry.find(std::string(audioKey));
    if (it != registry.end()) {
        return it->second.isGenerator();
    }
    
    return false;
}

SoundGenerator<int16_t>* AudioKeyRegistry::getGenerator(const char* audioKey) const {
    if (!audioKey) return nullptr;
    
    auto it = registry.find(std::string(audioKey));
    if (it != registry.end() && it->second.isGenerator()) {
        return it->second.generator;
    }
    
    return nullptr;
}

const char* AudioKeyRegistry::resolveKey(const char* audioKey) const {
    if (!audioKey) return nullptr;
    
    // Check unified registry
    auto it = registry.find(std::string(audioKey));
    if (it != registry.end()) {
        // Generators don't have paths - return nullptr
        if (it->second.isGenerator()) {
            return nullptr;
        }
        return it->second.path.c_str();
    }
    
    // Try dynamic resolver as fallback
    if (keyResolver) {
        return keyResolver(audioKey);
    }
    
    return nullptr;
}

AudioStreamType AudioKeyRegistry::getKeyType(const char* audioKey) const {
    if (!audioKey) return AudioStreamType::NONE;
    
    // Check unified registry
    auto it = registry.find(std::string(audioKey));
    if (it != registry.end()) {
        return it->second.type;
    }
    
    // Check for URLs (inline detection)
    if (isUrl(audioKey)) {
        return AudioStreamType::URL_STREAM;
    }
    
    // If resolvable via callback, assume file
    if (keyResolver && keyResolver(audioKey) != nullptr) {
        return AudioStreamType::FILE_STREAM;
    }
    
    // Default to file stream
    return AudioStreamType::FILE_STREAM;
}

void AudioKeyRegistry::listKeys() const {
    int count = registry.size();
    
    Logger.printf("📋 Audio Keys (%d total):\n", count);
    Logger.println("============================================================");
    
    if (count == 0) {
        Logger.println("   No audio keys registered.");
        return;
    }
    
    int index = 1;
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        const KeyEntry& entry = it->second;
        Logger.printf("%2d. %s\n", index++, entry.audioKey.c_str());
        
        if (entry.getDescription()) {
            Logger.printf("    Description: %s\n", entry.getDescription());
        }
        
        const char* typeStr = "unknown";
        switch (entry.type) {
            case AudioStreamType::GENERATOR: typeStr = "generator"; break;
            case AudioStreamType::URL_STREAM: typeStr = "url"; break;
            case AudioStreamType::FILE_STREAM: typeStr = "file"; break;
            default: break;
        }
        Logger.printf("    Type: %s\n", typeStr);
        
        if (entry.getPath()) {
            Logger.printf("    Path: %s\n", entry.getPath());
        }
        Logger.println();
    }
}
