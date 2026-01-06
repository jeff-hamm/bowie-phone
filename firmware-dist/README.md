# Bowie Phone Firmware Flash Instructions

This folder contains the firmware files for Bowie Phone with **dialtone enabled**.

## What's Included

- `bowie-phone-firmware.bin` - Main firmware file
- `partitions.bin` - Partition table
- `README.md` - This file

## Requirements

- ESP32 device connected via USB
- Python 3.x installed
- esptool.py (install with: `pip install esptool`)

## Flash Instructions

### Method 1: Using esptool.py (Recommended)

1. **Connect your ESP32** to your computer via USB

2. **Find your COM port** (Windows) or device path (Linux/Mac):
   - Windows: Usually `COM3`, `COM4`, etc. (check Device Manager)
   - Linux: Usually `/dev/ttyUSB0` or `/dev/ttyACM0`
   - Mac: Usually `/dev/cu.usbserial-*`

3. **Flash the firmware** (replace `COM3` with your port):

```bash
# Erase flash first (recommended for clean install)
esptool.py --port COM3 erase_flash

# Flash the firmware
esptool.py --chip esp32 --port COM3 --baud 115200 write_flash -z 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 bowie-phone-firmware.bin
```

### Method 2: Firmware only (if bootloader already exists)

If you just want to update the firmware without touching the bootloader:

```bash
esptool.py --chip esp32 --port COM3 --baud 115200 write_flash -z 0x10000 bowie-phone-firmware.bin
```

### Troubleshooting

1. **Port Access Error**: Make sure no other program (PuTTY, Arduino IDE, etc.) is using the serial port
2. **Connection Failed**: Try pressing and holding the BOOT button on your ESP32 while running the flash command
3. **Wrong Port**: Double-check the COM port in Device Manager (Windows) or `ls /dev/tty*` (Linux/Mac)

## After Flashing

1. **Reset the device** (press the RESET button or unplug/replug USB)
2. **Connect to WiFi** - The device will create a WiFi access point called "Bowie-Phone-Setup" if it can't connect to saved WiFi
3. **Test it** - Pick up the phone (go off-hook) and you should hear a dial tone!

## What's New in This Version

- **Dialtone enabled** - You'll now hear a dial tone when going off-hook
- All audio files must be in `.wav` format
- URL streaming mode active

## Configuration

The firmware is pre-configured with:
- WiFi AP: `Bowie-Phone-Setup` / password: `bowie123`
- Default WiFi: `PrettyFlyForaWifi` (will auto-connect if available)
- OTA updates enabled on `bowie-phone.local`
- Google Sheets integration for phone sequences

## Need Help?

Check the full documentation at: https://github.com/[your-repo]/bowie-phone

## Firmware Info

- **Built**: January 6, 2026
- **Version**: 1.0.0
- **Features**: Dialtone, URL Streaming, Tailscale VPN, OTA Updates
