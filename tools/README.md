# Bowie Phone Deployment Tools

This directory contains deployment and management scripts for the Bowie Phone ESP32 firmware.

## PhoneUtils.ps1 - Unified PowerShell Toolkit

**`PhoneUtils.ps1`** - All-in-one PowerShell module for Bowie Phone development

### Auto-Loading

When you open a new terminal in this workspace, PhoneUtils is automatically loaded via `.vscode/settings.json`. All functions are immediately available.

### Core Functions

#### Deployment
- **`Deploy-ViaSsh`** - Deploy firmware to remote machine via SSH
- **`Build-Firmware`** - Build firmware with PlatformIO
- **`Watch-SerialOutput`** - Monitor device serial output
- **`Get-RemoteDeployLog`** - Retrieve deployment logs

#### Version Management
- **`Bump-Version`** - Increment firmware version across all files
- **`Publish-Firmware`** - Build and publish firmware to web installer

### Features

- ✅ **Resilient deployment** - Continues even if connection drops
- ✅ **Multiple WiFi fallback** - Compile-time and deployment-time configuration
- ✅ **Auto-install esptool** - Downloads and installs if missing
- ✅ **Port cleanup** - Automatically kills blocking processes
- ✅ **Serial monitoring** - Built-in screen-based monitoring
- ✅ **Real-time logs** - Live deployment progress
- ✅ **Two targets** - Mac or Unraid deployment hosts
- ✅ **Version management** - Automated version bumping
- ✅ **Web installer** - Publish firmware for browser-based flashing

### Quick Start

```powershell
# Functions are auto-loaded when you open a terminal

# Build and deploy
Deploy-ViaSsh -MonitorAfter

# Deploy to specific target
Deploy-ViaSsh -Target unraid

# Deploy with custom WiFi credentials
Deploy-ViaSsh -WifiSsid "MyNetwork" -WifiPassword "secret123"

# Bump version and publish
Bump-Version
Publish-Firmware

# Just monitor serial
Watch-SerialOutput
```

### Deployment Parameters

When calling `Deploy-ViaSsh`:

| Parameter | Description | Default |
|-----------|-------------|---------|
| `-Environment` | PlatformIO environment to build | `bowie-phone-1` |
| `-Target` | Deployment target (`mac` or `unraid`) | `mac` |
| `-FlashMethod` | Flash method (`serial` or `ota`) | `serial` |
| `-SerialPort` | Serial port on remote machine | `/dev/cu.usbserial-0001` (mac) |
| `-WifiSsid` | WiFi SSID for deployment-time config | _(uses compile-time)_ |
| `-WifiPassword` | WiFi password | _(uses compile-time)_ |
| `-SkipBuild` | Skip build step | `$false` |
| `-MonitorAfter` | Monitor serial after flash | `$false` |
| `-Clean` | Clean before build | `$false` |
| `-NoWait` | Don't wait for completion | `$false` |

### Version Management

```powershell
# Auto-increment patch version (1.0.0 -> 1.0.1)
Bump-Version

# Set specific version
Bump-Version -NewVersion "2.0.0"
```

Updates version in:
- `platformio.ini` (FIRMWARE_VERSION)
- `docs/firmware/manifest.json`
- `docs/update.html`

### Publish Firmware

```powershell
# Build and publish to web installer
Publish-Firmware

# Publish specific environment
Publish-Firmware -Environment dream-phone-1

# Skip build, just copy binaries
Publish-Firmware -SkipBuild

# Override version
Publish-Firmware -Version "1.2.3"
```

Copies firmware to `docs/firmware/` and updates `manifest.json` for the web-based ESP installer.

### Individual Function Usage

All functions are auto-loaded when you open a terminal:

```powershell
# Build only
Build-Firmware -Environment bowie-phone-1

# Monitor serial anytime
Watch-SerialOutput -Target mac

# Get deployment logs
Get-RemoteDeployLog -Target mac
```

### Target Configuration

Configured in `$Script:Config.Targets`:

**Mac:**
- Host: `jumper@100.111.120.5`
- Serial: `/dev/cu.usbserial-0001`
- esptool: `/tmp/esptool-macos-arm64/esptool`

**Unraid:**
- Host: `root@100.86.189.46`
- Serial: `/dev/ttyUSB0`
- esptool: `esptool.py`

### Serial Monitoring

Interactive monitoring with `screen`:

```powershell
# Monitor with device reset
Watch-SerialOutput

# Monitor without reset
Watch-SerialOutput -NoReset

# Custom baud rate
Watch-SerialOutput -BaudRate 74880
```

**Controls:**
- `Ctrl+A` then `K` - Kill screen session
- `Ctrl+C` - Disconnect (leaves screen running)

### WiFi Configuration

**Compile-time (platformio.ini):**
```ini
build_flags =
    -DDEFAULT_SSID=\"HMSHoneypot\"
    -DDEFAULT_PASSWORD=\"cutebutdangerous\"
    -DFALLBACK_SSID_1=\"House Atreides\"
    -DFALLBACK_PASSWORD_1=\"desertpower\"
```

**Deployment-time (via config portal API):**
```powershell
.\tools\deploy_via_ssh.ps1 -WifiSsid "NewNetwork" -WifiPassword "newpass"
```

### How It Works

1. **SSH Connection** - Verifies connection to deployment host
2. **esptool Check** - Auto-installs if missing (uses `install_esptool.sh`)
3. **Port Cleanup** - Kills any processes blocking the serial port
4. **Build** - Compiles firmware with PlatformIO (unless `-SkipBuild`)
5. **Upload** - Transfers firmware files via SCP
6. **Deploy** - Runs autonomous bash script on remote host
7. **Flash** - Uses esptool to flash firmware
8. **WiFi Config** _(optional)_ - Configures via config portal API
9. **Monitor** _(optional)_ - Opens screen session for serial output

### Deployment Script

The PowerShell script generates and uploads an autonomous bash deployment script to the remote machine. This script:

- Runs with `nohup` (survives connection drops)
- Logs to `/tmp/fw_deploy.log`
- Verifies firmware files
- Flashes via esptool
- Optionally configures WiFi
- Returns `RESULT:SUCCESS` on completion

### Helper Scripts

**`install_esptool.sh`** - Standalone esptool installer
- Detects platform (macOS arm64/amd64, Linux amd64)
- Downloads from GitHub releases
- Installs to `/tmp/esptool-<platform>/`

## Other Tools

**`deploy-phonehome.ps1`** - Phone home deployment
- Publishes firmware to phone-home server

**`send_command.py`** - Send commands to device
- Serial/network command utility

**`serial_test.py`** - Serial port testing
- Debug serial communication

**`log-server/`** - Remote logging server
- Docker-based log collection service

## Requirements

### Windows (Local)
- PowerShell 5.1+
- SSH client (OpenSSH)
- SCP/SFTP support

### Remote (Mac/Unraid)
- SSH server
- `curl` and `unzip` (for esptool install)
- `screen` (for serial monitoring)
- Serial port access permissions

### PlatformIO
- PlatformIO Core installed
- ESP32 platform and dependencies
- Located at: `~/.platformio/penv/Scripts/platformio.exe`

## SSH Setup

Ensure passwordless SSH access:

```bash
# Generate key if needed
ssh-keygen -t ed25519

# Copy to remote
ssh-copy-id jumper@100.111.120.5
ssh-copy-id root@100.86.189.46

# Test connection
ssh jumper@100.111.120.5 "echo OK"
```

## Troubleshooting

### Build Fails
```powershell
# Clean build and deploy
Deploy-ViaSsh -Clean

# Or just clean build
Build-Firmware -Clean

# Check build log for full details
Get-Content build.log

# Check for specific errors
Select-String -Path build.log -Pattern "error"
```

Build errors are saved to `build.log` in the project root with full output.

### Port Busy
The script automatically kills blocking processes. If issues persist:
```bash
# On remote machine
lsof /dev/cu.usbserial-0001
pkill screen
```

### esptool Missing
Run manually on remote:
```bash
curl -L https://raw.githubusercontent.com/[...]/install_esptool.sh | bash
```

### SSH Connection Fails
```powershell
# Test SSH
ssh jumper@100.111.120.5 "echo OK"

# Check SSH config
cat ~/.ssh/config
```

### Serial Output Garbled
- Initial bootloader runs at 74880 baud (normal - shows as garbled)
- Application runs at 115200 baud (clean logs)
- Use `Watch-SerialOutput -BaudRate 74880` to see bootloader

### Deployment Logs
```powershell
# View remote deployment log (function auto-loaded)
Get-RemoteDeployLog -Target mac

# Or direct SSH
ssh jumper@100.111.120.5 "cat /tmp/fw_deploy.log"
```

## Examples

### Development Workflow
```powershell
# Functions are auto-loaded in new terminals

# Build and flash
Build-Firmware
Deploy-ViaSsh -MonitorAfter

# Just monitor after changes
Watch-SerialOutput
```

### Version and Publish Workflow
```powershell
# Bump version
Bump-Version  # 1.0.0 -> 1.0.1

# Build and publish to web installer
Publish-Firmware

# Deploy to device
Deploy-ViaSsh -MonitorAfter
```

### Production Deployment
```powershell
# Clean build with monitoring
Deploy-ViaSsh -Clean -MonitorAfter

# Fire and forget to production
Deploy-ViaSsh -Target unraid -NoWait
```

### WiFi Reconfiguration
```powershell
# Deploy with new WiFi credentials
Deploy-ViaSsh `
    -SkipBuild `
    -WifiSsid "NewNetwork" `
    -WifiPassword "newpassword" `
    -MonitorAfter
```

### Remote Debugging
```powershell
# Monitor device (functions auto-loaded)
Watch-SerialOutput -NoReset

# In screen session, type commands:
# scan - List WiFi networks
# state - Show device state
# help - Show all commands
```

## Serial Commands

Once monitoring with `Watch-SerialOutput`, type these commands:

- `scan` - Scan for WiFi networks
- `state` - Show current state
- `dns` - Test DNS resolution
- `help` - List all commands
- `*123#` - System status (via DTMF)
- `*789#` - Reboot device

## Notes

- Deployment is resilient - runs autonomously on remote host
- Connection drops don't interrupt flashing
- Logs saved to `/tmp/fw_deploy.log` on remote
- Serial port automatically cleaned before flash
- esptool auto-installed if missing
- WiFi credentials can be set at deploy-time or compile-time
- Multiple fallback WiFi SSIDs supported
