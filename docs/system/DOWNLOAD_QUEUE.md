# Download Queue — Safety Audit & Architecture

## Overview

`DownloadQueue` (`download_queue.h/.cpp`) manages downloading remote audio
files to SD card. It supports two modes:

| Mode | Where it runs | How it's driven |
|------|---------------|-----------------|
| **Polled (tick)** | Core 1 (main loop) | `audioMaintenanceLoop()` calls `tick()` |
| ~~Async task~~ | ~~Core 0~~ | ~~FreeRTOS task with `start()`~~ |

> **Current policy (2025-03):** Only polled mode is used. The async task is
> disabled because it shares core 0 with Goertzel DTMF detection and causes
> SD card contention, registry race conditions, and Goertzel timing issues.
> See "Known Issues" below.

## Data Flow

```
audioMaintenanceLoop()  [core 1, every ~100ms]
  ├─ isCacheStale()? → downloadAudio() → HTTP GET catalog JSON
  │   → parseAndRegisterAudioFiles() → audioKeyRegistry
  │   → enqueueMissingAudioFilesFromRegistry() → downloadQueue.enqueue()
  │
  └─ downloadQueue.tick()  [rate-limited to DOWNLOAD_QUEUE_CHECK_INTERVAL_MS]
      → _processNext()
        → HTTP GET audio file → write to SD card
        → registerKey() with registryMutex
        → completion callback
```

## Threading Model (Polled Mode)

Everything runs on core 1 (main loop):

```
Core 0:  Goertzel only (no contention)
Core 1:  loop() → audioMaintenanceLoop() → tick() → _processNext()
```

Benefits of polled mode:
- **No SD card contention** — all SD I/O on one core, sequential
- **No registry races** — registry reads and writes are single-threaded
- **No Goertzel starvation** — core 0 is exclusively for DTMF detection
- **Simpler reasoning** — no mutexes needed for correctness

Trade-off: Each `tick()` call blocks the main loop for one download (up to
`HTTP_TIMEOUT_DOWNLOAD_MS` = 30s on timeout, typically 1-5s per file). Audio
playback is unaffected (I2S runs on interrupt/DMA), but hook-switch polling
and DTMF digit dispatch pause during the download. This is acceptable because
downloads only happen at boot or after catalog refresh, not during active calls.

## Internal State

```
Item _items[MAX_DOWNLOAD_QUEUE]   // fixed-size circular array
int  _count                        // valid slots (head of array)
int  _nextIndex                    // first PENDING slot to process
```

Item lifecycle: `EMPTY → PENDING → IN_PROGRESS → DONE | FAILED`

## Mutex Usage

| Mutex | Protects | Held by |
|-------|----------|---------|
| `_mutex` | `_items[]`, `_count`, `_nextIndex` | `enqueue()`, `clear()`, `reset()`, `tick()`, status queries |
| `_regMutex` | `audioKeyRegistry` writes | `_processNext()` after download completes |

In polled mode, `_regMutex` is still acquired for registry writes out of
caution, but since everything is single-threaded on core 1, the lock is
uncontended.

## Error Handling

- **HTTP failure**: Exponential backoff (10s → 20s → 40s → ... → 5min max)
- **SD write failure**: Item marked FAILED, callback invoked with bytes=-1
- **Content-Type mismatch**: File re-saved with correct extension
- **Magic-byte mismatch**: File renamed after download based on header bytes
- **Queue full**: `EnqueueResult::QUEUE_FULL` returned; caller retries on
  next maintenance loop iteration

## Known Issues (Async Task Mode — Currently Disabled)

These issues exist in the `start()` / FreeRTOS task code path but are dormant
since only `tick()` is used:

### CRITICAL: No SD Card Mutex

The ESP32 SD/SD_MMC library is not thread-safe. When the download task ran on
core 0, it performed `SD_OPEN`, `SD_REMOVE`, `SD_RENAME`, `f.write()` while
the main loop on core 1 did `SD_EXISTS`, `SD_OPEN`, `SD_MKDIR`. No mutex
protects these concurrent accesses → potential SD filesystem corruption.

### CRITICAL: Registry Iterator Invalidation

`enqueueMissingAudioFilesFromRegistry()` iterates `audioKeyRegistry` (a
`std::map`) on core 1, while the download task on core 0 calls
`registerKey()` which mutates the map. Iterator invalidation during
`std::map` insertion is undefined behavior → crash.

Additionally, `enqueueMissingAudioFilesFromRegistry()` itself calls
`registerKey()` at line ~339 during its own iteration of the registry —
this is UB even in single-threaded mode. (TODO: fix by collecting
re-registrations into a separate list and applying after iteration.)

### CRITICAL: registryMutex Only Protects One Side

The `registryMutex` is taken by the download task when calling `registerKey()`
but **never taken** by the main loop when iterating or reading the registry.
The mutex only serializes download-task writes against each other, not against
main-loop reads.

### HIGH: Goertzel CPU Starvation

Both Goertzel and the download task were pinned to core 0 at priority 1.
HTTP blocking calls (`http.get()` with 30s timeout) and SD writes (no yield)
could delay Goertzel sample processing, causing missed DTMF detections.

### HIGH: Incomplete File Not Cleaned Up

When `totalBytes <= 0` after `streamBody()`, the item is marked FAILED but
the partially-written file is not deleted from SD card.

### MEDIUM: enqueueMissingAudioFilesFromRegistry() self-modification

The function calls `audioKeyRegistry.registerKey()` while iterating the
registry with a range-based for loop. This is undefined behavior (iterator
invalidation) regardless of threading. Should collect updates and apply
after the loop.

## Key Constants

| Constant | Default | Notes |
|----------|---------|-------|
| `MAX_DOWNLOAD_QUEUE` | 4 | Slots in the circular page |
| `DOWNLOAD_QUEUE_CHECK_INTERVAL_MS` | 1000 | Rate limit for `tick()` |
| `HTTP_TIMEOUT_DOWNLOAD_MS` | 30000 | TCP timeout per download |
| `HTTP_TIMEOUT_CATALOG_MS` | 15000 | TCP timeout for catalog JSON |

## Future Work

- **Async catalog fetch**: The `downloadAudioInternal()` HTTP GET + JSON
  parse currently blocks the main loop. Could be split: HTTP fetch on a
  one-shot task, then post the payload String to core 1 via a FreeRTOS
  queue for parsing and registration. This keeps the blocking HTTP off the
  main loop while keeping registry mutation on core 1.

- **SD card mutex**: If async file downloads are re-enabled, a shared
  `SemaphoreHandle_t` must wrap all SD operations across both cores.

- **Fix self-modification bug**: `enqueueMissingAudioFilesFromRegistry()`
  should not call `registerKey()` during its own iteration.
