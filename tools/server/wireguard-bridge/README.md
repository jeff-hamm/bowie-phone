# Tailscale ↔ WireGuard Bridge Script

## What It Does

`ts-wg-bridge.sh` lets a **Tailscale** client reach a device that is only
connected via **WireGuard** — by using this Unraid server as a router between
the two networks.

It does three things:

1. **Advertises** a WireGuard subnet to Tailscale (`tailscale set --advertise-routes`)
2. **Adds a NAT rule** so return traffic from WireGuard peers is routed back
   correctly (they don't know about Tailscale addresses)
3. **Persists** the NAT rule inside `wg0.conf` PostUp/PostDown so it survives
   reboots

## When To Use It

| Scenario | Command |
|----------|---------|
| You have a new WireGuard-only device and want to reach it from a Tailscale machine | `ts-wg-bridge.sh 10.253.0.0/24` |
| You added a second WireGuard subnet (e.g. `10.254.0.0/24`) | `ts-wg-bridge.sh 10.254.0.0/24` |
| You no longer need the bridge for a subnet | `ts-wg-bridge.sh 10.253.0.0/24 --remove` |
| You want to see what's currently bridged | `ts-wg-bridge.sh --list` |

## Location

```
/boot/config/custom-scripts/ts-wg-bridge.sh   (persistent across reboots)
```

## Usage

```bash
# Add a bridge (run as root on the Unraid server):
/boot/config/custom-scripts/ts-wg-bridge.sh 10.253.0.0/24

# Remove a bridge:
/boot/config/custom-scripts/ts-wg-bridge.sh 10.253.0.0/24 --remove

# List current bridges:
/boot/config/custom-scripts/ts-wg-bridge.sh --list
```

## After Adding a New Subnet

When a **new** subnet is advertised to Tailscale for the first time, the script
will print a box like this:

```
[bridge] ╔══════════════════════════════════════════════════════════════╗
[bridge] ║  NEW SUBNET — Tailscale admin approval required!           ║
[bridge] ╠══════════════════════════════════════════════════════════════╣
[bridge] ║  1. Open: https://login.tailscale.com/admin/machines       ║
[bridge] ║  2. Find machine: jumpdrive                                ║
[bridge] ║  3. Click ··· → Edit route settings…                       ║
[bridge] ║  4. Enable the new subnet: 10.253.0.0/24                  ║
[bridge] ╚══════════════════════════════════════════════════════════════╝
```

You **must** approve the route in the Tailscale admin console before any
Tailscale client can use it. This is a Tailscale security requirement — the
server can offer routes, but only an admin can accept them.

If the subnet was already advertised (e.g. re-running after a reboot), no admin
action is needed.

## On the Client Machine

Tailscale clients that have `--accept-routes` enabled (the default on most
platforms) will automatically learn the new subnet route once it's approved.

To verify from a Tailscale client (e.g. Windows):

```powershell
Test-NetConnection 10.253.0.2 -InformationLevel Detailed
```

## How It Works (Architecture)

```
┌──────────────┐         ┌──────────────────┐         ┌──────────────┐
│  Tailscale   │  TS     │   Unraid Server  │   WG    │  WG-only     │
│  Client      │◄───────►│  (jumpdrive)     │◄───────►│  Device      │
│ 100.107.7.10 │  tunnel │ TS: 100.86.189.46│  tunnel │ 10.253.0.2   │
└──────────────┘         │ WG: 10.253.0.1   │         └──────────────┘
                         └──────────────────┘
                         Advertises 10.253.0.0/24 to Tailscale.
                         NATs return traffic so WG peer replies work.
```

## What Gets Changed

| Change | Where | Persistent? |
|--------|-------|-------------|
| `tailscale set --advertise-routes=…` | Tailscale state | Yes (stored in tailscaled state) |
| `iptables -t nat -A POSTROUTING …` | Kernel netfilter | No (runtime only) |
| `PostUp=/PostDown=` lines in wg0.conf | `/boot/config/wireguard/wg0.conf` | Yes (Unraid flash) |

A backup of `wg0.conf` is created automatically before any edit
(`wg0.conf.bak-YYYYMMDD-HHMMSS`).

## Prerequisites

- WireGuard tunnel `wg0` must be active on the server
- Tailscale must be running on the server
- `ip_forward` must be enabled (already set by the Unraid Tailscale plugin)
- You must be a Tailscale admin to approve new subnet routes
