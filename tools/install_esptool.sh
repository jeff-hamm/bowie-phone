#!/bin/bash
# ============================================================================
# Install esptool standalone binary on macOS or Linux
# ============================================================================
# This script downloads the appropriate esptool binary for the platform
# and installs it to /tmp/esptool-<platform>/esptool
#
# Usage: ./install_esptool.sh
# ============================================================================

set -e

ESPTOOL_VERSION="v4.8.1"

# Detect platform
if [[ "$(uname)" == "Darwin" ]]; then
    if [[ "$(uname -m)" == "arm64" ]]; then
        PLATFORM="macos-arm64"
    else
        PLATFORM="macos-amd64"
    fi
else
    PLATFORM="linux-amd64"
fi

URL="https://github.com/espressif/esptool/releases/download/${ESPTOOL_VERSION}/esptool-${ESPTOOL_VERSION}-${PLATFORM}.zip"
OUT_DIR="/tmp/esptool-${PLATFORM}"

echo "Downloading esptool from $URL"
cd /tmp
curl -L -o esptool.zip "$URL"
unzip -o esptool.zip -d /tmp
rm esptool.zip
chmod +x "${OUT_DIR}/esptool"
echo "✅ Esptool installed at ${OUT_DIR}/esptool"
