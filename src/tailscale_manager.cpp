#include "tailscale_manager.h"
#include "logging.h"
#include <WireGuard-ESP32.h>
#include <time.h>

// WireGuard instance
static WireGuard wg;
static bool vpnConnected = false;
static bool vpnInitialized = false;
static char tailscaleIp[20] = {0};
static char statusBuffer[64] = "Not initialized";
static unsigned long lastReconnectAttempt = 0;
static const unsigned long RECONNECT_INTERVAL = 30000; // 30 seconds

// Stored config for reconnection
static char storedPrivateKey[64] = {0};
static char storedPeerEndpoint[128] = {0};
static char storedPeerPublicKey[64] = {0};
static uint16_t storedPeerPort = 41641;

bool initTailscale(const char* localIp, 
                   const char* privateKey,
                   const char* peerEndpoint, 
                   const char* peerPublicKey,
                   uint16_t peerPort) {
    
    if (!WiFi.isConnected()) {
        Logger.println("❌ Tailscale: WiFi not connected");
        strcpy(statusBuffer, "WiFi not connected");
        return false;
    }
    
    // Store config for potential reconnection
    strncpy(tailscaleIp, localIp, sizeof(tailscaleIp) - 1);
    strncpy(storedPrivateKey, privateKey, sizeof(storedPrivateKey) - 1);
    strncpy(storedPeerEndpoint, peerEndpoint, sizeof(storedPeerEndpoint) - 1);
    strncpy(storedPeerPublicKey, peerPublicKey, sizeof(storedPeerPublicKey) - 1);
    storedPeerPort = peerPort;
    
    Logger.println("🔐 Tailscale: Syncing time via NTP...");
    strcpy(statusBuffer, "Syncing NTP...");
    
    // WireGuard requires accurate time for handshake
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    
    // Wait for time sync (max 10 seconds)
    time_t now = 0;
    int attempts = 0;
    while (now < 1000000000 && attempts < 20) {
        delay(500);
        time(&now);
        attempts++;
    }
    
    if (now < 1000000000) {
        Logger.println("⚠️ Tailscale: NTP sync timeout, continuing anyway");
    } else {
        Logger.printf("✅ Tailscale: Time synced: %ld\n", now);
    }
    
    Logger.println("🔐 Tailscale: Starting WireGuard tunnel...");
    Logger.printf("   Local IP: %s\n", localIp);
    Logger.printf("   Peer: %s:%d\n", peerEndpoint, peerPort);
    strcpy(statusBuffer, "Connecting...");
    
    // Parse local IP
    IPAddress localAddr;
    if (!localAddr.fromString(localIp)) {
        Logger.println("❌ Tailscale: Invalid local IP format");
        strcpy(statusBuffer, "Invalid local IP");
        return false;
    }
    
    // Start WireGuard
    bool result = wg.begin(
        localAddr,
        privateKey,
        peerEndpoint,
        peerPublicKey,
        peerPort
    );
    
    if (result) {
        vpnConnected = true;
        vpnInitialized = true;
        Logger.printf("✅ Tailscale: Connected! Local IP: %s\n", localIp);
        snprintf(statusBuffer, sizeof(statusBuffer), "Connected: %s", localIp);
    } else {
        Logger.println("❌ Tailscale: Failed to establish tunnel");
        strcpy(statusBuffer, "Connection failed");
    }
    
    return result;
}

bool initTailscaleFromConfig() {
#if defined(WIREGUARD_PRIVATE_KEY) && defined(WIREGUARD_PEER_PUBLIC_KEY) && \
    defined(WIREGUARD_PEER_ENDPOINT) && defined(WIREGUARD_LOCAL_IP)
    
    Logger.println("🔐 Tailscale: Initializing from build config...");
    return initTailscale(
        WIREGUARD_LOCAL_IP,
        WIREGUARD_PRIVATE_KEY,
        WIREGUARD_PEER_ENDPOINT,
        WIREGUARD_PEER_PUBLIC_KEY,
        WIREGUARD_PEER_PORT
    );
    
#else
    Logger.println("⚠️ Tailscale: No WireGuard config in build flags");
    Logger.println("   Set WIREGUARD_* defines in platformio.ini");
    strcpy(statusBuffer, "Not configured");
    return false;
#endif
}

bool isTailscaleConnected() {
    return vpnConnected && vpnInitialized;
}

const char* getTailscaleIP() {
    if (vpnConnected && tailscaleIp[0] != '\0') {
        return tailscaleIp;
    }
    return nullptr;
}

void disconnectTailscale() {
    if (vpnInitialized) {
        Logger.println("🔐 Tailscale: Disconnecting...");
        wg.end();
        vpnConnected = false;
        vpnInitialized = false;
        strcpy(statusBuffer, "Disconnected");
    }
}

void handleTailscaleLoop() {
    // Check if we need to reconnect
    if (vpnInitialized && !vpnConnected) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
            lastReconnectAttempt = now;
            
            if (WiFi.isConnected() && storedPrivateKey[0] != '\0') {
                Logger.println("🔐 Tailscale: Attempting reconnection...");
                strcpy(statusBuffer, "Reconnecting...");
                
                // Try to reconnect
                IPAddress localAddr;
                localAddr.fromString(tailscaleIp);
                
                bool result = wg.begin(
                    localAddr,
                    storedPrivateKey,
                    storedPeerEndpoint,
                    storedPeerPublicKey,
                    storedPeerPort
                );
                
                if (result) {
                    vpnConnected = true;
                    Logger.println("✅ Tailscale: Reconnected!");
                    snprintf(statusBuffer, sizeof(statusBuffer), "Connected: %s", tailscaleIp);
                }
            }
        }
    }
}

const char* getTailscaleStatus() {
    return statusBuffer;
}
