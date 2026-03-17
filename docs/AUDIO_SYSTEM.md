# Audio System Architecture

Comprehensive analysis of the firmware audio pipeline: how audio keys are registered, resolved, cached, downloaded, and played.

```
Remote JSON Catalog (GitHub)
        │
        ▼
┌──────────────────────┐     ┌─────────────────────────┐
│  audio_file_manager  │────▶│   AudioKeyRegistry      │
│                      │     │   (unified map)          │
│  • downloadAudio()   │     │                          │
│  • isCacheStale()    │     │  audioKey → KeyEntry     │
│  • SD card caching   │     │  ┌─────────────────────┐ │
│  • download queue    │     │  │ FILE_STREAM (path)  │ │
└──────────┬───────────┘     │  │ URL_STREAM  (url)   │ │
           │                 │  │ GENERATOR   (ptr)   │ │
           │                 │  └─────────────────────┘ │
           │                 └────────────┬─────────────┘
           │                              │
           │                 ┌────────────▼─────────────┐
           │                 │ AudioPlaylistRegistry     │
           │                 │                           │
           │                 │ playlistName → Playlist   │
           │                 │   [PlaylistNode, ...]     │
           │                 │   (audioKey + duration)   │
           │                 └────────────┬─────────────┘
           │                              │
           │                 ┌────────────▼─────────────┐
           │                 │  ExtendedAudioPlayer      │
           │                 │                           │
           │                 │  • playAudioKey()         │
           │                 │  • playPlaylist()         │
           │                 │  • queueAudio() / next()  │
           │                 │  • Generator streams      │
           │                 │  • URL streaming fallback │
           │                 └──────────────────────────┘
           │
           ▼
┌──────────────────────┐
│  SD Card (/audio/)   │
│                      │
│  /audio_files.json   │  ← cached catalog
│  /audio_cache_*.txt  │  ← staleness metadata
│  /audio/<key>.<ext>  │  ← downloaded audio files
└──────────────────────┘
```

---

## 1. Audio File Manager — `audio_file_manager.h` & `audio_file_manager.cpp`

### `initializeAudioFileManager()`

Main entry point that sets up the entire audio infrastructure:

- Initializes SD card (via `initializeSDCard()`) — handles both SPI and SD_MMC modes with retry logic
- Creates appropriate `AudioSource` object (`AudioSourceSD` or `AudioSourceSDMMC`)
- Attempts to load cached audio catalog from SD card via `loadAudioFilesFromSDCard()`
- If cache loaded, checks staleness with `isCacheStale()` — uses 2-tier caching:
  - **Lightweight check** (every 5 minutes): Validates ETag/lastModified without full download
  - **Full refresh** (every 24 hours): Forces complete re-download
- If cache is NOT stale, queues missing remote audio files via `enqueueMissingAudioFilesFromRegistry()`
- Returns the audio source pointer or nullptr if SD unavailable

### `downloadAudio(int maxRetries, unsigned long retryDelayMs)`

Manages the complete remote catalog download workflow:

1. Checks WiFi connection status
2. Validates cache staleness — skips download if valid
3. Implements retry loop with configurable delays
4. Calls `downloadAudioInternal()` which:
   - Makes HTTP GET to `KNOWN_SEQUENCES_URL` with query params (`?streaming=false/true`)
   - Uses DNS IP caching for WireGuard/VPN scenarios
   - Parses JSON response via `parseAndRegisterAudioFiles()`
   - Performs **mark-and-sweep garbage collection**: removes audioKeys that existed before but aren't in new catalog
   - Registers all audio files with `AudioKeyRegistry`
   - Resolves all playlists with `playlistRegistry.resolveAllPlaylists()`
   - Saves payload to SD card (`/audio_files.json`) with timestamp for cache validation
   - Queues missing audio files for download via `enqueueMissingAudioFilesFromRegistry()`

### `processAudioDownloadQueue()`

Non-blocking queue processor called periodically from main loop:

- Rate-limited to every 1 second (`DOWNLOAD_QUEUE_CHECK_INTERVAL_MS`)
- Calls `processDownloadQueueInternal()` which:
  - Tracks items via global `downloadQueue[]` array
  - Downloads one file at a time with chunked I/O (1KB buffers)
  - Cleans up old files with different extensions when re-downloading
  - Returns true only when a file completes successfully
- Manages queue state:
  - `downloadQueueCount`: Total items ever added
  - `downloadQueueIndex`: Current processing position
  - Items have `AudioDownloadItem` struct with URL, local path, description, extension

### Cache Staleness Checking Logic

Three mechanisms:

1. **`isCacheStale()`**: Checks if refresh needed based on:
   - No cache at all → always stale
   - Cache validity hours exceeded → stale
   - Lightweight check indicates remote changed → stale
2. **`checkRemoteCacheValid()`**: HTTP query for remote lastModified without full download
3. **ETag persistence**: Cached to `/audio_cache_etag.txt` for quick validation

### JSON Parsing & Registration (`parseAndRegisterAudioFiles()`)

- Uses ArduinoJson to parse remote catalog
- Extracts `lastModified` at root level for cache validation
- For each entry (skipping non-objects), creates `AudioFile` struct:
  - `audioKey`: Unique identifier (e.g., "911", "dialtone")
  - `description`, `type`, `data` (file path or URL), `ext`, `gap`, `ringDuration`, `duration`
- Registers via `registerAudioFile()` which delegates to `AudioKeyRegistry::registerKey()`
- **If `ENABLE_PLAYLIST_FEATURES`**: Enriches playlist with ringback, click, previous/next nodes
- Calls optional callback for each file processed

---

## 2. Audio Key Registry — `audio_key_registry.h` & `audio_key_registry.cpp`

### Core Structure: `KeyEntry`

Unified container for any audio resource:

```cpp
struct KeyEntry {
    std::string audioKey;           // Name (e.g., "911")
    AudioStreamType type;           // GENERATOR, URL_STREAM, FILE_STREAM
    std::string path;               // Local file path
    std::string alternatePath;      // Streaming URL fallback
    SoundGenerator<int16_t>* generator; // Generator pointer
    std::string description;        // Human-readable text
    std::string ext;                // File extension
};
```

- Uses **union-like pattern**: either `path` or `generator` is valid based on type
- Supports fallback behavior: local file with URL streaming fallback

### Registration Methods

| Method | Behavior |
|--------|----------|
| `registerKey(audioKey, path, type, alternatePath)` | Explicit registration |
| `registerKey(audioKey, primaryPath, ext)` | Auto-detects: URLs (http/https) → generates local path + stores URL as fallback; local paths → FILE_STREAM directly |
| `registerGenerator(audioKey, generator)` | Registers synthesized tone generators |

### Lookup Methods

- **`hasKey(audioKey)`**: True/false check (checks registry → dynamic callback → resolver callback)
- **`hasKeyWithPrefix(prefix)`**: Matches prefix patterns (e.g., "91" matches "911", "912")
- **`getEntry(audioKey)`**: Returns pointer to `KeyEntry` with all metadata
- **`hasGenerator(audioKey)` / `getGenerator(audioKey)`**: Generator-specific lookups
- **`resolveKey(audioKey)`**: Returns actual resource path (nullptr for generators)
- **`getKeyType(audioKey)`**: Returns the `AudioStreamType`

### Iteration & Inspection

- `std::map`-based iterator interface (`begin()`, `end()`, `size()`)
- `listKeys()`: Logs all registered keys with types, paths, descriptions

### Global Instance

- `getAudioKeyRegistry()`: Singleton-like accessor initialized in `audio_playlist_registry.cpp`
- Pre-registers two tone generators:
  - `"dialtone"`: `DualToneGenerator(350Hz + 440Hz)`
  - `"ringback"`: `RepeatingToneGenerator` wrapping ringback tone (2s on, 4s off cadence)

---

## 3. Extended Audio Player — `extended_audio_player.h` & `extended_audio_player.cpp`

### `ExtendedAudioSource` (Stream Handler)

Implements `AudioTools::AudioSource` interface for multi-type streams:

- **Registry integration**: `setRegistry()` / `getRegistry()`
- **Stream selection**:
  - `selectStream(path)`: Dispatches based on prefix:
    - `"gen://..."` → generator stream
    - `"http://"` or `"https://"` → URL stream
    - else → file stream
  - `setGeneratorStream()`, `setURLStream()`, `setFileStream()`
- **Current state tracking**:
  - `currentType`, `currentKey`, `currentIndex`
  - Returns appropriate stream object (`URLStream`, `GeneratedSoundStream`, `File`)

### `ExtendedAudioPlayer` (Playback Manager)

Main controller with queue-based playback.

#### Initialization

- `begin(AudioStream& output, enableStreaming)`:
  - Creates `volumeStream` wrapper for volume control
  - Initializes `AudioPlayer` with queue support
  - Sets EOF callback for automatic queue advancement
  - Loads volume from persistent storage (`Preferences`)

#### Playback Control

- **`playAudio(type, audioKey, durationMs)`**: Play immediately, clearing queue
  - Resolves audioKey through registry
  - Tries local path first, falls back to streaming URL if enabled
  - Sets duration limit (for limiting ringback, etc.)
  - Invokes event callback

- **`playAudioKey(audioKey, durationMs)`**: Auto-detects stream type
  - Checks if playlist exists with that name (if `ENABLE_PLAYLIST_FEATURES`)
  - Falls back to direct audio key playback

- **`playPlaylist(playlistName)`**: Queues all items from named playlist

#### Queue Management

- **`queueAudio(type, audioKey, durationMs)`**: Adds to queue or plays if empty
  - Uses `std::vector<QueuedAudioItem>` queue
  - `QueuedAudioItem` stores stream type, key name, and duration limit

- **`next()`**: Manual advancement to next queued item
  - Called automatically via EOF callback or when current stream ends
  - Pops from front of queue and starts stream
  - Returns false if queue empty (playback stops)

#### Stream Resolution & Fallback

- `resolveAudioKey()`: Maps key to actual resource:
  - Generators → create `"gen://name"` pseudo-URL
  - Registry resolution → returns path
  - Pass-through for URLs
- Duration limit enforcement:
  - `copy()` method checks elapsed time vs `currentDurationMs`
  - Calls `onStreamEnd()` when limit reached
  - Allows playlist items to limit ringback duration independent of audio length

#### Volume & State

- `setVolume(float volume)`: 0.0–1.0, persists to storage
- `isAudioKeyPlaying(audioKey)`: Checks current playing key
- `getCurrentAudioKey()`: Returns currently playing key name

---

## 4. Tone Generators — `tone_generators.h`

### `DualToneGenerator`

Synthesizes two simultaneous sine waves:

```cpp
DualToneGenerator(freq1=350.0f, freq2=440.0f, amplitude=16000.0f)
```

- **Phase-based synthesis**: Maintains phase for each tone independently
- **Phase increment**: Pre-calculated as `2π * freq / sampleRate` every sample
- **Output**: `int16_t readSample()` sums both tones, wraps phase at 2π
- **Initialization**: `begin(AudioInfo)` recalculates phase increments for new sample rate

### `RepeatingToneGenerator<T>`

Wraps a tone generator to add silence periods:

```cpp
RepeatingToneGenerator(generator, toneMs, silenceMs)
```

- Alternates between:
  - Tone period: outputs from wrapped generator for `toneMs` milliseconds
  - Silence period: outputs zeros for `silenceMs` milliseconds
- **Sample counting**: Tracks position within current period
- **State machine**: `m_inTonePeriod` boolean tracks which state
- **Use case**: Ringback (2s tone, 4s silence), busy signal patterns, etc.

---

## 5. Audio Playlist Registry — `audio_playlist_registry.h` & `audio_playlist_registry.cpp`

### `PlaylistNode`

Single item in a playlist:

```cpp
struct PlaylistNode {
    std::string audioKey;       // Key to play
    unsigned long gap;          // Gap in ms before this item (ringback delay)
    unsigned long durationMs;   // Duration limit (0 = play to end)
};
```

### `Playlist`

Ordered sequence of nodes:

- Associates with `AudioKeyRegistry` for validation
- Methods:
  - `append()` / `prepend()` / `replaceEntry()`: Modify sequence
  - `update(desiredNodes)`: Efficient comparison-based update
  - `appendEntry()` / `prependEntry()`: With registry validation

### `AudioPlaylistRegistry`

Manages named playlists:

- **Creation**: `createPlaylist(name, overwrite)`
- **Storage**: `std::map<std::string, Playlist> playlists`
- **Modification**: `appendToPlaylist()`, `setPlaylist()`, `clearPlaylist()`
- **Resolution**: `resolvePlaylist()` validates all keys exist in registry
- **Global instance**: `getAudioPlaylistRegistry()`

### Enriched Playlist Creation (in `audio_file_manager.cpp`)

When parsing remote catalog, playlists are auto-created with structure:

1. Previous items from `"previous"` JSON array
2. Ringback (if `ringDuration > 0`)
3. Main audio file
4. Click sound (if registered)
5. Next items from `"next"` JSON array

This allows complex call flows in a single playlist reference.

---

## 6. Sequence Processor — `sequence_processor.h` & `sequence_processor.cpp`

### DTMF Sequence Management

- **Global buffer**: `dtmfSequence[MAX_SEQUENCE_LENGTH+1]`
- **State**: `sequenceIndex`, `sequenceReady` flag, `lastDigitTime`

### `readDTMFSequence()`

Main entry point called periodically:

- Returns true when a complete sequence is ready to process
- Calls `processNumberSequence()` internally
- Resets buffer after processing

### `addDtmfDigit(char digit)`

Simulated input for testing:

- Validates digit (0-9, *, #)
- Calls `addDigitToSequence()` which:
  - Stops dial tone on first digit
  - Treats `*` as sequence completion (excluding the `*`)
  - Adds digit to buffer
  - **Real-time matching**: Checks all suffixes of current buffer against registry
    - E.g., "9911" finds match on "911", moves to front, returns ready
  - Marks sequence ready when buffer full or match found

### `processNumberSequence()`

Decision tree for complete sequences:

1. Check if special command via `isSpecialCommand()`
2. Check if audio key exists in registry via `hasKey(sequence)`
3. If exists:
   - Try `playPlaylist(sequence)` if `ENABLE_PLAYLIST_FEATURES`
   - Fall back to `playAudioKey(sequence)`
4. If not: Call `processUnknownSequence()`

### `getSequence()` / `isReadingSequence()` / `isSequenceReady()`

Query methods for current state.

---

## Key Integration Points

### Data Flow: Remote to Playback

1. **`downloadAudio()`** → HTTP download of catalog JSON
2. **`parseAndRegisterAudioFiles()`** → Registers keys + creates playlists
3. **`processAudioDownloadQueue()`** → Downloads actual audio files
4. **`playAudioKey()` / `playPlaylist()`** → Triggers playback via `ExtendedAudioPlayer`

### Stream Type Detection

- Registry entry type stored in `KeyEntry.type`
- Auto-detection: URLs → `URL_STREAM`, generators → `GENERATOR`, else → `FILE_STREAM`
- `ExtendedAudioSource::selectStream()` dispatches based on path prefix

### Fallback Chain

1. Try local file playback
2. If fails + streaming enabled: Try streaming URL from `KeyEntry.alternatePath`
3. If both fail: Log error, return false

### Registry as Source of Truth

All other components query registry:

- `ExtendedAudioPlayer` resolves keys through registry
- `ExtendedAudioSource` looks up generators in registry
- `Playlist` validates nodes against registry
- `sequence_processor` checks `hasKey()` before processing

This architecture decouples audio resources from playback logic, enables seamless fallback between local caching and streaming, and supports complex enriched playlists with automatically managed ringback and click sounds.

---

## Configuration Constants

| Constant | Default | Description |
|----------|---------|-------------|
| `CACHE_VALIDITY_HOURS` | 24 | Full cache refresh interval |
| `CACHE_CHECK_INTERVAL_MS` | 300000 (5 min) | Lightweight ETag check interval |
| `MAX_AUDIO_FILES` | 50 | Maximum catalog entries |
| `MAX_DOWNLOAD_QUEUE` | 20 | Maximum concurrent download queue items |
| `DOWNLOAD_QUEUE_CHECK_INTERVAL_MS` | 1000 | Download queue processing rate |
| `AUDIO_FILES_DIR` | `/audio` | SD card directory for cached audio |
| `MAX_HTTP_RESPONSE_SIZE` | 8192 | Maximum HTTP response buffer |
