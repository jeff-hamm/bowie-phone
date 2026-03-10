#!/bin/bash
# Install esptool on a remote Unix host and link to ~/.local/opt/bin/esptool.
# Prints INSTALL_OK on success.

export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/miniforge3/bin:$PATH"
mkdir -p "$HOME/.local/opt/bin" "$HOME/.local/bin" 2>/dev/null

# Prefer PlatformIO's existing esptool if available
if [ -x "$HOME/.platformio/penv/bin/esptool" ]; then
    ln -sf "$HOME/.platformio/penv/bin/esptool" "$HOME/.local/opt/bin/esptool" || exit 1
elif [ -f "$HOME/.platformio/penv/bin/esptool.py" ]; then
    ln -sf "$HOME/.platformio/penv/bin/esptool.py" "$HOME/.local/opt/bin/esptool" || exit 1
elif [ -x "$HOME/.platformio/penv/bin/pip" ]; then
    "$HOME/.platformio/penv/bin/pip" install --quiet esptool || exit 1
    if [ -x "$HOME/.platformio/penv/bin/esptool" ]; then
        ln -sf "$HOME/.platformio/penv/bin/esptool" "$HOME/.local/opt/bin/esptool" || exit 1
    elif [ -f "$HOME/.platformio/penv/bin/esptool.py" ]; then
        ln -sf "$HOME/.platformio/penv/bin/esptool.py" "$HOME/.local/opt/bin/esptool" || exit 1
    else
        echo 'ERROR: esptool not found in PlatformIO penv after install'
        exit 1
    fi
elif [ -x "$HOME/miniforge3/bin/python" ]; then
    "$HOME/miniforge3/bin/python" -m pip install --quiet esptool || exit 1
elif [ -x "/opt/homebrew/bin/python3" ]; then
    /opt/homebrew/bin/python3 -m pip install --quiet --user esptool || exit 1
elif [ -x "/usr/local/bin/python3" ]; then
    /usr/local/bin/python3 -m pip install --quiet --user esptool || exit 1
elif command -v pip3 >/dev/null 2>&1 && pip3 --version >/dev/null 2>&1; then
    pip3 install --quiet --user esptool || exit 1
elif command -v pip >/dev/null 2>&1; then
    pip install --quiet --user esptool || exit 1
elif command -v python3 >/dev/null 2>&1 && python3 -m pip --version >/dev/null 2>&1; then
    python3 -m pip install --quiet --user esptool || exit 1
else
    echo 'ERROR: pip not found'
    exit 1
fi

# Link newly installed esptool to canonical location
if [ -x "$HOME/miniforge3/bin/esptool" ]; then
    ln -sf "$HOME/miniforge3/bin/esptool" "$HOME/.local/opt/bin/esptool" || true
elif [ -x "$HOME/.local/bin/esptool" ]; then
    ln -sf "$HOME/.local/bin/esptool" "$HOME/.local/opt/bin/esptool" || true
elif command -v esptool >/dev/null 2>&1; then
    ln -sf "$(command -v esptool)" "$HOME/.local/opt/bin/esptool" || true
fi

test -f "$HOME/.local/opt/bin/esptool" || exit 1
echo 'INSTALL_OK'
