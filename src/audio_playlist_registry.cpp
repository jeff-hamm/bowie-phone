/**
 * @file audio_playlist_registry.cpp
 * @brief Audio Playlist Registry Implementation
 * 
 * @date 2025
 */

#include "audio_playlist_registry.h"
#include "logging.h"
#include "tone_generators.h"

// ============================================================================
// GLOBAL TONE GENERATORS
// ============================================================================

// Dial tone: 350 Hz + 440 Hz (North American standard)
static DualToneGenerator dialToneGenerator(350.0f, 440.0f, 16000.0f);

// Ringback base tone: 440 Hz + 480 Hz (North American standard)
static DualToneGenerator ringbackToneGenerator(440.0f, 480.0f, 16000.0f);

// Ringback with cadence: 2 seconds on, 4 seconds off
static RepeatingToneGenerator<int16_t> ringbackRepeater(ringbackToneGenerator, 2000, 4000);

// ============================================================================
// GLOBAL INSTANCES
// ============================================================================

// Global key registry
static AudioKeyRegistry* globalKeyRegistry = nullptr;

// Global playlist registry  
static AudioPlaylistRegistry* globalPlaylistRegistry = nullptr;


AudioKeyRegistry& getAudioKeyRegistry() {
    if (!globalKeyRegistry) {
        globalKeyRegistry = new AudioKeyRegistry();        
        Logger.println("✅ Global AudioKeyRegistry initialized with tone generators");
    }
    return *globalKeyRegistry;
}

AudioPlaylistRegistry& getAudioPlaylistRegistry() {
    if (!globalPlaylistRegistry) {
        globalPlaylistRegistry = new AudioPlaylistRegistry();
        globalPlaylistRegistry->setKeyRegistry(&getAudioKeyRegistry());
        
        Logger.println("✅ Global AudioPlaylistRegistry initialized");
    }
    return *globalPlaylistRegistry;
}

// ============================================================================
// PLAYLIST MANAGEMENT
// ============================================================================

Playlist* AudioPlaylistRegistry::createPlaylist(const char* name, bool overwrite) {
    if (!name || strlen(name) == 0) return nullptr;
    
    std::string key(name);
    
    // Check if already exists
    auto playlist = this->getPlaylistMutable(name);
    if (playlist) {
        Logger.printf("⚠️ Playlist already exists: %s\n", name);
        if (!overwrite) {
            return playlist;
        }
    }
    
    playlists[key] = Playlist(name, keyRegistry);
    Logger.printf("📋 Created playlist: %s\n", name);
    return &playlists[key];
}

bool AudioPlaylistRegistry::deletePlaylist(const char* name) {
    if (!name) return false;
    
    auto it = playlists.find(std::string(name));
    if (it != playlists.end()) {
        playlists.erase(it);
        Logger.printf("🗑️ Deleted playlist: %s\n", name);
        return true;
    }
    
    return false;
}

void AudioPlaylistRegistry::clearPlaylists() {
    playlists.clear();
    Logger.println("🗑️ Cleared all playlists");
}

bool AudioPlaylistRegistry::hasPlaylist(const char* name) const {
    if (!name) return false;
    return playlists.find(std::string(name)) != playlists.end();
}

const Playlist* AudioPlaylistRegistry::getPlaylist(const char* name) const {
    if (!name) return nullptr;
    
    auto it = playlists.find(std::string(name));
    if (it != playlists.end()) {
        return &it->second;
    }
    return nullptr;
}

Playlist* AudioPlaylistRegistry::getPlaylistMutable(const char* name) {
    if (!name) return nullptr;
    
    auto it = playlists.find(std::string(name));
    if (it != playlists.end()) {
        return &it->second;
    }
    return nullptr;
}

// ============================================================================
// PLAYLIST MODIFICATION
// ============================================================================

bool AudioPlaylistRegistry::appendToPlaylist(const char* playlistName, const char* audioKey, unsigned long durationMs) {
    if (!playlistName || !audioKey) return false;
    
    Playlist* playlist = getPlaylistMutable(playlistName);
    if (!playlist) {
        // Auto-create if doesn't exist
        if (!createPlaylist(playlistName)) return false;
        playlist = getPlaylistMutable(playlistName);
        if (!playlist) return false;
    }
    
    // Validate entry exists in registry if we have one
    if (keyRegistry) {
        const KeyEntry* entry = keyRegistry->getEntry(audioKey);
        if (!entry) {
            Logger.printf("⚠️ Warning: audioKey '%s' not found in registry (appending anyway)\n", audioKey);
        }
    }
    
    playlist->nodes.emplace_back(audioKey, durationMs);
    Logger.printf("📋 Appended to '%s': %s (duration=%lu)\n", playlistName, audioKey, durationMs);
    
    return true;
}

bool AudioPlaylistRegistry::appendToPlaylist(const char* playlistName, const KeyEntry* entry, unsigned long durationMs) {
    if (!playlistName || !entry) return false;
    
    // Delegate to string-based version
    return appendToPlaylist(playlistName, entry->audioKey.c_str(), durationMs);
}

bool AudioPlaylistRegistry::clearPlaylist(const char* playlistName) {
    if (!playlistName) return false;
    
    Playlist* playlist = getPlaylistMutable(playlistName);
    if (playlist) {
        playlist->clear();
        Logger.printf("📋 Cleared playlist: %s\n", playlistName);
        return true;
    }
    
    return false;
}

bool AudioPlaylistRegistry::setPlaylist(const char* playlistName, 
                                        const char** audioKeys, 
                                        const unsigned long* durations, 
                                        size_t count) {
    if (!playlistName || !audioKeys || count == 0) return false;
    
    // Create or clear the playlist
    if (!hasPlaylist(playlistName)) {
        if (!createPlaylist(playlistName)) return false;
    } else {
        clearPlaylist(playlistName);
    }
    
    // Append all items
    for (size_t i = 0; i < count; i++) {
        if (audioKeys[i]) {
            unsigned long dur = durations ? durations[i] : 0;
            appendToPlaylist(playlistName, audioKeys[i], dur);
        }
    }
    
    Logger.printf("📋 Set playlist '%s' with %d items\n", playlistName, (int)count);
    return true;
}

// ============================================================================
// RESOLUTION
// ============================================================================

size_t AudioPlaylistRegistry::resolvePlaylist(const char* playlistName) {
    if (!playlistName || !keyRegistry) return 0;
    
    Playlist* playlist = getPlaylistMutable(playlistName);
    if (!playlist) return 0;
    
    // Validate that each audioKey exists in the registry
    size_t valid = 0;
    for (const auto& node : playlist->nodes) {
        const KeyEntry* entry = keyRegistry->getEntry(node.audioKey.c_str());
        if (entry) {
            valid++;
        } else {
            Logger.printf("⚠️ audioKey '%s' not found in registry\n", node.audioKey.c_str());
        }
    }
    
    Logger.printf("📋 Resolved playlist '%s': %d/%d nodes valid\n", 
                  playlistName, (int)valid, (int)playlist->nodes.size());
    
    return valid;
}

size_t AudioPlaylistRegistry::resolveAllPlaylists() {
    size_t total = 0;
    
    for (auto& kv : playlists) {
        total += resolvePlaylist(kv.first.c_str());
    }
    
    Logger.printf("📋 Resolved all playlists: %d total nodes\n", (int)total);
    return total;
}

// ============================================================================
// PLAYLIST MEMBER FUNCTIONS
// ============================================================================

bool Playlist::appendEntry(const char* audioKey, unsigned long durationMs) {
    if (!audioKey) return false;
    
    // Validate entry exists in registry if we have one
    if (keyRegistry) {
        const KeyEntry* entry = keyRegistry->getEntry(audioKey);
        if (!entry) {
            Logger.printf("⚠️ Warning: audioKey '%s' not found in registry (appending anyway)\n", audioKey);
        }
    }
    
    nodes.emplace_back(audioKey, durationMs);
    return true;
}

bool Playlist::prependEntry(const char* audioKey, unsigned long durationMs) {
    if (!audioKey) return false;
    
    // Validate entry exists in registry if we have one
    if (keyRegistry) {
        const KeyEntry* entry = keyRegistry->getEntry(audioKey);
        if (!entry) {
            Logger.printf("⚠️ Warning: audioKey '%s' not found in registry (prepending anyway)\n", audioKey);
        }
    }
    
    nodes.insert(nodes.begin(), PlaylistNode(audioKey, durationMs));
    return true;
}

bool Playlist::replaceEntry(size_t index, const char* audioKey, unsigned long durationMs) {
    if (!audioKey || index >= nodes.size()) return false;
    
    // Validate entry exists in registry if we have one
    if (keyRegistry) {
        const KeyEntry* entry = keyRegistry->getEntry(audioKey);
        if (!entry) {
            Logger.printf("⚠️ Warning: audioKey '%s' not found in registry (replacing anyway)\n", audioKey);
        }
    }
    
    nodes[index] = PlaylistNode(audioKey, durationMs);
    return true;
}
