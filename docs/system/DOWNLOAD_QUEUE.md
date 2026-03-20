# Web Queue — Architecture

## Overview

`WebQueue` (`web_queue.h/.cpp`) manages HTTP GETs and POSTs using a
**cooperative chunked state machine**.  All work runs on core 1 via `tick()`
— there is no FreeRTOS task.

Each `tick()` call does at most one 4 KB chunk of I/O (~2-4 ms), so the main
loop can service audio playback (`audioPlayer.copy()`), hook-switch polling,
and DTMF dispatch between chunks.

A global singleton (`webQueue`) is shared by the audio file manager
(catalog/file downloads) and RemoteLogger (log POST uploads).

## Item Types

| Type | Purpose | Output |
|------|---------|--------|
| `FILE_DL` | Download a URL to an SD card path | SD file + registry update |
| `CATALOG_DL` | Download a URL and accumulate into a `String` | Callback with body |
| `POST` | POST a body to a URL | Callback with HTTP status |

Catalog and POST items have **priority** over file items — `_findNextPending()`
scans for `CATALOG_DL` and `POST` items first.

## Data Flow

```
audioMaintenanceLoop()  [core 1, every loop() iteration]
  ├─ webQueue.tick()                ← always called, ~0-4 ms
  │   ├─ if active: _streamChunk() ← read one 4KB chunk from HTTP
  │   └─ if idle:   _startNext()   ← open HTTP conn + SD file (rate-limited 1s)
  │
  ├─ isCacheStale()? → downloadAudio()
  │   └─ webQueue.enqueueCatalog(url, onCatalogDownloaded)  ← non-blocking
  │
  └─ queue empty && !catalogPending? → enqueueMissingAudioFilesFromRegistry()

RemoteLogger.flush()   [core 1, every 5s]
  └─ webQueue.enqueuePost(serverUrl, json, onLogPostDone)  ← non-blocking
```

### Catalog Download Flow

```
downloadAudio()
  └─ enqueueCatalog(url, onCatalogDownloaded)   ← returns immediately

tick() ... tick() ... tick()   ← chunks accumulate in _bodyAccum

onCatalogDownloaded(success, body, userData)     ← invoked from tick() on core 1
  ├─ parseAndRegisterAudioFiles(body)
  ├─ pruneOrphanedFiles()
  ├─ save JSON + timestamp to SD
  └─ enqueueMissingAudioFilesFromRegistry()
```

### File Download Flow

```
tick() → _startNext()
  ├─ allocate HttpClient on heap
  ├─ http.get(url)                ← blocking ~100-500 ms (TCP connect)
  ├─ http.beginChunkedRead()
  └─ SD_OPEN(localPath, FILE_WRITE)

tick() → _streamChunk()           ← called many times, one 4KB chunk each
  ├─ http.readChunk(buf, 4096)    ← returns 0 if nothing available yet
  ├─ write to SD file
  └─ capture first 12 bytes in _headerBuf for magic detection

tick() → _finishCurrent(true)     ← when bodyDone()
  ├─ close SD file
  ├─ magic-byte verify → rename if extension mismatch
  ├─ registerKey() with _regMutex
  └─ invoke FileCallback
```

## State Machine

```
                ┌──────────┐
                │   IDLE   │ tick() rate-limited to 1s
                └─────┬────┘
                      │ _findNextPending()
                      ▼
                ┌──────────┐
                │ STARTING │ _startNext(): allocate HTTP, connect, open file
                └─────┬────┘
                      │ success
                      ▼
            ┌───────────────────┐
      ┌────▶│    STREAMING      │ _streamChunk(): read one 4KB chunk
      │     └──────┬────────┬───┘
      │            │        │
      │     not done yet    │ bodyDone() or error
      └────────────┘        │
                            ▼
              ┌─────────────────────┐
              │ FINISH / FAIL       │
              │ _finishCurrent()    │
              │ _failCurrent()      │
              └─────────────────────┘
```

## Threading Model

Everything runs on core 1 (main loop) — no mutexes needed for queue state:

```
Core 0:  Goertzel only (no contention)
Core 1:  loop() → audioMaintenanceLoop() → webQueue.tick()
                → RemoteLogger.flush()  → webQueue.enqueuePost()
```

Benefits:
- **No SD card contention** — all SD I/O on one core, sequential
- **No registry races** — registry reads and writes are single-threaded
- **No Goertzel starvation** — core 0 is exclusively for DTMF detection
- **No audio stutter** — each tick() ≤ 4 ms; DMA buffer has ~23 ms at 44.1 kHz

The `_regMutex` is still acquired for registry writes out of caution, but is
always uncontended since everything is single-threaded on core 1.

## Internal State

```
Item _items[MAX_WEB_QUEUE]        // fixed-size array (8 slots)
int  _count                        // valid slots
int  _nextIndex                    // next slot to scan

// Active download (persists across tick() calls):
HttpClient*  _http                 // heap-allocated, owned
File         _sdFile               // open SD file (FILE_DL only)
String       _bodyAccum            // accumulated body (CATALOG_DL / POST response)
int          _activeIdx            // slot index, or -1 if idle
int          _totalBytes           // content-length (-1 if chunked)
uint8_t      _headerBuf[12]       // first 12 bytes for magic detection
```

Item lifecycle: `EMPTY → PENDING → IN_PROGRESS → DONE | FAILED`

## Error Handling

- **HTTP failure / connect timeout**: `_failCurrent()` with exponential
  backoff (10s → 20s → 40s → ... → 5 min max)
- **SD write failure**: Item marked FAILED, callback invoked with bytes ≤ 0
- **Partial file on failure**: Deleted from SD (`SD_REMOVE`) — no orphans
- **Magic-byte mismatch**: File renamed after download based on header bytes
- **Queue full**: `EnqueueResult::QUEUE_FULL` returned; caller retries on
  next maintenance loop iteration
- **Backoff reset**: Resets to 0 after any successful download

## HttpClient Chunked Read API

The `HttpClient` wrapper (`http_utils.h`) provides non-blocking body reads:

| Method | Purpose |
|--------|---------|
| `beginChunkedRead()` | Sets `_bodyRemaining` from Content-Length after `get()` |
| `readChunk(buf, max)` | Reads up to `max` bytes; returns 0 if nothing available, -1 on error/EOF |
| `bodyDone()` | Returns true when all bytes received or connection closed |

The original `streamBody()` template is preserved for use by OTA and other
blocking callers.

## Key Constants

| Constant | Default | Notes |
|----------|---------|-------|
| `MAX_WEB_QUEUE` | 8 | Slots in the array |
| `WEB_QUEUE_IDLE_INTERVAL_MS` | 1000 | Rate limit for idle tick() |
| `WEB_QUEUE_CHUNK_SIZE` | 4096 | Max bytes per readChunk() call |
| `HTTP_TIMEOUT_DOWNLOAD_MS` | 30000 | TCP timeout per download |
| `HTTP_TIMEOUT_CATALOG_MS` | 10000 | TCP timeout for catalog JSON |
| `HTTP_TIMEOUT_SHORT_MS` | 5000 | TCP timeout for POST items |

## Known Issues

### MEDIUM: enqueueMissingAudioFilesFromRegistry() self-modification

The function calls `audioKeyRegistry.registerKey()` while iterating the
registry with a range-based for loop. This is undefined behavior (iterator
invalidation) regardless of threading. Should collect updates and apply
after the loop.

## Historical Context

Prior to the cooperative chunked design, the download queue had a FreeRTOS task
mode (`start()`) that ran on core 0. This caused three critical issues that
motivated the current architecture:

1. **SD card contention** — SD library is not thread-safe across cores
2. **Registry iterator invalidation** — concurrent map mutation from two cores
3. **Goertzel starvation** — blocking HTTP calls delayed DTMF detection

The task mode was removed entirely. All downloads now run cooperatively on
core 1 via `tick()`, eliminating all three issues by design.
