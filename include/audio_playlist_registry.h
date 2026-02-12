/**
 * @file audio_playlist_registry.h
 * @brief Audio Playlist Registry for managing named audio playlists
 * 
 * Works with AudioKeyRegistry to create playlists that reference
 * registered audio keys. Each playlist is a sequence of audio items
 * with optional durations.
 * 
 * @date 2025
 */

#pragma once

#include "audio_key_registry.h"
#include <vector>
#include <map>
#include <string>

// ============================================================================
// PLAYLIST NODE
// ============================================================================

/**
 * @brief A single item in a playlist
 * 
 * Stores an audioKey name and duration. The actual KeyEntry is resolved
 * by the playlist's registry when needed.
 */
struct PlaylistNode {
    std::string audioKey;       ///< The audio key name
    unsigned long gap;          ///< Gap duration in ms before this item
    unsigned long durationMs;   ///< Duration in milliseconds (0 = play to completion)
    
    PlaylistNode() : gap(0), durationMs(0) {}
    
    PlaylistNode(const char* key, unsigned long gapDuration = 0, unsigned long duration = 0)
        : audioKey(key ? key : "")
        , gap(gapDuration)
        , durationMs(duration) {}
    
    /**
     * @brief Check if this node is valid (has a non-empty audioKey)
     */
    bool isValid() const { return !audioKey.empty(); }
    
    /**
     * @brief Get the audio key for this node
     */
    const char* getAudioKey() const { return audioKey.c_str(); }
    
    bool equals(const PlaylistNode& other) const {
        return (*this == other);
    }

    /**
     * @brief Equality operator - compares audioKey and duration
     */
    bool operator==(const PlaylistNode& other) const {
        return audioKey == other.audioKey && gap == other.gap && durationMs == other.durationMs;
    }
    
    /**
     * @brief Inequality operator
     */
    bool operator!=(const PlaylistNode& other) const {
        return !(*this == other);
    }
};

// ============================================================================
// PLAYLIST
// ============================================================================

/**
 * @brief A named playlist containing a sequence of audio items
 */
struct Playlist {
    std::string name;                   ///< Playlist name
    std::vector<PlaylistNode> nodes;    ///< Ordered list of audio items
    AudioKeyRegistry* keyRegistry;      ///< Registry for looking up entries by name
    
    Playlist() : keyRegistry(nullptr) {}
    
    Playlist(const char* playlistName, AudioKeyRegistry* registry = nullptr)
        : name(playlistName ? playlistName : "")
        , keyRegistry(registry) {}
    
    /**
     * @brief Get the number of items in the playlist
     */
    size_t size() const { return nodes.size(); }
    
    /**
     * @brief Check if the playlist is empty
     */
    bool empty() const { return nodes.empty(); }
    
    /**
     * @brief Clear all items from the playlist
     */
    void clear() { nodes.clear(); }
    
    /**
     * @brief Append an item to the playlist
     */
    void append(const PlaylistNode& node) { nodes.push_back(node); }
    
    /**
     * @brief Append an item by audioKey and duration
     */
    void append(const char* audioKey, unsigned long durationMs = 0) {
        if (audioKey) {
            nodes.emplace_back(audioKey, durationMs);
        }
    }
    
    /**
     * @brief Prepend an item by audioKey and duration (insert at beginning)
     */
    void prepend(const char* audioKey, unsigned long gapDuration = 0, unsigned long durationMs = 0) {
        if (audioKey) {
            nodes.insert(nodes.begin(), PlaylistNode(audioKey, gapDuration, durationMs));
        }
    }
    
    /**
     * @brief Append an item by audioKey name (validates against registry with warning)
     * @param audioKey The audio key name to append
     * @param durationMs Duration in milliseconds
     * @return true always (appends even if not found, just warns)
     */
    bool appendEntry(const char* audioKey, unsigned long durationMs = 0);
    bool appendEntry(PlaylistNode node) {
        return appendEntry(node.audioKey.c_str(), node.durationMs);
    }
    
    /**
     * @brief Prepend an item by audioKey name (validates against registry with warning)
     * @param audioKey The audio key name to prepend
     * @param durationMs Duration in milliseconds
     * @return true always (prepends even if not found, just warns)
     */
    bool prependEntry(const char* audioKey, unsigned long durationMs = 0);
    bool prependEntry(PlaylistNode node) {
        return prependEntry(node.audioKey.c_str(), node.durationMs);
    }

    /**
     * @brief Replace an item at index with a new audioKey (validates against registry with warning)
     * @param index The index to replace
     * @param audioKey The audio key name
     * @param durationMs Duration in milliseconds
     * @return true if replaced, false if index out of bounds
     */
    bool replaceEntry(size_t index, const char* audioKey, unsigned long durationMs = 0);
    bool replaceEntry(size_t index, PlaylistNode node)
    {
        return replaceEntry(index, node.audioKey.c_str(), node.durationMs);
    }
    
    /**
     * @brief Update playlist to match desired nodes
     * 
     * Compares current nodes with desired nodes and efficiently updates the playlist:
     * - Appends new nodes if playlist is too short
     * - Replaces nodes that differ
     * - Removes extra nodes if playlist is too long
     * 
     * @param desiredNodes The desired playlist structure
     */
    void update(const std::vector<PlaylistNode>& desiredNodes) {
        for (size_t i = 0; i < desiredNodes.size(); i++) {
            if (nodes.size() <= i) {
                // Append new node
                appendEntry(desiredNodes[i]);
            } else if (nodes[i].audioKey != desiredNodes[i].audioKey ||
                       nodes[i].durationMs != desiredNodes[i].durationMs) {
                // Replace differing node
                replaceEntry(i, desiredNodes[i]);
            }
            // else: node matches, no action needed
        }
        
        // Remove extra nodes if current playlist is longer
        if (nodes.size() > desiredNodes.size()) {
            nodes.resize(desiredNodes.size());
        }
    }
    
    /**
     * @brief Update playlist with variable parameters
     * 
     * Convenience overload that accepts variable PlaylistNode arguments.
     * Example: playlist.update(node1, node2, node3);
     * 
     * @param args Variable number of PlaylistNode arguments
     */
    template<typename... Args>
    void update(Args&&... args) {
        std::vector<PlaylistNode> desiredNodes = {std::forward<Args>(args)...};
        update(desiredNodes);
    }
};

// ============================================================================
// AUDIO PLAYLIST REGISTRY
// ============================================================================

/**
 * @brief Registry for managing named audio playlists
 * 
 * Works with an AudioKeyRegistry to create and manage playlists.
 * Playlists can be created, deleted, and appended to.
 * Nodes in a playlist reference audioKeys that will be resolved
 * when the playlist is played.
 */
class AudioPlaylistRegistry {
public:
    AudioPlaylistRegistry() = default;
    virtual ~AudioPlaylistRegistry() = default;
    
    // ========================================================================
    // REGISTRY ASSOCIATION
    // ========================================================================
    
    /**
     * @brief Set the AudioKeyRegistry to use for resolving keys
     * @param reg Pointer to the registry (caller retains ownership)
     */
    void setKeyRegistry(AudioKeyRegistry* reg) { keyRegistry = reg; }
    
    /**
     * @brief Get the associated AudioKeyRegistry
     */
    AudioKeyRegistry* getKeyRegistry() const { return keyRegistry; }
    
    // ========================================================================
    // PLAYLIST MANAGEMENT
    // ========================================================================
    
    /**
     * @brief Create a new empty playlist
     * @param name The playlist name
     * @return true if created successfully (false if already exists)
     */
    virtual Playlist* createPlaylist(const char* name, bool overwrite = false);
    
    /**
     * @brief Create and populate a playlist with variable arguments
     * 
     * Convenience method that creates a playlist and immediately populates it
     * with the provided nodes.
     * 
     * @param name The playlist name
     * @param overwrite Whether to overwrite existing playlist
     * @param args Variable number of PlaylistNode arguments
     * @return Pointer to created playlist, or nullptr if failed
     */
    template<typename... Args>
    Playlist* createPlaylist(const char* name, bool overwrite, Args&&... args) {
        Playlist* playlist = createPlaylist(name, overwrite);
        if (playlist) {
            playlist->update(std::forward<Args>(args)...);
        }
        return playlist;
    }
    
    /**
     * @brief Delete a playlist
     * @param name The playlist name
     * @return true if deleted, false if not found
     */
    virtual bool deletePlaylist(const char* name);
    
    /**
     * @brief Clear all playlists
     */
    virtual void clearPlaylists();
    
    /**
     * @brief Check if a playlist exists
     */
    virtual bool hasPlaylist(const char* name) const;
    
    /**
     * @brief Get a playlist by name
     * @return Pointer to the playlist, or nullptr if not found
     */
    virtual const Playlist* getPlaylist(const char* name) const;
    
    /**
     * @brief Get a mutable playlist by name
     * @return Pointer to the playlist, or nullptr if not found
     */
    virtual Playlist* getPlaylistMutable(const char* name);
    
    // ========================================================================
    // PLAYLIST MODIFICATION
    // ========================================================================
    
    /**
     * @brief Append an audioKey to a playlist
     * @param playlistName The playlist to modify
     * @param audioKey The audioKey to append
     * @param durationMs Duration in ms (0 = play to completion)
     * @return true if appended successfully
     */
    virtual bool appendToPlaylist(const char* playlistName, const char* audioKey, unsigned long durationMs = 0);
    
    /**
     * @brief Append a KeyEntry to a playlist
     * @param playlistName The playlist to modify
     * @param entry The entry to append
     * @param durationMs Duration in ms (0 = play to completion)
     * @return true if appended successfully
     */
    virtual bool appendToPlaylist(const char* playlistName, const KeyEntry* entry, unsigned long durationMs = 0);
    
    /**
     * @brief Clear a playlist's contents
     * @param playlistName The playlist to clear
     * @return true if cleared successfully
     */
    virtual bool clearPlaylist(const char* playlistName);
    
    /**
     * @brief Create or replace a playlist with a sequence of audioKeys
     * 
     * This is a convenience method for creating a playlist from scratch.
     * Creates the playlist if it doesn't exist, clears it if it does,
     * then appends all the provided items.
     * 
     * @param playlistName The playlist name
     * @param audioKeys Array of audioKey strings
     * @param durations Array of durations (can be nullptr for all 0)
     * @param count Number of items
     * @return true if created successfully
     */
    virtual bool setPlaylist(const char* playlistName, 
                             const char** audioKeys, 
                             const unsigned long* durations, 
                             size_t count);
    
    // ========================================================================
    // RESOLUTION
    // ========================================================================
    
    /**
     * @brief Resolve all nodes in a playlist to their KeyEntries
     * 
     * This updates the entry pointers in each node by looking up
     * the audioKey in the associated AudioKeyRegistry.
     * 
     * @param playlistName The playlist to resolve
     * @return Number of successfully resolved nodes
     */
    virtual size_t resolvePlaylist(const char* playlistName);
    
    /**
     * @brief Resolve all playlists
     * @return Total number of successfully resolved nodes
     */
    virtual size_t resolveAllPlaylists();
    
    // ========================================================================
    // ITERATION
    // ========================================================================
    
    /**
     * @brief Get the number of registered playlists
     */
    size_t size() const { return playlists.size(); }
    
    /**
     * @brief Get iterator to beginning of playlists
     */
    std::map<std::string, Playlist>::const_iterator begin() const { return playlists.begin(); }
    
    /**
     * @brief Get iterator to end of playlists
     */
    std::map<std::string, Playlist>::const_iterator end() const { return playlists.end(); }
    
protected:
    // The key registry for resolving audioKeys
    AudioKeyRegistry* keyRegistry = nullptr;
    
    // Map of playlist name -> Playlist
    std::map<std::string, Playlist> playlists;
};

// ============================================================================
// GLOBAL ACCESS
// ============================================================================

/**
 * @brief Get the global AudioKeyRegistry instance
 */
AudioKeyRegistry& getAudioKeyRegistry();

/**
 * @brief Get the global AudioPlaylistRegistry instance
 */
AudioPlaylistRegistry& getAudioPlaylistRegistry();
