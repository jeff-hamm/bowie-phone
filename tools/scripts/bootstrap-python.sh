#!/bin/bash
# Bootstrap Python toolchain on macOS (Homebrew or Miniforge fallback).
# Prints PYTHON_READY on success.

set -e
export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/miniforge3/bin:$PATH"

if command -v python3 >/dev/null 2>&1 && python3 -m pip --version >/dev/null 2>&1; then
    echo 'PYTHON_READY'
    exit 0
fi

if command -v brew >/dev/null 2>&1; then
    if [ -x /opt/homebrew/bin/brew ]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [ -x /usr/local/bin/brew ]; then
        eval "$(/usr/local/bin/brew shellenv)"
    fi
    brew install python >/dev/null 2>&1 || true
fi

if command -v python3 >/dev/null 2>&1 && python3 -m pip --version >/dev/null 2>&1; then
    echo 'PYTHON_READY'
    exit 0
fi

# Fallback: install Miniforge
ARCH="$(uname -m)"
if [ "$ARCH" = "arm64" ]; then
    MINIFORGE_URL="https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-MacOSX-arm64.sh"
else
    MINIFORGE_URL="https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-MacOSX-x86_64.sh"
fi

INSTALLER="/tmp/miniforge.sh"
curl -fsSL "$MINIFORGE_URL" -o "$INSTALLER"
bash "$INSTALLER" -b -p "$HOME/miniforge3" >/dev/null
"$HOME/miniforge3/bin/python" -m pip --version >/dev/null 2>&1
echo 'PYTHON_READY'
