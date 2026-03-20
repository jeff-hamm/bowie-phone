---
name: server-routes
description: View, add, or edit WireGuard routes, iptables NAT rules, dnsmasq config, and Tailscale subnet advertisements on the Unraid server. Use for "routes", "iptables", "NAT", "dnsmasq", "wireguard config", "add peer", "subnet bridge", "server firewall", or "networking config".
compatibility: Requires SSH access to root@192.168.1.216 (Unraid server). WireGuard tools (wg, wg-quick) and Tailscale must be installed on the server.
---

# Server Routes & Networking Skill

## Scope

- Server: Unraid at `192.168.1.216` (hostname: `jumpdrive`)
- SSH: `root@192.168.1.216` (key-based auth)
- Networking doc: `docs/system/NETWORKING.md`
- Server tools: `tools/server/`
- Install script: `tools/server/install.sh`

## Key File Locations on Server

| File | Purpose |
|------|---------|
| `/mnt/pool/appdata/home/.conf/wg0.conf` | WireGuard server config (peers, NAT rules) |
| `/boot/config/wireguard/wg0.conf` | WireGuard flash copy (survives reboot) |
| `/mnt/pool/appdata/home/.conf/ts-wg-bridges.conf` | Tailscale↔WG bridge subnet list |
| `/mnt/pool/appdata/dnsmasq/dnsmasq.conf` | DNS forwarder config |
| `/mnt/pool/appdata/phone-receiver/docker-compose.yml` | Docker services (log-receiver, dnsmasq, log-viewer) |
| `/mnt/pool/appdata/home/.local/bin/ts-wg-bridge` | Bridge management script |
| `/mnt/pool/appdata/home/event.d/boot/55-ts-wg-bridge-restore.sh` | Boot-time bridge restore |

## Environment Variables

From `tools/server/.env`:
```
APP_DATA=/mnt/pool/appdata
PHOME=${APP_DATA}/home
```

## SSH Command Pattern

All server commands run via SSH. Use this pattern:

```powershell
ssh root@192.168.1.216 '<command>'
```

For multi-line scripts, use a heredoc:
```powershell
ssh root@192.168.1.216 'bash -s' @'
<commands>
'@
```

## Common Operations

### View current WireGuard status
```powershell
ssh root@192.168.1.216 'wg show wg0'
```

### View current peers and IPs
```powershell
ssh root@192.168.1.216 'grep -E "^\[Peer\]|AllowedIPs|^#" /mnt/pool/appdata/home/.conf/wg0.conf'
```

### View iptables NAT rules
```powershell
ssh root@192.168.1.216 'iptables -t nat -S POSTROUTING; iptables -t nat -S PREROUTING'
```

### View current Tailscale advertised routes
```powershell
ssh root@192.168.1.216 'tailscale status; tailscale debug prefs 2>/dev/null | grep -A5 AdvertiseRoutes'
```

### Add a new WireGuard peer

1. Generate keypair on the server:
   ```powershell
   ssh root@192.168.1.216 'wg genkey | tee /tmp/newpeer_privkey | wg pubkey'
   ```

2. Add peer to wg0.conf (both pool and flash copies):
   ```powershell
   ssh root@192.168.1.216 'bash -s' @'
   CONF_POOL="/mnt/pool/appdata/home/.conf/wg0.conf"
   CONF_FLASH="/boot/config/wireguard/wg0.conf"
   PEER_NAME="<name>"
   PEER_PUBKEY="<pubkey>"
   PEER_IP="10.253.0.<N>"
   
   for conf in "$CONF_POOL" "$CONF_FLASH"; do
     cp -a "$conf" "${conf}.bak-$(date +%Y%m%d-%H%M%S)"
     cat >> "$conf" <<PEER

   [Peer]
   # $PEER_NAME
   PublicKey = $PEER_PUBKEY
   AllowedIPs = ${PEER_IP}/32
   PEER
   done
   
   wg syncconf wg0 <(wg-quick strip wg0)
   echo "Peer $PEER_NAME ($PEER_IP) added"
   '@
   ```

3. Verify:
   ```powershell
   ssh root@192.168.1.216 'wg show wg0'
   ```

### Add/remove a Tailscale↔WG subnet bridge

Use the `ts-wg-bridge` tool already installed on the server:

```powershell
# Add a bridge
ssh root@192.168.1.216 '/mnt/pool/appdata/home/.local/bin/ts-wg-bridge 10.253.0.0/24'

# Remove a bridge
ssh root@192.168.1.216 '/mnt/pool/appdata/home/.local/bin/ts-wg-bridge 10.253.0.0/24 --remove'

# List current bridges
ssh root@192.168.1.216 '/mnt/pool/appdata/home/.local/bin/ts-wg-bridge --list'
```

### Edit dnsmasq config

```powershell
ssh root@192.168.1.216 'cat /mnt/pool/appdata/dnsmasq/dnsmasq.conf'
# After editing:
ssh root@192.168.1.216 'docker restart esp_wg_proxy'
```

### Restart WireGuard

```powershell
ssh root@192.168.1.216 'wg-quick down wg0 && wg-quick up wg0'
```

**WARNING:** This will disconnect all WG peers (including ESP32 devices). They will auto-reconnect.

### Restart Docker services

```powershell
ssh root@192.168.1.216 'cd /mnt/pool/appdata/phone-receiver && docker compose up -d'
```

### Deploy server tools from repo

```powershell
# Copy tools/server/ to the server and run install
scp -r tools/server/* root@192.168.1.216:/tmp/bowie-server-install/
ssh root@192.168.1.216 'cd /tmp/bowie-server-install && bash install.sh'
```

## IP Allocation (from NETWORKING.md)

| Range | Purpose |
|-------|---------|
| `10.253.0.1` | Server (always `.1`) |
| `10.253.0.2` – `.9` | ESP32 microcontrollers |
| `10.253.0.10` – `.19` | Developer machines |
| `10.253.0.20` – `.254` | Reserved |

## Safety Rules

- Always back up config files before editing (the commands above include `cp -a` backup steps)
- Use `wg syncconf` instead of `wg-quick down/up` when possible to avoid dropping all peers
- After adding a Tailscale route, admin approval is needed at https://login.tailscale.com/admin/machines
- After any route change, update `docs/system/NETWORKING.md` to keep documentation in sync
