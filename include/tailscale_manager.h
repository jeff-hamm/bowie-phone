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
#define WIREGUARD_PEER_PORT 51820 // Default Tailscale WireGuard port
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
 * Check if Tailscale should be enabled based on boot key press
 * Call this early in setup() before WiFi initialization
 * Checks if KEY1 (GPIO36) is held during boot
 * @return true if Tailscale should be enabled
 */
bool shouldEnableTailscale();

/**
 * Check if Tailscale was enabled at boot
 * @return true if Tailscale is enabled
 */
bool isTailscaleEnabled();

/**
 * Toggle Tailscale enabled state in NVS
 * Requires reboot to take effect
 * @return New enabled state
 */
bool toggleTailscaleEnabled();

/**
 * Set Tailscale enabled state explicitly in NVS
 * Requires reboot to take effect
 * @param enabled New state
 */
void setTailscaleEnabled(bool enabled);

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

// Set callback to check if reconnection should be skipped (e.g., during active call)
void setTailscaleSkipCallback(bool (*callback)());

/**
 * Get connection status string for debugging
 * @return Human-readable status string
 */
const char* getTailscaleStatus();

/**
 * VPN Configuration Storage (NVS)
 * These functions allow runtime configuration of VPN settings
 * without recompiling firmware.
 */

// Structure to hold VPN configuration
struct VPNConfig {
    char localIp[20];
    char privateKey[64];
    char peerEndpoint[128];
    char peerPublicKey[64];
    uint16_t peerPort;
    bool configured;  // true if NVS has valid config
};

/**
 * Load VPN configuration from NVS
 * @param config Pointer to VPNConfig struct to fill
 * @return true if valid config was loaded from NVS
 */
bool loadVPNConfig(VPNConfig* config);

/**
 * Save VPN configuration to NVS
 * @param config Pointer to VPNConfig struct to save
 * @return true if saved successfully
 */
bool saveVPNConfig(const VPNConfig* config);

/**
 * Clear VPN configuration from NVS (reverts to compile-time defaults)
 */
void clearVPNConfig();

/**
 * Check if VPN is configured (either via NVS or compile-time)
 * @return true if VPN can be initialized
 */
bool isVPNConfigured();

/**
 * Initialize VPN config web server routes
 * Call this after WiFi/webserver is ready
 * @param server Pointer to the WebServer instance
 */
void initVPNConfigRoutes(void* server);
