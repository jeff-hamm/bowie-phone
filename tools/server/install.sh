#!/usr/bin/env bash
# Install / update Bowie Phone server tools on Unraid.
# Run directly on the server — no SSH wrapper needed.
#
# Usage:
#   ./install.sh              # Install using defaults from .env
#   APP_DATA=/custom/path ./install.sh   # Override APP_DATA

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load .env if present (provides APP_DATA, PHOME, etc.)
if [[ -f "$SCRIPT_DIR/.env" ]]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/.env"
fi

APP_DATA="${APP_DATA:-/mnt/pool/appdata}"
PHOME="${PHOME:-${APP_DATA}/home}"

log()  { printf "\033[1;32m[install]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[install]\033[0m %s\n" "$*" >&2; }
die()  { printf "\033[1;31m[install]\033[0m %s\n" "$*" >&2; exit 1; }

# ── helpers ───────────────────────────────────────────────────────────────────

# Expand $PHOME and $APP_DATA in an @location value.
expand_location() {
    local loc="$1"
    loc="${loc//\$PHOME/$PHOME}"
    loc="${loc//\$APP_DATA/$APP_DATA}"
    loc="${loc//\$\{PHOME\}/$PHOME}"
    loc="${loc//\$\{APP_DATA\}/$APP_DATA}"
    echo "$loc"
}

# Parse @location from a file's header comments (first 10 lines).
parse_location() {
    local file="$1"
    head -n 10 "$file" | sed -n 's/^#.*@location[[:space:]]\+\(.*\)/\1/p' | head -1
}

# Install a regular directory (copy contents).
install_dir() {
    local src="$1" dest="$2" name="$3"
    log "Installing $name → $dest"
    mkdir -p "$dest"
    # rsync preferred; fall back to cp
    if command -v rsync &>/dev/null; then
        rsync -a --delete "$src/" "$dest/"
    else
        cp -a "$src/." "$dest/"
    fi
}

# Install a symlinked directory — each file with @location gets a symlink.
install_symlinked() {
    local src="$1" name="$2"
    local dest="${APP_DATA}/${name}"
    log "Installing $name (symlinks) → $dest"
    mkdir -p "$dest"
    # Copy source files into APP_DATA/<name>
    cp -a "$src/." "$dest/"

    for file in "$dest"/*; do
        [[ -f "$file" ]] || continue
        local loc
        loc="$(parse_location "$file")"
        [[ -z "$loc" ]] && continue

        local target_dir
        target_dir="$(expand_location "$loc")"
        local base
        base="$(basename "$file")"
        local target="${target_dir}/${base}"

        mkdir -p "$target_dir"

        if [[ -L "$target" ]]; then
            local existing
            existing="$(readlink -f "$target")"
            if [[ "$existing" == "$(readlink -f "$file")" ]]; then
                log "  ✓ $base → $target (already linked)"
                continue
            fi
            warn "  Updating symlink $target"
            ln -sf "$file" "$target"
        elif [[ -e "$target" ]]; then
            warn "  Backing up existing $target → ${target}.bak"
            mv "$target" "${target}.bak"
            ln -s "$file" "$target"
        else
            ln -s "$file" "$target"
        fi
        log "  → $base symlinked to $target"
        # Ensure scripts are executable
        chmod +x "$file" 2>/dev/null || true
    done
}

# ── main ──────────────────────────────────────────────────────────────────────

log "Bowie Phone server tools installer"
log "  APP_DATA = $APP_DATA"
log "  PHOME    = $PHOME"
echo ""

# 1. phone-receiver: regular copy + docker compose up
if [[ -d "$SCRIPT_DIR/phone-receiver" ]]; then
    install_dir "$SCRIPT_DIR/phone-receiver" "${APP_DATA}/phone-receiver" "phone-receiver"
    # Build and restart the container
    if command -v docker &>/dev/null && [[ -f "${APP_DATA}/phone-receiver/docker-compose.yml" ]]; then
        log "Building and starting phone-receiver containers..."
        (cd "${APP_DATA}/phone-receiver" && docker compose --env-file "$SCRIPT_DIR/.env" up -d --build)
    fi
fi

# 2. wireguard-bridge: copy to APP_DATA, then symlink based on @location
if [[ -d "$SCRIPT_DIR/wireguard-bridge" ]]; then
    install_symlinked "$SCRIPT_DIR/wireguard-bridge" "wireguard-bridge"
fi

echo ""
log "Done. Installed services:"
log "  phone-receiver  → ${APP_DATA}/phone-receiver"
log "  wireguard-bridge → ${APP_DATA}/wireguard-bridge (symlinked)"
