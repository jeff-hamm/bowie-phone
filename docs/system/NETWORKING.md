# Bowie Phone Networking

## Overview

All devices communicate over a WireGuard mesh (`10.253.0.0/24`) hosted on the Unraid server. ESP32 microcontrollers connect to WiFi first, then establish a WG tunnel for remote management. Developer machines reach the MCUs through the server as a hub.

### Current Devices

| Device | Hostname | Role | LAN IP | WireGuard IP |
|--------|----------|------|--------|-------------|
| **Unraid server** | hammlet / Jumpdrive | WG hub, DNS proxy, log receiver | 192.168.1.216 | 10.253.0.1 |
| **Bowie Phone** | starfire-phone | ESP32 phone #1 | 192.168.6.128 (WiFi DHCP) | 10.253.0.2 |
| **Jumpbox** | jumpbox | Dev machine (Jeff) | 192.168.1.x (DHCP) | 10.253.0.10 |

### IP Allocation Scheme

The `/24` subnet is partitioned by device role:

| Range | Purpose | Example |
|-------|---------|--------|
| `10.253.0.1` | Server (always `.1`) | hammlet |
| `10.253.0.2` – `.9` | ESP32 microcontrollers | starfire-phone = `.2` |
| `10.253.0.10` – `.19` | Developer machines | jumpbox = `.10` |
| `10.253.0.20` – `.254` | Reserved / future use | — |

When adding a new device, pick the next unused IP in the appropriate range.

```
                        ┌──────────────────┐
                        │   Unraid Server  │
                        │   10.253.0.1     │
                        │   (hammlet)      │
                        │   192.168.1.216  │
                        └──┬───────────┬───┘
                     WG    │           │   WG
               ┌───────────┘           └───────────┐
               ▼                                   ▼
┌──────────────────┐                 ┌──────────────────┐
│  Bowie Phone     │                 │  Jumpbox (Dev)   │
│  10.253.0.2      │                 │  10.253.0.10     │
│  (starfire)      │                 │                  │
│  WiFi: DHCP      │                 │  LAN: DHCP       │
└──────────────────┘                 └──────────────────┘

  To add more:                         To add more:
  10.253.0.3 ─ phone #2               10.253.0.11 ─ dev #2
  10.253.0.4 ─ phone #3               10.253.0.12 ─ dev #3
  ...                                  ...
```

## Device Communication Paths

These paths apply to **any** dev machine or MCU on the mesh — substitute the device's WG IP.

### Dev → ESP32 (management, OTA, telnet)
- **Example:** Jumpbox `10.253.0.10` → WG tunnel → Server `10.253.0.1` → WG tunnel → Bowie Phone `10.253.0.2`
- **Used for:** HTTP OTA upload (port 80), telnet logging (port 23), web API, `/reboot`, `/upload`
- **The dev machine does NOT reach the ESP32 directly over LAN** — they may be on different subnets (e.g. 192.168.1.x vs 192.168.6.x). All management traffic routes through WireGuard via the server hub.
- **Scaling:** Any dev on `10.253.0.10–.19` can reach any MCU on `10.253.0.2–.9` through the server. No additional routing needed — WG peers are all on the same `/24`.

### ESP32 → Server (logs, DNS)
- **Example:** Bowie Phone `10.253.0.2` → WG tunnel → Server `10.253.0.1`
- **DNS:** UDP/TCP port 53 → `esp_wg_proxy` container (dnsmasq) → forwards to 8.8.8.8 / 1.1.1.1
- **Logs:** HTTP POST to `http://10.253.0.1:3000/logs` → `phone-log-receiver` container
- **Log storage:** `/mnt/pool/appdata/phone/logs/<device-id>/<session-id>/` — session-based (see [Remote Log System](#remote-log-system))
- **Scaling:** Each new MCU gets its own log subdirectory. The log receiver auto-creates session directories based on the device ID and boot ID.

### ESP32 → Internet (HTTPS — CURRENTLY BROKEN)
- **Intended path:** ESP32 WiFi → router → internet
- **Actual path after WG init:** ESP32 → WG tunnel → Server → MASQUERADE → internet
- **Problem:** The WireGuard-ESP32 library calls `netif_set_default(wg_netif)` after `wg.begin()`, making the WG interface the default route for ALL traffic. AllowedIPs is hardcoded to `0.0.0.0/0`. External HTTPS (Google Apps Script, Google Drive) attempts go through the tunnel and fail with "connection refused."
- **Server NAT rules exist** (`MASQUERADE 10.253.0.0/24 → br0/vhost0/wlan0`) but forwarding isn't working end-to-end. See [Known Issues](#known-issues).
- **Affects all MCUs** — any ESP32 using the WG library will have the same routing takeover.

## WireGuard Configuration

### Server (`wg0.conf` on Unraid)
- **Location:** `/mnt/pool/appdata/home/.conf/wg0.conf`
- **Interface:** `10.253.0.1/24`, port 51820
- **Current peers:**
  - `starfire-phone` (Bowie Phone): AllowedIPs `10.253.0.2/32`
  - `jumpbox` (Jeff's dev machine): AllowedIPs `10.253.0.10/32`
- **NAT rules (PostUp):**
  - `MASQUERADE -s 10.253.0.0/24 -o br0` (LAN breakout)
  - `MASQUERADE -s 10.253.0.0/24 -o vhost0` (VM bridge)
  - `MASQUERADE -s 10.253.0.0/24 -o wlan0` (WiFi)
  - `MASQUERADE -s 100.64.0.0/10 -d 10.253.0.0/24 -o wg0` (Tailscale → WG bridge)
  - `DNAT --dport 33232 → 100.107.7.10:33232` (port forward to Tailscale node)
  - Legacy port redirect: UDP 41641 → 51820
- **NAT rules are subnet-wide** (`10.253.0.0/24`), so new peers work without modifying iptables.

### ESP32 — Bowie Phone (starfire-phone)
- **Local IP:** `10.253.0.2` (compile-time: `WIREGUARD_LOCAL_IP`)
- **Subnet:** `255.255.255.0` (hardcoded /24)
- **Gateway:** `10.253.0.1` (derived: localAddr with `.1`)
- **Peer endpoint:** `jumprouter.infinitebutts.com:51820`
- **Private key:** compile-time `WIREGUARD_PRIVATE_KEY`
- **After WG connect:** DNS reconfigured to `10.253.0.1` (primary), `8.8.8.8` (fallback)

### Dev Machine — Jumpbox
- **WG IP:** `10.253.0.10`
- **Peer endpoint:** `jumprouter.infinitebutts.com:51820`

## Docker Services on Server

Defined in `/mnt/pool/appdata/phone/docker-compose.yml`:

| Container | Image | Ports | Purpose |
|-----------|-------|-------|---------|
| `phone-log-receiver` | custom (built from `.`) | 3000 | Receives HTTP log posts from ESP32 |
| `esp_wg_proxy` | `jpillora/dnsmasq` | 10.253.0.1:53 (UDP+TCP) | DNS forwarder for WG clients |
| `phone-log-viewer` | `amir20/dozzle` | 8080 (profile: viewer) | Optional log web UI |

### dnsmasq Configuration
- **Location:** `/mnt/pool/appdata/dnsmasq/dnsmasq.conf`
- Forwards to `8.8.8.8` and `1.1.1.1`
- `no-resolv` (ignores `/etc/resolv.conf`)
- Cache size: 1000

## ESP32 Boot Network Sequence

1. **WiFi connect** — tries NVS saved credentials, then `DEFAULT_SSID`, then `FALLBACK_SSID_1` (15s timeout each)
2. **If all fail** → starts AP config portal ("Bowie-Phone-Setup" / `ziggystardust`) on 192.168.4.1
3. **On WiFi success:**
   - DNS set to `8.8.8.8` / `1.1.1.1`
   - WireGuard tunnel init → `wg.begin()` → `netif_set_default(wg_netif)` ← **all traffic now routes through WG**
   - DNS reconfigured to `10.253.0.1` (dnsmasq) / `8.8.8.8` (fallback)
   - Remote logger enabled; pre-connect log buffer flushed (see [Boot Notification Protocol](#boot-notification-protocol))
   - Boot notification POST sent to `10.253.0.1:3000/logs` (once per boot)
   - WiFi callback fires: telnet starts, audio catalog download attempted
4. **OTA + web server start** — port 80 (HTTP API), port 3232 (ArduinoOTA)

## ESP32 HTTP Endpoints

All on port 80, reachable via `http://10.253.0.2/`:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Config UI |
| `/status` | GET | JSON: WiFi IP, RSSI, VPN, heap, partitions, uptime |
| `/api/status` | GET | JSON: AP name, config mode |
| `/reboot` | GET | Triggers `esp_restart()` |
| `/upload` | POST | Multipart file upload → SD card |
| `/vpn/on` | GET | Enable WireGuard |
| `/vpn/off` | GET | Disable WireGuard |
| `/vpn/status` | GET | VPN connection info |
| `/logs` | GET | Recent log output |
| `/save` | POST | Save WiFi credentials |
| `/wifi/clear` | POST | Clear saved WiFi |
| `/wifi/scan` | GET | Scan available networks |
| `/prepareota` | GET | Prepare for OTA (stop audio, free heap) |
| `/update` | POST | HTTP firmware upload |

## Remote Log System

The `phone-log-receiver` server stores logs in a **session-based** directory layout keyed by the `boot_id` the ESP32 generates on power-on.

### Directory Layout

```
<LOG_DIR>/
  <device-id>/
    <session-id>/
      <session-id>__metadata.env   ← created on first POST for this session
      <session-id>_YYYY-MM-DD.log  ← one file per calendar day
      <session-id>_YYYY-MM-DD.log
```

**session-id** = first 12 hex characters of the `boot_id` sent by the ESP32 (e.g. `a3f2c18d4b7e`).

### Session Metadata (`__metadata.env`)

Plain-text `.env` format, written once when the session is first created:

```ini
SESSION_ID=a3f2c18d4b7e
BOOT_ID=a3f2c18d4b7e...
FIRST_SEEN=2025-01-15T08:23:01.000Z
CLIENT_IP=192.168.1.216
TAILSCALE_IP=100.x.x.x
FIRMWARE=1.2.3
BOOT_REASON=power_on
```

### Log Line Format

```
[2025-01-15T08:23:05.123Z] | uptime 4s > application log message here
```

### Boot Marker Line

Written at the top of the log file when a `boot=true` POST is received:

```
[2025-01-15T08:23:01.000Z] | uptime 0s > *** BOOT: firmware=1.2.3 reason=power_on rssi=-62dBm ip=10.253.0.2 ***
```

### Log Rotation

- Controlled by `LOG_RETENTION_DAYS` (default 30) and `LOG_MAX_SIZE_MB` (default 500)
- Rotation walks session subdirs; old day-files are deleted by age, then by size if over limit
- Empty session directories (and their metadata) are cleaned up automatically

### HTTP Endpoints (Server)

| Endpoint | Method | Query Params | Purpose |
|----------|--------|--------------|---------|
| `POST /logs` | POST | — | Receive log batch or boot notification from ESP32 |
| `GET /logs/:device` | GET | `?session=<id>` | Fetch log content (latest session by default) |
| `GET /logs/:device/download/:date` | GET | `?session=<id>` | Download a specific day's log file |
| `GET /devices` | GET | — | List known devices and session count |
| `GET /health` | GET | — | Liveness check |

---

## Boot Notification Protocol

On first flush after WireGuard connects, `remote_logger.cpp` sends a one-time boot POST before any buffered log lines.

### Boot POST Fields

```json
{
  "device":      "bowie-phone-1",
  "boot":        true,
  "boot_id":     "a3f2c18d4b7e...",
  "boot_reason": "power_on",
  "firmware":    "1.2.3",
  "uptime_sec":  0,
  "free_heap":   180000,
  "wifi_ip":     "192.168.6.42",
  "tailscale_ip":"100.x.x.x",
  "rssi":        -62,
  "logs":        ""
}
```

`boot_reason` values come from `esp_reset_reason()`: `power_on`, `software`, `wdt`, `panic`, `brownout`, `external`, `unknown`.

### Regular Log POST Fields

Subsequent log batches are lean — only what the server needs:

```json
{
  "device":     "bowie-phone-1",
  "boot_id":    "a3f2c18d4b7e...",
  "uptime_sec": 45,
  "logs":       "line1\nline2\nline3"
}
```

### Pre-Connect Log Capture

`RemoteLogger` is added to the logger chain at the very start of `setup()` (before WiFi init). When not yet enabled, `write()` buffers into `preConnectBuffer` instead of discarding. Once WireGuard connects and `begin()` is called, the buffer is moved into the outgoing log queue so all boot-time output ships on the first flush — no early logs are lost.

---

## Server Tools

Deployment scripts live in `tools/server/`. Run `install.sh` from the Unraid server (or via SSH from a dev machine) to deploy:

```bash
cd /path/to/repo/tools/server
bash install.sh
```

### What `install.sh` Does

1. Sources `tools/server/.env` to get `APP_DATA` (e.g. `/mnt/pool/appdata/phone`) and `PHOME`
2. For each subdirectory (e.g. `phone-receiver/`, `wireguard-bridge/`):
   - Copies the directory to `$APP_DATA/<name>/`
3. For `phone-receiver`: runs `docker compose up -d --build`
4. For `wireguard-bridge`: creates symlinks — reads the `# @location <path>` header from each file and symlinks that path to the deployed copy

### Symlink Convention (`@location`)

Each `wireguard-bridge/` file has a header comment:
```bash
# @location /etc/wireguard/ts-wg-bridge      ← or any absolute path
```
`install.sh` expands `$PHOME`/`$APP_DATA` variables in the path, creates parent directories, and creates the symlink.

### Route / Peer Management

For SSH-based server administration (add WG peers, edit iptables, manage dnsmasq, restart services), see `.agents/skills/server-routes/SKILL.md`.

---

## Known Issues

### External HTTPS from ESP32 fails after WG init
- **Symptom:** HTTP error -1 ("connection refused") for all external HTTPS (Google, GitHub)
- **Root cause:** `WireGuard-ESP32-Arduino` library hardcodes `netif_set_default(wg_netif)` and AllowedIPs=`0.0.0.0/0`, routing ALL traffic through the tunnel
- **Server side:** NAT MASQUERADE rules exist for `10.253.0.0/24 → br0/vhost0/wlan0`, and `ip_forward=1`. Forwarded traffic routing works (`ip route get ... iif wg0` succeeds), but actual TCP connections from WG-sourced IPs to external hosts fail with "No route to host"
- **Workaround:** Push manifest data TO the device via `POST /upload` (works because that's inbound over WG, not outbound HTTPS)
- **tcpdump finding:** No TCP/443 packets appear on server's wg0 — the ESP32's WG tunnel may not be forwarding the HTTPS SYN packets at all, despite `netif_set_default`

### `esp_restart()` can strand device in AP mode
- **Symptom:** Device reboots but never reconnects to WiFi, goes silent
- **Cause:** Warm reboot doesn't fully reset WiFi radio hardware; 15s per-network timeout may be too short
- **Fix:** Physical power cycle, or connect to "Bowie-Phone-Setup" AP (password: `ziggystardust`) at 192.168.4.1

## Adding a New Device

### New ESP32 / Microcontroller

1. **Pick the next WG IP** in the MCU range (`10.253.0.3`, `.4`, …)
2. **Generate a WireGuard keypair** (`wg genkey | tee privkey | wg pubkey > pubkey`)
3. **Add a `[Peer]` block** to the server's `wg0.conf`:
   ```ini
   [Peer]
   # <device-name>
   PublicKey = <pubkey>
   AllowedIPs = 10.253.0.X/32
   ```
4. **Restart WG on the server** (`wg syncconf wg0 <(wg-quick strip wg0)` or `wg-quick down wg0 && wg-quick up wg0`)
5. **Set compile-time defines** in the new device's `platformio.ini` env:
   - `WIREGUARD_LOCAL_IP` = `10.253.0.X`
   - `WIREGUARD_PRIVATE_KEY` = the generated private key
   - `WIREGUARD_PEER_PUBLIC_KEY` = the server's public key (same for all peers)
6. **No iptables changes needed** — NAT rules target the whole `/24` subnet

### New Developer Machine

1. **Pick the next WG IP** in the dev range (`10.253.0.11`, `.12`, …)
2. **Generate a WireGuard keypair**
3. **Add a `[Peer]` block** to the server's `wg0.conf` (same as above)
4. **Create a local WG config** on the dev machine:
   ```ini
   [Interface]
   PrivateKey = <privkey>
   Address = 10.253.0.X/24

   [Peer]
   PublicKey = <server-pubkey>
   Endpoint = jumprouter.infinitebutts.com:51820
   AllowedIPs = 10.253.0.0/24
   PersistentKeepalive = 25
   ```
5. **Start the tunnel** (`wg-quick up wg0` or WireGuard GUI)
6. **Verify connectivity:** `ping 10.253.0.1` (server), `ping 10.253.0.2` (Bowie Phone)

## Key File Locations

| What | Where |
|------|-------|
| WG server config | `root@192.168.1.216:/mnt/pool/appdata/home/.conf/wg0.conf` |
| dnsmasq config | `root@192.168.1.216:/mnt/pool/appdata/dnsmasq/dnsmasq.conf` |
| Docker compose | `root@192.168.1.216:/mnt/pool/appdata/phone/docker-compose.yml` |
| Device logs | `root@192.168.1.216:/mnt/pool/appdata/phone/logs/<device>/<session>/` |
| Log receiver source | `tools/server/phone-receiver/server.js` |
| Server deploy script | `tools/server/install.sh` |
| WG bridge tools | `tools/server/wireguard-bridge/` |
| Server `.env` | `tools/server/.env` |
| ESP32 remote logger | `src/remote_logger.cpp`, `include/remote_logger.h` |
| ESP32 WG code | `src/tailscale_manager.cpp` |
| ESP32 WiFi code | `src/wifi_manager.cpp` |
| ESP32 HTTPS calls | `src/audio_file_manager.cpp` |
| Build config | `platformio.ini` (env: `bowie-phone-custom` for HTTP OTA) |
| WG library source | `.pio/libdeps/bowie-phone-1/WireGuard-ESP32/src/WireGuard.cpp` |
| Networking doc | `docs/system/NETWORKING.md` |
| Route management skill | `.agents/skills/server-routes/SKILL.md` |
