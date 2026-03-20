# Threading & CPU Architecture

## ESP32 Dual-Core Layout

The ESP32 has two Xtensa LX6 cores. Arduino's `setup()` and `loop()` run on
**core 1** (the default). FreeRTOS tasks can be pinned to either core.

```
┌────────────────────────────────────┐  ┌────────────────────────────────────┐
│            CORE 0                  │  │            CORE 1                  │
│                                    │  │                                    │
│  ┌─────────────────────────────┐   │  │  ┌─────────────────────────────┐   │
│  │ GoertzelTask (FreeRTOS)     │   │  │  │ Arduino loop()             │   │
│  │ Priority 1, 16 KB stack     │   │  │  │                            │   │
│  │                             │   │  │  │ • Audio playback (copy)    │   │
│  │ • I2S RX → GoertzelStream   │   │  │  │ • Hook-switch polling      │   │
│  │ • evaluateBlock()           │   │  │  │ • DTMF digit dispatch      │   │
│  │ • Logger.printf()    ─────────────────▶  Sequence processor        │   │
│  │                             │   │  │  │ • Goertzel mute control    │   │
│  └─────────────────────────────┘   │  │  │ • WiFi / OTA / Telnet     │   │
│                                    │  │  │ • RemoteLogger.loop()      │   │
│  WiFi + lwIP interrupt handlers    │  │  │ • SD card downloads        │   │
│  WireGuard crypto (in WiFi task)   │  │  └─────────────────────────────┘   │
└────────────────────────────────────┘  └────────────────────────────────────┘
```

## FreeRTOS Tasks

| Task | Core | Priority | Stack | Source |
|------|------|----------|-------|--------|
| **GoertzelTask** | 0 | 1 | 16 KB | `dtmf_goertzel.cpp` |
| **Arduino loopTask** | 1 | 1 | 8 KB | framework default |
| **WiFi/lwIP** | 0 | — | — | ESP-IDF internal |

No other custom FreeRTOS tasks are created. Audio-tools timer callbacks run in
ISR context (not a regular task).

## Cross-Core Communication

### Goertzel → Main Loop (safe)

```
Core 0: xQueueSend(goertzelKeyQueue, &digit, 0);  // FreeRTOS queue, non-blocking
Core 1: char k = getGoertzelKey();                 // xQueueReceive, non-blocking
```

Detected DTMF digits are passed through a FreeRTOS queue (size 8) which
prevents digit loss when the main loop is briefly busy (e.g. stopping dialtone).
`goertzelMuted` remains a `volatile bool` — single-bool writes are atomic on
ESP32 (32-bit aligned).

| Variable | Type | Written by | Read by |
|----------|------|-----------|--------|
| `goertzelKeyQueue` | `QueueHandle_t` | Core 0 (Goertzel) | Core 1 (loop) |
| `goertzelMuted` | `volatile bool` | Core 1 (loop) | Core 0 (Goertzel) |

### Logger → RemoteLogger (race condition)

The `Logger` class fans out `write()` calls to all registered `Print` streams,
including `RemoteLogger`. When the Goertzel task calls `Logger.println()` on
core 0, the call chain is:

```
Core 0: Logger.println("🎵 Goertzel DTMF: '5' ...")
  → Logger.write()
    → streams[0]->write()  → Serial  (safe: UART has HW FIFO)
    → streams[1]->write()  → RemoteLogger.write()
      → appends to logBuffer (String)
      → if buffer full: flush() → sendLogs() → s_logClient.post()
```

Meanwhile on core 1:

```
Core 1: RemoteLogger.loop()
  → flush()
    → sendLogs() → s_logClient.post()
    → logBuffer = ""
```

**Unprotected shared state in `RemoteLoggerClass`:**

| Field | Type | Concern |
|-------|------|---------|
| `logBuffer` | `String` | Concurrent append (core 0) + read/clear (core 1) |
| `lineCount` | `int` | Concurrent increment + compare/reset |
| `lastFlushTime` | `unsigned long` | Concurrent read + write |

**Practical risk**: `String` reallocation during `+=` on one core while the
other core is iterating or clearing the same `String` can cause heap corruption
or a panic. The `HTTPClient` underlying `s_logClient` is also not thread-safe —
concurrent `post()` calls from both cores would corrupt TCP state.

### Current Mitigation (incomplete)

The Goertzel task only calls `Logger.printf` in a few places:

1. **Startup/shutdown messages** — one-shot, unlikely to race.
2. **DTMF detection logs** — infrequent (one per key press, ~200 ms apart).
3. **Mute state changes** — rare (on audio start/stop).

Because these messages are short and infrequent, the buffer rarely fills enough
to trigger `flush()` from core 0. The flush threshold (`REMOTE_LOG_BATCH_SIZE`
= 10 lines, or `REMOTE_LOG_BUFFER_SIZE` = 4096 bytes) is high enough that
core 0 almost always just appends without flushing. The real flush happens from
core 1 via `RemoteLogger.loop()` on a 5-second timer.

**This is a latent bug.** Under heavy logging from core 0 (e.g. rapid DTMF
detection, or debug-level logging enabled), the race triggers.

### Recommended Fix

Add a `portMUX_TYPE` spinlock around buffer access in `RemoteLoggerClass`:

```cpp
// In remote_logger.h:
portMUX_TYPE _bufferMux = portMUX_INITIALIZER_UNLOCKED;

// In write():
portENTER_CRITICAL(&_bufferMux);
logBuffer += (char)byte;
if (byte == '\n') lineCount++;
portEXIT_CRITICAL(&_bufferMux);

// In flush() before reading logBuffer:
portENTER_CRITICAL(&_bufferMux);
String snapshot = logBuffer;
logBuffer = "";
lineCount = 0;
portEXIT_CRITICAL(&_bufferMux);
```

This keeps the critical section tiny (string copy + clear) while letting the
HTTP POST happen outside the lock. The `s_logClient.post()` call itself only
runs from the snapshot, so no concurrent TCP access.

The spinlock is appropriate here because the critical section is microseconds
(no blocking calls inside it). A FreeRTOS mutex would work too but adds
unnecessary overhead for such a short hold time.

## HTTP Client Threading

`HTTPClient` (ESP32 Arduino) is **not thread-safe**. Each `HttpClient` wrapper
instance owns its own `HTTPClient` + `WiFiClientSecure`, so instances on
different tasks are fine. The concern is a **single instance** used from
multiple cores — which is the case for `s_logClient` in `remote_logger.cpp`.

With the spinlock fix above, `s_logClient.post()` is only ever called from
within `flush()`, and `flush()` reads from a local snapshot string. But
`flush()` itself can still be entered from both cores simultaneously. The
spinlock guards the buffer but not the HTTP call.

**Full fix**: Also guard `flush()` entry with an atomic flag or ensure
`flush()` only runs from core 1. Since `RemoteLogger.loop()` is the only
periodic caller (from `handleTailscaleLoop()` on core 1), and the
`write()`-triggered flush is the only core-0 path, the cleanest approach is to
**never flush from `write()`**. Instead, just buffer and let `loop()` handle
all flushing:

```cpp
size_t RemoteLoggerClass::write(uint8_t byte) {
    portENTER_CRITICAL(&_bufferMux);
    logBuffer += (char)byte;
    if (byte == '\n') lineCount++;
    portEXIT_CRITICAL(&_bufferMux);
    return 1;  // never call flush() here
}
```

This eliminates all HTTP-from-core-0 paths and confines `s_logClient` usage
to core 1 exclusively.

## Web Queue — Cooperative Chunked HTTP on Core 1

The `WebQueue` is a cooperative state machine that runs entirely on
core 1 via `tick()`. There is no FreeRTOS task — all HTTP I/O and SD writes
happen in the caller's context.

Each `tick()` reads at most one 4 KB chunk (~2-4 ms), keeping the main loop
responsive for audio playback. At 44.1 kHz stereo 16-bit, the I2S DMA buffer
(4 KB) drains in ~23 ms — so a 2-4 ms tick is well within budget.

Three item types are supported:
- **FILE_DL** — download URL → SD card file (audio files)
- **CATALOG_DL** — download URL → String → callback (catalog JSON)
- **POST** — POST body to URL → callback with status (log uploads)

Catalog and POST items have priority over file items.

```
Core 0:  GoertzelTask only
Core 1:  loop() → audioMaintenanceLoop()
           ├─ webQueue.tick()               ← always called, ~0-4 ms
           │   ├─ if active: _streamChunk() ← one 4KB chunk
           │   └─ if idle:   _startNext()   ← open HTTP + file / send POST
           │
           ├─ isCacheStale()? → enqueueCatalog(url, callback)
           └─ queue empty?    → enqueueMissingAudioFilesFromRegistry()

         → RemoteLogger.flush()
           └─ webQueue.enqueuePost(url, json, callback)  ← non-blocking
```

Historical context: The queue previously had a FreeRTOS task mode on core 0
which caused SD contention, registry races, and Goertzel starvation. That
mode has been removed entirely. See [DOWNLOAD_QUEUE.md](DOWNLOAD_QUEUE.md)
for the full architecture and state machine diagram.
