#pragma once

#include <Arduino.h>
#include <WiFi.h>

/**
 * Tailscale/WireGuard VPN Manager for ESP32
 * 
 * This module provides secure VPN connectivity via WireGuard,
 * which is the underlying protocol used by Tailscale.
 * 
 * Setup Instructions:
 * 1. On your Tailscale admin console, create an auth key or get peer info
 * 2. Generate WireGuard keys for ESP32: wg genkey | tee privatekey | wg pubkey > publickey
 * 3. Add the ESP32 as a peer in your Tailscale/Headscale setup
 * 4. Configure the defines in platformio.ini or call initTailscale() with params
 * 
 * For Headscale users:
 *   headscale nodes register --user your-user --key YOUR_ESP32_PUBLIC_KEY
 * 
 * For Tailscale users:
 *   Use the Tailscale admin panel to add a subnet router or get peer keys
 */

// Configuration defaults (override in platformio.ini build_flags)
#ifndef WIREGUARD_LOCAL_IP
#define WIREGUARD_LOCAL_IP "100.64.0.100"  // Tailscale CGNAT range
#endif

#ifndef WIREGUARD_PEER_PORT
#define WIREGUARD_PEER_PORT 41641  // Default Tailscale WireGuard port
#endif

/**
 * Initialize the Tailscale/WireGuard VPN connection
 * 
 * @param localIp The local IP address for this device on the tailnet (e.g., "100.64.0.100")
 * @param privateKey The WireGuard private key for this device (base64)
 * @param peerEndpoint The endpoint address of the Tailscale peer (e.g., "relay.tailscale.com")
 * @param peerPublicKey The WireGuard public key of the peer (base64)
 * @param peerPort The WireGuard port (default: 41641)
 * @return true if connection initiated successfully
 */
bool initTailscale(const char* localIp, 
                   const char* privateKey,
                   const char* peerEndpoint, 
                   const char* peerPublicKey,
                   uint16_t peerPort = WIREGUARD_PEER_PORT);

/**
 * Initialize Tailscale using build flags from platformio.ini
 * Requires WIREGUARD_* defines to be set
 * @return true if connection initiated successfully
 */
bool initTailscaleFromConfig();

/**
 * Check if the WireGuard VPN is connected and active
 * @return true if VPN tunnel is established
 */
bool isTailscaleConnected();

/**
 * Get the Tailscale/WireGuard local IP address
 * @return IP address string or nullptr if not connected
 */
const char* getTailscaleIP();

/**
 * Disconnect the WireGuard VPN
 */
void disconnectTailscale();

/**
 * Handle Tailscale connection maintenance (call in loop())
 * Handles reconnection, keepalives, etc.
 */
void handleTailscaleLoop();

/**
 * Get connection status string for debugging
 * @return Human-readable status string
 */
const char* getTailscaleStatus();
