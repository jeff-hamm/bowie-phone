#!/bin/bash
# ============================================================================
# ESP32 Firmware Deployment Script (Autonomous)
# Generated: @@TIMESTAMP@@
# ============================================================================
# This script runs autonomously on the remote machine.
# Even if the originating connection drops, deployment continues.
# Logs are written to: @@LOGFILE@@
# ============================================================================

set -e

LOGFILE="@@LOGFILE@@"
STAGING="@@STAGING@@"
PORT="@@PORT@@"
FLASH_METHOD="@@FLASH_METHOD@@"

# Logging function
log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') | $1" | tee -a "$LOGFILE"
}

# Start fresh log
echo "============================================================================" > "$LOGFILE"
log "ESP32 Deployment Starting"
log "============================================================================"

# ============================================================================
# STEP 1: Verify files
# ============================================================================
log "STEP 1: Verifying firmware files"

for file in firmware.bin bootloader.bin partitions.bin; do
    if [ ! -f "$STAGING/$file" ]; then
        log "ERROR: Missing file: $STAGING/$file"
        echo "RESULT:MISSING_FILES"
        exit 1
    fi
    SIZE=$(ls -l "$STAGING/$file" | awk '{print $5}')
    log "  $file: $SIZE bytes"
done

# ============================================================================
# STEP 2: Flash firmware (@@FLASH_METHOD_NAME@@)
# ============================================================================
log "STEP 2: Flashing firmware via @@FLASH_METHOD_NAME@@"

# Release serial port if using serial flash
if [ "$FLASH_METHOD" = "serial" ]; then
    pkill -f "cat $PORT" 2>/dev/null || true
    pkill screen 2>/dev/null || true
    sleep 2
fi

log "Running flash command..."
@@FLASH_BLOCK@@
@@WIFI_BLOCK@@
# ============================================================================
# DONE
# ============================================================================
log "============================================================================"
log "Deployment completed successfully!"
log "============================================================================"
echo "RESULT:SUCCESS"
