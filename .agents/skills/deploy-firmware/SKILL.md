---
name: deploy-firmware
description: Build, deploy, monitor, and debug Bowie Phone firmware. Covers PlatformIO direct OTA (preferred), PhoneUtils SSH-based serial/OTA workflows, remote log streaming, and deployment troubleshooting.
compatibility: Requires PowerShell on Windows host. PlatformIO OTA needs WireGuard active. SSH workflows need mac target access and tools/PhoneUtils.ps1.
---

# Bowie Phone Firmware Workflow

## Scope

- Repository: `bowie-phone`
- PlatformIO config: `platformio.ini`
- Helper script: `tools/PhoneUtils.ps1` (SSH-based workflows)

## PlatformIO Environments

| Environment | Purpose | Upload Method |
|-------------|---------|---------------|
| `bowie-phone-1` | Base build env, local USB serial (`COM3`) | USB serial |
| `bowie-phone-ota` | OTA via espota to `10.253.0.2:3232` | espota |
| `bowie-phone-custom` | **Preferred OTA** — HTTP OTA via `tools/http_ota_upload.py` | custom HTTP |
| `diag` | Diagnostics build (no audio libs) | USB serial |
| `test-bowie-integration` | Unity integration tests | USB serial |

## Agent Workflow — After Upload

After any build+upload, **always start a monitor session in a background terminal** so firmware output is visible for testing and validation:

```powershell
~\.platformio\penv\Scripts\pio.exe device monitor --environment bowie-phone-custom
```

Run this as a **background terminal** (`isBackground: true`) immediately after upload completes, then use `get_terminal_output` to read the live logs and confirm the change behaved as expected.

## Quick Start — PlatformIO OTA (Preferred)

The `bowie-phone-custom` environment uses HTTP OTA upload via `tools/http_ota_upload.py`.
Requires WireGuard active on Windows so the device at `10.253.0.2` is reachable.

1. **Build only:**
   ```powershell
   ~\.platformio\penv\Scripts\platformio.exe run --environment bowie-phone-custom
   ```

2. **Build + upload (OTA):**
   ```powershell
   ~\.platformio\penv\Scripts\platformio.exe run --target upload --environment bowie-phone-custom
   ```

3. **Upload + monitor** (build, flash OTA, then stream telnet logs from device):
   ```powershell
   ~\.platformio\penv\Scripts\platformio.exe run --target upload --target monitor --environment bowie-phone-custom
   ```
   Monitor connects via telnet to `10.253.0.2:23` (configured as `monitor_port = socket://10.253.0.2:23`).

4. **Monitor only** (no build/upload):
   ```powershell
   ~\.platformio\penv\Scripts\platformio.exe device monitor --environment bowie-phone-custom
   ```

5. **Stream remote logs** (device posts via WireGuard to unraid log server):
   ```powershell
   . .\tools\PhoneUtils.ps1
   Watch-RemoteLogs
   Watch-RemoteLogs -DeviceId bowie-phone-1
   Watch-RemoteLogs -ServerUrl http://10.253.0.1:3000
   ```

## PhoneUtils SSH Workflows

For serial flashing via SSH to a remote mac, or SSH-tunneled OTA.

1. Load utilities:
   ```powershell
   . .\tools\PhoneUtils.ps1
   ```

2. Build firmware:
   ```powershell
   Build-Firmware -Environment bowie-phone-1
   ```
   Clean build:
   ```powershell
   Build-Firmware -Environment bowie-phone-1 -Clean
   ```
   Extra build flags:
   ```powershell
   Build-Firmware -Environment bowie-phone-1 -BuildArgs "-DRUN_SD_DEBUG_FIRST"
   ```

3. Deploy to mac over SSH (serial):
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial
   ```

4. Deploy without rebuilding:
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -SkipBuild
   ```

5. Explicit serial port:
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -SerialPort /dev/tty.usbserial-0001
   ```

6. Monitor serial output after deploy (starts automatically, use `-NoMonitor` to suppress):
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial
   ```
   Standalone monitor:
   ```powershell
   Monitor-SerialOutput -Target mac -SerialPort /dev/tty.usbserial-0001
   ```

7. SSH-tunneled OTA via bowie-phone target (routes through unraid to `10.253.0.2`):
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target bowie-phone
   Deploy-ToDevice -Environment bowie-phone-1 -Target bowie-phone -SkipBuild
   ```
   Fire-and-forget:
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target bowie-phone -NoWait
   ```

## Debug Playbook

1. If deploy fails at dependencies:
   ```powershell
   Ensure-RemoteDependencies -Target mac -FlashMethod serial
   ```

2. Verify serial devices on remote mac:
   ```powershell
   ssh jumper@100.111.120.5 "ls -1 /dev/tty.usb* /dev/cu.usb* 2>/dev/null"
   ```

3. If port is busy, specify explicitly:
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -SerialPort /dev/tty.usbserial-0001
   ```

4. Inspect remote deploy logs:
   ```powershell
   Get-RemoteDeployLog -Target mac
   ```

5. Replay previous serial session:
   ```powershell
   Monitor-SerialOutput -Target mac -Replay
   ```

## Common Commands

| Task | Command |
|------|---------|
| Build + OTA (preferred) | `~\.platformio\penv\Scripts\platformio.exe run --target upload --environment bowie-phone-custom` |
| Build + OTA + monitor | `~\.platformio\penv\Scripts\platformio.exe run --target upload --target monitor --environment bowie-phone-custom` |
| Build only | `~\.platformio\penv\Scripts\platformio.exe run --environment bowie-phone-custom` |
| Monitor only | `~\.platformio\penv\Scripts\platformio.exe device monitor --environment bowie-phone-custom` |
| Serial deploy (SSH) | `Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial` |
| Remote logs | `Watch-RemoteLogs -DeviceId bowie-phone-1` |
| Publish firmware | `Publish-Firmware -Environment bowie-phone-1` |

## Notes

- **Preferred workflow**: `pio run -e bowie-phone-custom -t upload -t monitor` for build+OTA+logs in one command.
- WireGuard must be active on Windows for any OTA or telnet monitor to reach `10.253.0.2`.
- `bowie-phone-custom` extends `bowie-phone-ota` but uses custom HTTP upload script instead of espota.
- Keep `upload_port = COM3` on base env for local USB workflows; OTA envs override this.
- Mac serial naming is typically `/dev/tty.usbserial-0001` or `/dev/cu.usbserial-0001`.
- The PhoneUtils deploy helper auto-bootstraps remote Python/esptool on headless macOS.
