#!/bin/bash
# ts-wg-bridge.sh — Bridge a WireGuard subnet through Tailscale on this server.
#
# Usage:
#   ts-wg-bridge.sh <subnet>            Add subnet (e.g. 10.253.0.0/24)
#   ts-wg-bridge.sh <subnet> --remove   Remove a previously bridged subnet
#   ts-wg-bridge.sh --list              Show current bridges
#
# Requires: tailscale, iptables, wg0 interface active.

set -euo pipefail

WG_CONF="/boot/config/wireguard/wg0.conf"
WG_IFACE="wg0"
TS_CIDR="100.64.0.0/10"

log()  { printf "\033[1;32m[bridge]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[bridge]\033[0m %s\n" "$*" >&2; }
die()  { printf "\033[1;31m[bridge]\033[0m %s\n" "$*" >&2; exit 1; }

usage() {
    sed -n '3,8s/^# //p' "$0"
    exit 1
}

# ── helpers ──────────────────────────────────────────────────────────────────

get_current_ts_routes() {
    tailscale debug prefs 2>/dev/null \
        | sed -n '/AdvertiseRoutes/,/\]/p' \
        | grep -oP '"\K[0-9./]+'
}

add_ts_route() {
    local subnet="$1"
    local existing
    existing=$(get_current_ts_routes | paste -sd, -)

    if echo ",$existing," | grep -qF ",$subnet,"; then
        log "Tailscale already advertises $subnet"
        return 1  # no new subnet
    fi

    local new_routes
    if [ -n "$existing" ]; then
        new_routes="${existing},${subnet}"
    else
        new_routes="$subnet"
    fi

    log "Advertising routes: $new_routes"
    tailscale set --advertise-routes="$new_routes"
    return 0  # new subnet added
}

remove_ts_route() {
    local subnet="$1"
    local existing
    existing=$(get_current_ts_routes | paste -sd, -)

    if ! echo ",$existing," | grep -qF ",$subnet,"; then
        warn "Tailscale is not advertising $subnet — nothing to remove"
        return
    fi

    local new_routes
    new_routes=$(echo "$existing" | tr ',' '\n' | grep -vF "$subnet" | paste -sd, -)

    log "Updated routes: ${new_routes:-<none>}"
    tailscale set --advertise-routes="${new_routes}"
}

add_nat_rule() {
    local subnet="$1"
    local postup="PostUp=iptables -t nat -A POSTROUTING -s ${TS_CIDR} -d ${subnet} -o ${WG_IFACE} -j MASQUERADE"
    local postdown="PostDown=iptables -t nat -D POSTROUTING -s ${TS_CIDR} -d ${subnet} -o ${WG_IFACE} -j MASQUERADE"

    # Runtime
    if ! iptables -t nat -C POSTROUTING -s "$TS_CIDR" -d "$subnet" -o "$WG_IFACE" -j MASQUERADE 2>/dev/null; then
        iptables -t nat -A POSTROUTING -s "$TS_CIDR" -d "$subnet" -o "$WG_IFACE" -j MASQUERADE
        log "Runtime NAT rule added for $subnet"
    else
        log "Runtime NAT rule already exists for $subnet"
    fi

    # Persistent
    if grep -Fq "$postup" "$WG_CONF" 2>/dev/null; then
        log "Persistent PostUp already in $WG_CONF"
    else
        cp -a "$WG_CONF" "${WG_CONF}.bak-$(date +%Y%m%d-%H%M%S)"
        sed -i "/^\[Peer\]/i $postup" "$WG_CONF"
        log "Persistent PostUp added to $WG_CONF"
    fi

    if grep -Fq "$postdown" "$WG_CONF" 2>/dev/null; then
        log "Persistent PostDown already in $WG_CONF"
    else
        sed -i "/^\[Peer\]/i $postdown" "$WG_CONF"
        log "Persistent PostDown added to $WG_CONF"
    fi
}

remove_nat_rule() {
    local subnet="$1"
    local postup="PostUp=iptables -t nat -A POSTROUTING -s ${TS_CIDR} -d ${subnet} -o ${WG_IFACE} -j MASQUERADE"
    local postdown="PostDown=iptables -t nat -D POSTROUTING -s ${TS_CIDR} -d ${subnet} -o ${WG_IFACE} -j MASQUERADE"

    # Runtime
    if iptables -t nat -C POSTROUTING -s "$TS_CIDR" -d "$subnet" -o "$WG_IFACE" -j MASQUERADE 2>/dev/null; then
        iptables -t nat -D POSTROUTING -s "$TS_CIDR" -d "$subnet" -o "$WG_IFACE" -j MASQUERADE
        log "Runtime NAT rule removed for $subnet"
    fi

    # Persistent
    if grep -Fq "$postup" "$WG_CONF" 2>/dev/null; then
        cp -a "$WG_CONF" "${WG_CONF}.bak-$(date +%Y%m%d-%H%M%S)"
        grep -vF "$postup" "$WG_CONF" > "${WG_CONF}.tmp" && mv "${WG_CONF}.tmp" "$WG_CONF"
        log "Persistent PostUp removed from $WG_CONF"
    fi

    if grep -Fq "$postdown" "$WG_CONF" 2>/dev/null; then
        grep -vF "$postdown" "$WG_CONF" > "${WG_CONF}.tmp" && mv "${WG_CONF}.tmp" "$WG_CONF"
        log "Persistent PostDown removed from $WG_CONF"
    fi
}

show_list() {
    log "Tailscale advertised routes:"
    get_current_ts_routes | sed 's/^/  /'

    log "Active Tailscale→WG NAT rules:"
    iptables -t nat -S POSTROUTING 2>/dev/null \
        | grep -- "-s ${TS_CIDR}.*-o ${WG_IFACE}.*MASQUERADE" \
        | sed 's/^/  /' || echo "  (none)"

    log "Persistent WG NAT lines in ${WG_CONF}:"
    grep -- "-s ${TS_CIDR}.*-o ${WG_IFACE}.*MASQUERADE" "$WG_CONF" 2>/dev/null \
        | sed 's/^/  /' || echo "  (none)"
}

# ── main ─────────────────────────────────────────────────────────────────────

[ $# -lt 1 ] && usage

if [ "$1" = "--list" ] || [ "$1" = "-l" ]; then
    show_list
    exit 0
fi

SUBNET="$1"
shift

# Basic CIDR validation
if ! echo "$SUBNET" | grep -qP '^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}/\d{1,2}$'; then
    die "Invalid subnet: $SUBNET  (expected CIDR like 10.253.0.0/24)"
fi

REMOVE=false
while [ $# -gt 0 ]; do
    case "$1" in
        --remove|-r) REMOVE=true ;;
        *) die "Unknown option: $1" ;;
    esac
    shift
done

SERVER_NAME=$(hostname)

if $REMOVE; then
    log "Removing bridge for $SUBNET …"
    remove_nat_rule "$SUBNET"
    remove_ts_route "$SUBNET"
    log "Done. Route removal is instant — no admin approval needed."
else
    log "Adding bridge for $SUBNET …"
    add_nat_rule "$SUBNET"
    NEW_ROUTE=true
    add_ts_route "$SUBNET" || NEW_ROUTE=false

    echo ""
    if $NEW_ROUTE; then
        log "╔══════════════════════════════════════════════════════════════╗"
        log "║  NEW SUBNET — Tailscale admin approval required!           ║"
        log "╠══════════════════════════════════════════════════════════════╣"
        log "║  1. Open: https://login.tailscale.com/admin/machines       ║"
        log "║  2. Find machine: ${SERVER_NAME}"
        log "║  3. Click ··· → Edit route settings…                       ║"
        log "║  4. Enable the new subnet: ${SUBNET}"
        log "╚══════════════════════════════════════════════════════════════╝"
    else
        log "Subnet was already advertised — no admin action needed."
    fi

    log "Done. Client machines with accept-routes enabled can now reach $SUBNET via this server."
fi
