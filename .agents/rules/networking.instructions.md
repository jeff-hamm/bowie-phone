---
description: "Use when reading, writing, or debugging networking code: remote logging, HTTP client calls, telnet, WiFi/WireGuard connectivity, phone-receiver server, webhooks, or the phone-home check-in system."
applyTo: "src/remote_logger.cpp,include/remote_logger.h,src/wifi_manager.cpp,include/wifi_manager.h,include/http_utils.h,src/tailscale_manager.cpp,include/tailscale_manager.h,include/phone_home.h,src/phone_home.cpp,tools/server/phone-receiver/**"
---
# Bowie Phone — Networking & Logging Architecture

**Read `docs/system/NETWORKING.md` first** when working on any networking-related task. It contains the full design of:

- WireGuard mesh topology and IP allocation
- All three logging transports (Serial, ESPTelnet, HTTP POST) and their trade-offs
- The phone-receiver server (HTTP log ingest + telnet proxy)
- Boot notification protocol
- Docker service layout
- Proposed persistent TCP log stream (replaces HTTP POST)
- Proposed webhook system (server-side event forwarding)

## Key Design Rules

1. **Logging must never block core 1.** `Logger.write()` fans out to Serial, ESPTelnet, and RemoteLogger synchronously on core 1. Remote network I/O must be async or deferred.

2. **Core 0 is shared with Goertzel.** Any HTTP task on core 0 must run at priority 0 (below Goertzel at priority 1) and respect `HTTP_TIMEOUT_LOG_MS` (2500ms) to stay under WDT.

3. **Telnet suppresses HTTP POST.** When `isTelnetConnected()` returns true, `RemoteLogger::flush()` skips HTTP POST. The same data is already streaming in real-time over telnet.

4. **HTTP POST has dramatic backoff.** Failures back off 30s → 60s → 120s → 240s → cap 5min. Success resets. Dropping logs is always preferable to tripping WDT or starving Goertzel.

5. **Boot notification is always HTTP POST.** Even with a persistent TCP stream, structured boot data (firmware version, reset reason, heap, RSSI) is sent as JSON to `POST /logs` once per boot.

6. **The server is the webhook origin.** The ESP32 never calls external HTTPS endpoints directly (WG routing issue). The phone-receiver server handles all outbound webhook delivery.

## File Map

| File | Role |
|------|------|
| `include/remote_logger.h` | RemoteLoggerClass definition, buffer config constants |
| `src/remote_logger.cpp` | HTTP POST flush logic, backoff, pre-connect buffering |
| `include/http_utils.h` | HttpClient wrapper: sync, async (postAsync), cooperative (chunked) |
| `include/logging.h` | LoggerClass multi-stream Print dispatcher |
| `src/wifi_manager.cpp` | ESPTelnet server, `isTelnetConnected()`, network loop |
| `include/config.h` | Timeout constants, WDT config |
| `tools/server/phone-receiver/server.js` | HTTP log receiver + telnet proxy |
