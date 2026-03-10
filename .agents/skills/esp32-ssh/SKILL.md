---
name: esp32-ssh
description: Build, deploy, monitor, and debug Bowie Phone firmware on the mac SSH target. Use when you need end-to-end ESP32 workflow commands for serial or OTA flashing, remote dependency setup, serial monitoring, and deployment troubleshooting.
compatibility: Requires PowerShell on Windows host, SSH access to mac target, and bowie-phone tools/PhoneUtils.ps1.
---

# Bowie Mac Firmware Workflow

Use this skill for repeatable ESP32 firmware operations against the `mac` deploy target in this repository.

## Scope

- Repository: `bowie-phone`
- Main utility script: `tools/PhoneUtils.ps1`
- Typical environment: `bowie-phone-1`
- Typical flash method: `serial`

## Workflow

1. Load utilities and sanity-check access.
   ```powershell
   . .\tools\PhoneUtils.ps1
   ```

2. Build firmware for the target environment.
   ```powershell
   Build-Firmware -Environment bowie-phone-1
   ```
   Optional clean build:
   ```powershell
   Build-Firmware -Environment bowie-phone-1 -Clean
   ```
   Optional extra build flags:
   ```powershell
   Build-Firmware -Environment bowie-phone-1 -BuildArgs "-DRUN_SD_DEBUG_FIRST"
   ```

3. Deploy to mac over SSH.
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial
   ```

4. Deploy without rebuilding when binaries are already fresh.
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -SkipBuild
   ```

5. Use explicit serial port when the default is wrong.
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -SerialPort /dev/tty.usbserial-0001
   ```

6. Monitor serial output after deploy.
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -MonitorAfter
   ```
   Or monitor standalone:
   ```powershell
   Monitor-SerialOutput -Target mac -SerialPort /dev/tty.usbserial-0001
   ```

7. Run OTA deploy via unraid (preferred — Windows can't reach the device directly).
   SSH goes to unraid, which can reach the device at `10.253.0.2` over WireGuard. Device IP resolves automatically.
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target unraid -FlashMethod ota
   Deploy-ToDevice -Environment bowie-phone-1 -Target unraid -FlashMethod ota -SkipBuild
   ```
   Override device IP explicitly if needed:
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target unraid -FlashMethod ota -DeviceIp 10.253.0.2
   ```

8. Stream live device logs from the remote log server (device posts via WireGuard to unraid at `10.253.0.1:3000`).
   ```powershell
   Watch-RemoteLogs
   Watch-RemoteLogs -DeviceId bowie-phone-1
   Watch-RemoteLogs -ServerUrl http://10.253.0.1:3000
   ```

## Debug Playbook

1. If deploy fails at dependencies, run dependency check directly:
   ```powershell
   Ensure-RemoteDependencies -Target mac -FlashMethod serial
   ```

2. If serial flashing fails, verify serial device names on remote mac:
   ```powershell
   ssh jumper@100.111.120.5 "ls -1 /dev/tty.usb* /dev/cu.usb* 2>/dev/null"
   ```

3. If port is busy, clear and retry deploy.
   ```powershell
   Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -SerialPort /dev/tty.usbserial-0001
   ```

4. If deployment output is truncated or disconnected, inspect remote logs:
   ```powershell
   Get-RemoteDeployLog -Target mac
   ```

5. If monitor session disconnects, replay the previous serial session log:
   ```powershell
   Monitor-SerialOutput -Target mac -Replay
   ```

## Common Commands

- Build and deploy in one command:
  ```powershell
  Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial
  ```
- Fire-and-forget deployment:
  ```powershell
  Deploy-ToDevice -Environment bowie-phone-1 -Target mac -FlashMethod serial -NoWait
  ```
- Publish firmware artifacts for web installer:
  ```powershell
  Publish-Firmware -Environment bowie-phone-1
  ```

## Notes

- Preferred mac serial naming for this project is often `/dev/tty.usbserial-0001` or `/dev/cu.usbserial-0001`; use explicit `-SerialPort` when uncertain.
- Keep `platformio.ini` `upload_port` on Windows (`COM3`) unchanged for local workflows; remote mac serial is controlled by `Deploy-ToDevice` parameters.
- The deploy helper auto-attempts remote dependency setup, including Python/esptool bootstrapping paths for headless macOS.
