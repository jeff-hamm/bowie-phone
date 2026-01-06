#!/bin/bash
# Bowie Phone Firmware Installer for macOS
# This script handles everything - no prerequisites needed!

set -e

echo "========================================"
echo "Bowie Phone Firmware Installer"
echo "========================================"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
FIRMWARE_URL="https://raw.githubusercontent.com/jeff-hamm/bowie-phone/main/firmware-dist/bowie-phone-firmware.bin"
PARTITIONS_URL="https://raw.githubusercontent.com/jeff-hamm/bowie-phone/main/firmware-dist/partitions.bin"
DEFAULT_PORT="/dev/cu.usbserial-0001"

# Function to print colored output
print_error() {
    echo -e "${RED}ERROR: $1${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

print_info() {
    echo "ℹ $1"
}

# Check if Python 3 is installed
if ! command -v python3 &> /dev/null; then
    print_error "Python 3 is not installed!"
    echo ""
    echo "Python 3 is required to flash the firmware."
    echo ""
    echo "To install Python 3:"
    echo "1. Install Homebrew (if not installed): /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
    echo "2. Then run: brew install python3"
    echo ""
    echo "Or download from: https://www.python.org/downloads/"
    echo ""
    exit 1
fi

print_success "Python 3 found"

# Check if pip is available
if ! command -v pip3 &> /dev/null; then
    print_error "pip3 is not available!"
    echo ""
    echo "Please install pip3:"
    echo "  python3 -m ensurepip --upgrade"
    echo ""
    exit 1
fi

# Check if esptool is installed, if not install it
if ! python3 -m esptool version &> /dev/null; then
    print_warning "esptool not found, installing..."
    pip3 install --user esptool
    if [ $? -eq 0 ]; then
        print_success "esptool installed"
    else
        print_error "Failed to install esptool"
        exit 1
    fi
else
    print_success "esptool found"
fi

# Find available serial ports
echo ""
print_info "Scanning for serial devices..."
PORTS=$(ls /dev/cu.* 2>/dev/null | grep -E "(usbserial|SLAB|wchusbserial)" || true)

if [ -z "$PORTS" ]; then
    print_warning "No USB serial devices found!"
    echo ""
    echo "Please ensure your ESP32 is connected via USB."
    echo ""
    read -p "Enter port manually (e.g., /dev/cu.usbserial-0001): " PORT
else
    echo ""
    echo "Found serial devices:"
    echo "$PORTS" | nl
    echo ""
    
    # Count ports
    PORT_COUNT=$(echo "$PORTS" | wc -l | xargs)
    
    if [ "$PORT_COUNT" -eq 1 ]; then
        PORT=$(echo "$PORTS" | head -1)
        print_info "Auto-selected: $PORT"
    else
        echo "Enter port number (or press Enter for $DEFAULT_PORT):"
        read -p "> " PORT_NUM
        
        if [ -z "$PORT_NUM" ]; then
            PORT="$DEFAULT_PORT"
        else
            PORT=$(echo "$PORTS" | sed -n "${PORT_NUM}p")
        fi
    fi
fi

echo ""
print_info "Using port: $PORT"

# Create temporary directory
TMP_DIR=$(mktemp -d)
cd "$TMP_DIR"

print_info "Temporary directory: $TMP_DIR"

# Download firmware files
echo ""
print_info "Downloading firmware files..."

if curl -L -f "$FIRMWARE_URL" -o firmware.bin; then
    print_success "Downloaded firmware.bin ($(ls -lh firmware.bin | awk '{print $5}'))"
else
    print_error "Failed to download firmware"
    rm -rf "$TMP_DIR"
    exit 1
fi

if curl -L -f "$PARTITIONS_URL" -o partitions.bin; then
    print_success "Downloaded partitions.bin ($(ls -lh partitions.bin | awk '{print $5}'))"
else
    print_error "Failed to download partitions"
    rm -rf "$TMP_DIR"
    exit 1
fi

# Flash the firmware
echo ""
echo "========================================"
echo "Ready to flash firmware to $PORT"
echo "========================================"
echo ""
echo "This will:"
echo "  1. Erase existing flash"
echo "  2. Write partition table"
echo "  3. Write firmware"
echo ""
read -p "Continue? (y/N) " -n 1 -r
echo ""

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    print_warning "Aborted by user"
    rm -rf "$TMP_DIR"
    exit 0
fi

echo ""
print_info "Step 1/3: Erasing flash..."
if python3 -m esptool --port "$PORT" erase_flash; then
    print_success "Flash erased"
else
    print_error "Failed to erase flash!"
    echo ""
    echo "Troubleshooting tips:"
    echo "  - Make sure no other program is using the port"
    echo "  - Try holding the BOOT button while connecting"
    echo "  - Check that the device is properly connected"
    echo ""
    rm -rf "$TMP_DIR"
    exit 1
fi

echo ""
print_info "Step 2/3: Writing partitions..."
if python3 -m esptool --chip esp32 --port "$PORT" --baud 115200 write_flash -z 0x8000 partitions.bin; then
    print_success "Partitions written"
else
    print_error "Failed to write partitions!"
    rm -rf "$TMP_DIR"
    exit 1
fi

echo ""
print_info "Step 3/3: Writing firmware..."
if python3 -m esptool --chip esp32 --port "$PORT" --baud 115200 write_flash -z 0x10000 firmware.bin; then
    print_success "Firmware written"
else
    print_error "Failed to write firmware!"
    rm -rf "$TMP_DIR"
    exit 1
fi

# Cleanup
rm -rf "$TMP_DIR"

echo ""
echo "========================================"
print_success "INSTALLATION COMPLETE!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Reset your ESP32 (press RESET button or unplug/replug USB)"
echo "  2. Monitor serial output with:"
echo "     screen $PORT 115200"
echo "     (Press Ctrl+A then K then Y to exit)"
echo ""
echo "  3. The device will create WiFi network 'Bowie-Phone-Setup'"
echo "  4. Pick up the phone to hear the dial tone!"
echo ""
