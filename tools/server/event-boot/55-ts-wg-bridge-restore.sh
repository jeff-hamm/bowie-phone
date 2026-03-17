#!/usr/bin/env bash
# @desc Restore Tailscale→WireGuard subnet bridges from persistent config
#
# Reads $PHOME/.conf/ts-wg-bridges.conf and:
#   1. Re-applies any missing iptables NAT rules (idempotent)
#   2. Ensures Tailscale is advertising all listed subnets
#
# The wg0.conf PostUp/PostDown handles NAT at WireGuard tunnel start,
# but this script covers the case where Tailscale restarts independently,
# or when interfaces come up in a different order than expected.
#
# Must run AFTER: 50-restore-wireguard-config.sh, Tailscale plugin started

set -euo pipefail

PHOME="${PHOME:-/mnt/pool/appdata/home}"
BRIDGES_CONF="${PHOME}/.conf/ts-wg-bridges.conf"
WG_IFACE="wg0"
TS_CIDR="100.64.0.0/10"

log()  { logger -t ts-wg-bridge-restore "$*"; echo "[ts-wg-bridge-restore] $*"; }
warn() { logger -t ts-wg-bridge-restore "WARNING: $*"; echo "[ts-wg-bridge-restore] WARNING: $*" >&2; }

if [[ ! -f "$BRIDGES_CONF" ]]; then
    log "No bridges config found at $BRIDGES_CONF — nothing to restore"
    exit 0
fi

subnets=()
while IFS= read -r line; do
    [[ "$line" =~ ^[[:space:]]*(#|$) ]] && continue
    subnets+=("$line")
done < "$BRIDGES_CONF"

if [[ ${#subnets[@]} -eq 0 ]]; then
    log "No subnets listed in $BRIDGES_CONF — nothing to restore"
    exit 0
fi

log "Restoring ${#subnets[@]} bridge(s): ${subnets[*]}"

# ── 1. iptables NAT rules ─────────────────────────────────────────────────────
# Guard against wg0 not being up yet — rules will fire from PostUp in that case
if ip link show "$WG_IFACE" &>/dev/null; then
    for subnet in "${subnets[@]}"; do
        if ! iptables -t nat -C POSTROUTING -s "$TS_CIDR" -d "$subnet" \
                -o "$WG_IFACE" -j MASQUERADE 2>/dev/null; then
            iptables -t nat -A POSTROUTING -s "$TS_CIDR" -d "$subnet" \
                -o "$WG_IFACE" -j MASQUERADE
            log "NAT rule restored for $subnet"
        else
            log "NAT rule already present for $subnet"
        fi
    done
else
    warn "$WG_IFACE not up yet — PostUp will apply NAT rules when tunnel starts"
fi

# ── 2. Tailscale route advertisement ─────────────────────────────────────────
# Wait up to 30s for tailscale to be ready (plugin starts async)
ts_ready=false
for i in $(seq 1 30); do
    if tailscale status &>/dev/null 2>&1; then
        ts_ready=true
        break
    fi
    sleep 1
done

if ! $ts_ready; then
    warn "Tailscale not reachable after 30s — skipping route advertisement restore"
    exit 0
fi

# Get current advertised routes
current_routes=$(tailscale debug prefs 2>/dev/null \
    | sed -n '/AdvertiseRoutes/,/\]/p' \
    | grep -oP '"\K[0-9./]+' \
    | paste -sd, - || true)

# Merge: ensure all bridge subnets are included
new_routes="$current_routes"
added=()
for subnet in "${subnets[@]}"; do
    if echo ",$new_routes," | grep -qF ",$subnet,"; then
        log "Tailscale already advertising $subnet"
    else
        new_routes="${new_routes:+${new_routes},}${subnet}"
        added+=("$subnet")
    fi
done

if [[ ${#added[@]} -gt 0 ]]; then
    log "Re-advertising missing subnets: ${added[*]}"
    tailscale set --advertise-routes="$new_routes"
    log "Tailscale routes updated: $new_routes"
else
    log "All subnets already advertised — no Tailscale update needed"
fi
