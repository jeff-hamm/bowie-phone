@echo off
REM Bowie Phone Firmware Flasher for Windows
REM This script makes it easy to flash the firmware to your ESP32

echo ========================================
echo Bowie Phone Firmware Flasher
echo ========================================
echo.

REM Check if esptool is installed
python -m esptool version >nul 2>&1
if errorlevel 1 (
    echo ERROR: esptool not found!
    echo.
    echo Please install it with: pip install esptool
    echo.
    pause
    exit /b 1
)

REM Ask for COM port
set /p PORT="Enter your COM port (e.g., COM3, COM4): "

echo.
echo Using port: %PORT%
echo.
echo Step 1/3: Erasing flash...
python -m esptool --port %PORT% erase_flash
if errorlevel 1 (
    echo.
    echo ERROR: Failed to erase flash!
    echo Make sure the device is connected and no other program is using the port.
    pause
    exit /b 1
)

echo.
echo Step 2/3: Flashing partitions...
python -m esptool --chip esp32 --port %PORT% --baud 115200 write_flash -z 0x8000 partitions.bin
if errorlevel 1 (
    echo.
    echo ERROR: Failed to flash partitions!
    pause
    exit /b 1
)

echo.
echo Step 3/3: Flashing firmware...
python -m esptool --chip esp32 --port %PORT% --baud 115200 write_flash -z 0x10000 bowie-phone-firmware.bin
if errorlevel 1 (
    echo.
    echo ERROR: Failed to flash firmware!
    pause
    exit /b 1
)

echo.
echo ========================================
echo SUCCESS! Firmware flashed successfully!
echo ========================================
echo.
echo Next steps:
echo 1. Reset your ESP32 (press RESET button or unplug/replug USB)
echo 2. The device will boot up and create a WiFi network
echo 3. Pick up the phone to hear the dial tone!
echo.
pause
