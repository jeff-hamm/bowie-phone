#!/usr/bin/env python3
"""
Custom PlatformIO upload method for OTA via Unraid WireGuard tunnel
Usage: pio run -e esp32dev_unraid -t upload
"""

Import("env")
import subprocess
import time
import sys
import os
import re

# Extract configuration from build flags
DEVICE_IP = None
OTA_PORT = None
OTA_PASSWORD = None
UNRAID_IP = None

build_flags = env.get("BUILD_FLAGS", [])
for flag in build_flags:
    flag_str = str(flag)
    
    # Extract WireGuard local IP (device IP)
    match = re.search(r'-DWIREGUARD_LOCAL_IP=(?:\\)?\"?([^\"\\s]+)\"?', flag_str)
    if match:
        DEVICE_IP = match.group(1)
    
    # Extract OTA port
    match = re.search(r'-DOTA_PORT=(\d+)', flag_str)
    if match:
        OTA_PORT = match.group(1)
    
    # Extract OTA password
    match = re.search(r'-DOTA_PASSWORD=(?:\\)?\"?([^\"\\s]+)\"?', flag_str)
    if match:
        OTA_PASSWORD = match.group(1)
    
    # Extract Unraid server IP
    match = re.search(r'-DUNRAID_SERVER_IP=(?:\\)?\"?([^\"\\s]+)\"?', flag_str)
    if match:
        UNRAID_IP = match.group(1)

# Fallback to defaults if not found in build flags
if not DEVICE_IP:
    DEVICE_IP = "10.253.0.2"
if not OTA_PORT:
    OTA_PORT = "3232"
if not OTA_PASSWORD:
    OTA_PASSWORD = "bowie-ota-2024"
if not UNRAID_IP:
    UNRAID_IP = "100.86.189.46"

def print_step(step, total, message, color_code="93"):
    """Print a colored step message"""
    print(f"\033[{color_code}m[{step}/{total}] {message}\033[0m")

def print_success(message):
    """Print a success message"""
    print(f"\033[92m✅ {message}\033[0m")

def print_error(message):
    """Print an error message"""
    print(f"\033[91m❌ {message}\033[0m")

def print_warning(message):
    """Print a warning message"""
    print(f"\033[93m⚠️  {message}\033[0m")

def run_command(cmd, shell=True, check=True):
    """Run a shell command and return result"""
    result = subprocess.run(cmd, shell=shell, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise Exception(f"Command failed: {result.stderr}")
    return result

def upload_via_unraid(*args, **kwargs):
    """Custom upload method via Unraid WireGuard tunnel"""
    
    print("\n" + "="*50)
    print("\033[96mBowie Phone OTA Deployment via Unraid\033[0m")
    print("="*50 + "\n")
    
    # Get firmware path from environment
    firmware_path = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    
    if not os.path.exists(firmware_path):
        print_error(f"Firmware not found: {firmware_path}")
        sys.exit(1)
    
    # Step 1: Copy firmware to Unraid
    print_step(1, 4, "Copying firmware to Unraid server...")
    try:
        scp_cmd = f'scp "{firmware_path}" root@{UNRAID_IP}:/tmp/firmware.bin'
        run_command(scp_cmd)
        print_success("Firmware copied to Unraid")
    except Exception as e:
        print_error(f"SCP failed: {e}")
        sys.exit(1)
    
    print()
    
    # Step 2: Prepare device for OTA
    print_step(2, 4, "Preparing device for OTA...")
    try:
        prepare_cmd = f"ssh root@{UNRAID_IP} \"curl -s -m 10 http://{DEVICE_IP}/prepareota\""
        result = run_command(prepare_cmd, check=False)
        if result.returncode == 0:
            print(f"   Response: {result.stdout.strip()}")
            print_success("Device prepared for OTA (via HTTP)")
        else:
            raise Exception(result.stderr)
    except Exception as e:
        print_warning(f"HTTP prepare failed: {e}")
        print("   OTA onStart will attempt preparation automatically")
    
    print()
    
    # Step 3: Wait for device to settle
    print_step(3, 4, "Waiting for device to stabilize...")
    time.sleep(3)
    print_success("Ready")
    print()
    
    # Step 4: Perform OTA update
    print_step(4, 4, "Starting HTTP OTA update via WireGuard...")
    print(f"   Device: {DEVICE_IP}")
    print(f"   Via: {UNRAID_IP}")
    print()
    
    try:
        ota_cmd = f"ssh root@{UNRAID_IP} \"curl -f -m 120 -F 'firmware=@/tmp/firmware.bin' http://{DEVICE_IP}/update\""
        result = run_command(ota_cmd)
        
        print("\n" + "="*50)
        print("\033[92m✅ OTA UPDATE SUCCESSFUL!\033[0m")
        print("="*50 + "\n")
        print("\033[96mDevice should reboot with new firmware in a few seconds.\033[0m\n")
        
    except Exception as e:
        print("\n" + "="*50)
        print("\033[91m❌ OTA UPDATE FAILED\033[0m")
        print("="*50 + "\n")
        print_warning("Troubleshooting:")
        print(f"  1. Check serial output for specific error")
        print(f"  2. Try espota fallback: ssh root@{UNRAID_IP} 'python3 /tmp/espota.py -i {DEVICE_IP} -p {OTA_PORT} -a {OTA_PASSWORD} -f /tmp/firmware.bin'")
        print(f"  3. As last resort, use serial upload: pio run -t upload")
        sys.exit(1)

# Replace the default upload action with our custom one
env.Replace(UPLOADCMD=upload_via_unraid)
