---
description: "Use when reading, writing, or debugging audio firmware code: audio playback, audio key registration, playlists, tone generators, sequence processing, or the SD card download queue."
applyTo: "include/audio_*.h,include/extended_audio_player.h,include/sequence_processor.h,include/tone_generators.h,src/audio_*.cpp,src/extended_audio_player.cpp,src/sequence_processor.cpp"
---
# Bowie Phone — Audio System Architecture

## Component Map

| Header / Source | Responsibility |
|---|---|
| `audio_file_manager.h/.cpp` | Remote catalog download, SD caching, download queue |
| `audio_key_registry.h/.cpp` | Central key→stream registry (FILE, URL, GENERATOR) |
| `audio_playlist_registry.h/.cpp` | Named playlists of ordered `PlaylistNode` items |
| `extended_audio_player.h/.cpp` | Queue-based playback, stream dispatch, volume |
| `tone_generators.h` | `DualToneGenerator` (dial tone) and `RepeatingToneGenerator` (ringback) |
| `sequence_processor.h/.cpp` | DTMF digit accumulation and sequence→key dispatch |

## Data Flow

```
HTTP JSON catalog → parseAndRegisterAudioFiles()
  → AudioKeyRegistry (all keys)
  → AudioPlaylistRegistry (enriched playlists)
  → download queue (missing local files)

DTMF digit → addDtmfDigit() → hasKey() → playPlaylist() or playAudioKey()
```

## AudioKeyRegistry — Source of Truth

- **Always** register audio resources through `AudioKeyRegistry` before use.
- Key types: `FILE_STREAM` (SD path), `URL_STREAM` (http/https + optional local fallback), `GENERATOR` (synthesized tone).
- Auto-detection in `registerKey(key, path, ext)`: http/https prefix → `URL_STREAM` with auto-generated local path; everything else → `FILE_STREAM`.
- Generators are pre-registered at startup: `"dialtone"` (350 + 440 Hz), `"ringback"` (2 s on / 4 s off).
- `KeyEntry.alternatePath` holds the streaming URL fallback for `FILE_STREAM` keys whose file hasn't been downloaded yet.

## ExtendedAudioPlayer — Playback Rules

- **`playAudioKey(key, durationMs)`** — preferred entry point; auto-checks for a playlist of the same name first.
- **`playAudio(type, key, durationMs)`** — direct play, clears queue.
- **`queueAudio(type, key, durationMs)`** — append or play-if-empty; use for sequential call flows.
- `durationMs = 0` means play to end. Non-zero limits playback (e.g., ringback during call setup).
- Stream dispatch in `ExtendedAudioSource::selectStream()`: `"gen://"` prefix → generator; `http(s)://` → URL stream; else → file stream.
- Volume persisted via `Preferences`; range 0.0–1.0.

## Playlist Enrichment Pattern

When `ENABLE_PLAYLIST_FEATURES` is defined, `registerAudioFile()` auto-builds a playlist per audio key:
1. `previous` nodes (from JSON array)
2. Ringback node (if `ringDuration > 0`)
3. Main audio file node
4. `"click"` sound node (if registered)
5. `next` nodes (from JSON array)

Do **not** manually recreate this structure; modify the JSON catalog or the enrichment logic in `audio_file_manager.cpp`.

## Cache & Download Queue

- SD catalog file: `/audio_files.json`; ETag cached at `/audio_cache_etag.txt`.
- Two-tier staleness: lightweight ETag check every 5 min (`CACHE_CHECK_INTERVAL_MS`), full reload every 24 h (`CACHE_VALIDITY_HOURS`).
- `processAudioDownloadQueue()` must be called from the main loop; it is rate-limited to 1 s intervals and downloads one file per call.
- Queue cap: `MAX_DOWNLOAD_QUEUE` (20). Audio files stored at `/audio/<key>.<ext>`.

## Sequence Processor Rules

- `addDtmfDigit()` performs real-time suffix matching: "9911" triggers on "911", reorders buffer to front, marks ready — **no full-buffer wait needed** for matched keys.
- `*` acts as a manual sequence terminator (excluded from the resulting key string).
- Add new special commands in `isSpecialCommand()`, not inside `addDigitToSequence()`.

## Key Constants (defined in `config.h` unless noted)

| Constant | Default | Notes |
|---|---|---|
| `CACHE_VALIDITY_HOURS` | 24 | Full catalog refresh interval |
| `CACHE_CHECK_INTERVAL_MS` | 300 000 | Lightweight ETag check |
| `MAX_AUDIO_FILES` | 50 | Catalog entry cap |
| `MAX_DOWNLOAD_QUEUE` | 20 | Download queue capacity |
| `DOWNLOAD_QUEUE_CHECK_INTERVAL_MS` | 1 000 | Queue processing rate |
| `AUDIO_FILES_DIR` | `/audio` | SD card audio directory |
| `MAX_HTTP_RESPONSE_SIZE` | 8 192 | HTTP buffer (bytes) |

## Detailed Documentation

See [docs/system/AUDIO_SYSTEM.md](../../docs/system/AUDIO_SYSTEM.md) for the full audio pipeline analysis, component descriptions, and data flow details.
