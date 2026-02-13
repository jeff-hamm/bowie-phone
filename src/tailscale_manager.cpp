#include "tailscale_manager.h"
#include "logging.h"
#include "config.h"
#include "remote_logger.h"
#include "notifications.h"
#include <WireGuard-ESP32.h>
#include <Preferences.h>
#include <WebServer.h>
#include <time.h>

// Tailscale enable pin configuration
// Can be overridden via build flags: -DTAILSCALE_ENABLE_PIN=xx
#ifndef TAILSCALE_ENABLE_PIN
#define TAILSCALE_ENABLE_PIN 36  // Default: KEY1 on AudioKit board
#endif

// NVS namespace for VPN config
#define VPN_NVS_NAMESPACE "vpn"

// WireGuard instance
static WireGuard wg;
static Preferences vpnPrefs;
static bool vpnConnected = false;
static bool vpnInitialized = false;
static char tailscaleIp[20] = {0};
static char statusBuffer[64] = "Not initialized";
static unsigned long lastReconnectAttempt = 0;
static const unsigned long RECONNECT_INTERVAL = 600000; // 10 minutes

// Stored config for reconnection
static char storedPrivateKey[64] = {0};
static char storedPeerEndpoint[128] = {0};
static char storedPeerPublicKey[64] = {0};
static uint16_t storedPeerPort = 51820;
#ifdef TAILSCALE_ALWAYS_ENABLED
static bool tailscaleEnabled = true;  // Set by shouldEnableTailscale()
#else
static bool tailscaleEnabled = false; // Set by shouldEnableTailscale()
#endif
// Callback to check if reconnection should be skipped (e.g., phone is off hook)
static bool (*shouldSkipReconnect)() = nullptr;

// Callbacks for Tailscale connection state changes
static TailscaleStateCallback onTailscaleConnect = nullptr;
static TailscaleStateCallback onTailscaleDisconnect = nullptr;

// NVS namespace for Tailscale enable state
#define TAILSCALE_NVS_NAMESPACE "tailscale"

// Load tailscale enabled state from NVS
static bool loadTailscaleEnabledState() {
    Preferences prefs;
    if (!prefs.begin(TAILSCALE_NVS_NAMESPACE, true)) {
        return false;  // Default: disabled
    }
    bool enabled = prefs.getBool("enabled", false);
    prefs.end();
    return enabled;
}

// Save tailscale enabled state to NVS
static void saveTailscaleEnabledState(bool enabled) {
    Preferences prefs;
    if (!prefs.begin(TAILSCALE_NVS_NAMESPACE, false)) {
        Logger.println("❌ Failed to save Tailscale state to NVS");
        return;
    }
    prefs.putBool("enabled", enabled);
    prefs.end();
    Logger.printf("💾 Tailscale enabled state saved: %s\n", enabled ? "ON" : "OFF");
}

// Toggle tailscale enabled state in NVS and return new state
bool toggleTailscaleEnabled() {
    bool newState = !loadTailscaleEnabledState();
    saveTailscaleEnabledState(newState);
    tailscaleEnabled = newState;
    Logger.printf("🔐 Tailscale toggled to: %s (reboot required)\n", newState ? "ENABLED" : "DISABLED");
    return newState;
}

// Set tailscale enabled state explicitly
void setTailscaleEnabled(bool enabled) {
    saveTailscaleEnabledState(enabled);
    tailscaleEnabled = enabled;
    Logger.printf("🔐 Tailscale set to: %s (reboot required)\n", enabled ? "ENABLED" : "DISABLED");
}

bool shouldEnableTailscale() {
    // Load saved state from NVS first
    bool savedState = loadTailscaleEnabledState();
    
    // Check if enable pin is held during boot - TOGGLES the saved state
    // Default: KEY1 (GPIO36) on AudioKit board
    pinMode(TAILSCALE_ENABLE_PIN, INPUT_PULLUP);
    delay(50);  // Debounce
    bool pinHeld = (digitalRead(TAILSCALE_ENABLE_PIN) == LOW);
    #ifdef TAILSCALE_ALWAYS_ENABLED
        saveTailscaleEnabledState(true);
    // Always enable Tailscale when TAILSCALE_ALWAYS_ENABLED is defined
        Logger.println("🔐 Tailscale VPN ALWAYS ENABLED (compile-time flag)");
        tailscaleEnabled = true;
        return true;
    #else
    if (pinHeld) {
        // Toggle the saved state
        bool newState = !savedState;
        saveTailscaleEnabledState(newState);
        Logger.printf("🔐 GPIO%d held at boot - Tailscale toggled to: %s\n", 
                      TAILSCALE_ENABLE_PIN, newState ? "ENABLED" : "DISABLED");
        tailscaleEnabled = newState;
    } else {
        // Use saved state
        tailscaleEnabled = savedState;
        if (savedState) {
            Logger.printf("🔐 Tailscale VPN ENABLED (from saved state)\n");
        } else {
            Logger.printf("🌐 Tailscale VPN DISABLED (hold GPIO%d during boot to toggle)\n", TAILSCALE_ENABLE_PIN);
        }
    }
    
    return tailscaleEnabled;
#endif
}

bool isTailscaleEnabled() {
    return tailscaleEnabled;
}

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
    
    // Try to resolve the endpoint hostname first to check DNS
    IPAddress peerAddr;
    if (!WiFi.hostByName(peerEndpoint, peerAddr)) {
        Logger.printf("⚠️ Tailscale: DNS lookup failed for %s\n", peerEndpoint);
        Logger.println("   Will let WireGuard try anyway...");
    } else {
        Logger.printf("✅ Tailscale: Resolved %s -> %s\n", peerEndpoint, peerAddr.toString().c_str());
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
        
        // Notify Tailscale connected (turns on red LED)
        notify(NotificationType::TailscaleConnected, true);
        
        // Call connect callback if set
        if (onTailscaleConnect) {
            onTailscaleConnect();
        }
        
        // Use WireGuard server's DNS forwarder (10.253.0.1) as primary
        // This ensures DNS works through the VPN tunnel
        // Fallback to public DNS in case WireGuard server DNS is down
        IPAddress vpnDns(10, 253, 0, 1);  // WireGuard server running dnsmasq
        IPAddress fallbackDns = DNS_PRIMARY_IPADDRESS;  // Public DNS fallback
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), vpnDns, fallbackDns);
        Logger.printf("🌐 DNS configured for VPN: %s (primary), %s (fallback)\n", 
                      vpnDns.toString().c_str(), fallbackDns.toString().c_str());
    } else {
        vpnInitialized = true;  // Mark as initialized so handleTailscaleLoop will retry
        vpnConnected = false;
        Logger.println("❌ Tailscale: Failed to establish tunnel - will retry");
        strcpy(statusBuffer, "Connection failed - retrying");
    }
    
    return result;
}

bool initTailscaleFromConfig() {
    // First try to load from NVS (runtime configuration)
    VPNConfig config;
    if (loadVPNConfig(&config) && config.configured) {
        Logger.println("🔐 Tailscale: Initializing from NVS config...");
        return initTailscale(
            config.localIp,
            config.privateKey,
            config.peerEndpoint,
            config.peerPublicKey,
            config.peerPort
        );
    }
    
    // Fall back to compile-time defines
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
    Logger.println("⚠️ Tailscale: No WireGuard config in build flags or NVS");
    Logger.println("   Configure via /vpn web page or set WIREGUARD_* defines");
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
        
        // Notify Tailscale disconnected (turns off red LED)
        notify(NotificationType::TailscaleConnected, false);
        
        // Call disconnect callback if set
        if (onTailscaleDisconnect) {
            onTailscaleDisconnect();
        }
    }
}

void setTailscaleSkipCallback(bool (*callback)()) {
    shouldSkipReconnect = callback;
}

void setTailscaleConnectCallback(TailscaleStateCallback callback) {
    onTailscaleConnect = callback;
}

void setTailscaleDisconnectCallback(TailscaleStateCallback callback) {
    onTailscaleDisconnect = callback;
}

void handleTailscaleLoop() {
    // Skip if Tailscale was not enabled at boot
    if (!tailscaleEnabled) {
        return;
    }
    
    // Flush buffered remote logs periodically
    RemoteLogger.loop();
    
    // Check if we need to reconnect
    if (vpnInitialized && !vpnConnected) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
            lastReconnectAttempt = now;
            
            // Skip reconnection if callback says so (e.g., phone is off hook for DTMF)
            if (shouldSkipReconnect && shouldSkipReconnect()) {
                Logger.println("🔐 Tailscale: Skipping reconnect (phone in use)");
                return;
            }
            
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
                    
                    // Notify Tailscale connected (turns on red LED)
                    notify(NotificationType::TailscaleConnected, true);
                    
                    // Call connect callback if set
                    if (onTailscaleConnect) {
                        onTailscaleConnect();
                    }
                }
            }
        }
    }
}

const char* getTailscaleStatus() {
    return statusBuffer;
}

// ============================================================================
// VPN Configuration Storage (NVS)
// ============================================================================

bool loadVPNConfig(VPNConfig* config) {
    if (!config) return false;
    
    memset(config, 0, sizeof(VPNConfig));
    
    if (!vpnPrefs.begin(VPN_NVS_NAMESPACE, true)) {  // Read-only
        Logger.println("ℹ️ VPN: No NVS config found");
        return false;
    }
    
    config->configured = vpnPrefs.getBool("configured", false);
    
    if (config->configured) {
        String localIp = vpnPrefs.getString("localIp", "");
        String privateKey = vpnPrefs.getString("privateKey", "");
        String peerEndpoint = vpnPrefs.getString("peerEndpoint", "");
        String peerPublicKey = vpnPrefs.getString("peerPublicKey", "");
        config->peerPort = vpnPrefs.getUShort("peerPort", 41641);
        
        strncpy(config->localIp, localIp.c_str(), sizeof(config->localIp) - 1);
        strncpy(config->privateKey, privateKey.c_str(), sizeof(config->privateKey) - 1);
        strncpy(config->peerEndpoint, peerEndpoint.c_str(), sizeof(config->peerEndpoint) - 1);
        strncpy(config->peerPublicKey, peerPublicKey.c_str(), sizeof(config->peerPublicKey) - 1);
        
        Logger.printf("✅ VPN: Loaded config from NVS (endpoint: %s)\n", config->peerEndpoint);
    }
    
    vpnPrefs.end();
    return config->configured;
}

bool saveVPNConfig(const VPNConfig* config) {
    if (!config) return false;
    
    if (!vpnPrefs.begin(VPN_NVS_NAMESPACE, false)) {  // Read-write
        Logger.println("❌ VPN: Failed to open NVS for writing");
        return false;
    }
    
    vpnPrefs.putString("localIp", config->localIp);
    vpnPrefs.putString("privateKey", config->privateKey);
    vpnPrefs.putString("peerEndpoint", config->peerEndpoint);
    vpnPrefs.putString("peerPublicKey", config->peerPublicKey);
    vpnPrefs.putUShort("peerPort", config->peerPort);
    vpnPrefs.putBool("configured", true);
    
    vpnPrefs.end();
    
    Logger.printf("✅ VPN: Config saved to NVS (endpoint: %s)\n", config->peerEndpoint);
    return true;
}

void clearVPNConfig() {
    if (!vpnPrefs.begin(VPN_NVS_NAMESPACE, false)) {
        return;
    }
    vpnPrefs.clear();
    vpnPrefs.end();
    Logger.println("🗑️ VPN: NVS config cleared");
}

bool isVPNConfigured() {
    VPNConfig config;
    if (loadVPNConfig(&config) && config.configured) {
        return true;
    }
    
#if defined(WIREGUARD_PRIVATE_KEY) && defined(WIREGUARD_PEER_PUBLIC_KEY) && \
    defined(WIREGUARD_PEER_ENDPOINT) && defined(WIREGUARD_LOCAL_IP)
    return true;
#else
    return false;
#endif
}

// ============================================================================
// VPN Web Configuration Page
// ============================================================================

static const char VPN_CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<title>VPN Configuration</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px}
.c{max-width:500px;margin:auto;background:#16213e;padding:20px;border-radius:12px;border:1px solid #0f3460}
h2{margin:0 0 20px;color:#e94560}
label{display:block;margin:15px 0 5px;color:#a0a0a0;font-size:14px}
input,select{width:100%;padding:10px;margin:0;border:1px solid #0f3460;border-radius:6px;background:#0f0f23;color:#eee;font-family:monospace}
input:focus{outline:none;border-color:#e94560}
button{width:100%;background:#e94560;color:white;padding:12px;border:none;border-radius:25px;cursor:pointer;font-size:16px;margin-top:20px}
button:hover{background:#ff6b6b}
.status{padding:10px;border-radius:6px;margin-bottom:15px;font-size:14px}
.connected{background:rgba(74,222,128,0.2);border-left:3px solid #4ade80}
.disconnected{background:rgba(233,69,96,0.2);border-left:3px solid #e94560}
.help{font-size:12px;color:#666;margin-top:5px}
.btn-clear{background:#666;margin-top:10px}
.back{display:block;text-align:center;margin-top:15px;color:#e94560}
</style>
</head><body>
<div class="c">
<h2>🔐 VPN Configuration</h2>
<div class="status %STATUS_CLASS%">%STATUS%</div>
<form action="/vpn/save" method="POST">
<label>Local IP (your Tailscale IP)</label>
<input type="text" name="localIp" value="%LOCAL_IP%" placeholder="10.0.0.x or 100.x.x.x" required>
<div class="help">Your device's IP on the Tailscale/WireGuard network</div>

<label>Private Key (base64)</label>
<input type="password" name="privateKey" value="%PRIVATE_KEY%" placeholder="Your WireGuard private key" required>
<div class="help">Generate with: wg genkey</div>

<label>Peer Endpoint (hostname or IP)</label>
<input type="text" name="peerEndpoint" value="%PEER_ENDPOINT%" placeholder="relay.tailscale.com" required>
<div class="help">Your Tailscale relay or peer's public address</div>

<label>Peer Public Key (base64)</label>
<input type="text" name="peerPublicKey" value="%PEER_PUBLIC_KEY%" placeholder="Peer's WireGuard public key" required>

<label>Peer Port</label>
<input type="number" name="peerPort" value="%PEER_PORT%" placeholder="41641" min="1" max="65535">
<div class="help">Default: 41641 for Tailscale, 51820 for WireGuard</div>

<button type="submit">💾 Save & Connect</button>
</form>
<form action="/vpn/clear" method="POST">
<button type="submit" class="btn-clear">🗑️ Clear Config (use defaults)</button>
</form>
<a href="/" class="back">← Back</a>
</div>
</body></html>
)rawliteral";

// Web server pointer (set by initVPNConfigRoutes)
static WebServer* vpnWebServer = nullptr;

void handleVPNConfigPage() {
    if (!vpnWebServer) return;
    
    String html = FPSTR(VPN_CONFIG_PAGE);
    
    // Get current config
    VPNConfig config;
    bool hasNvsConfig = loadVPNConfig(&config);
    
    // Status
    if (isTailscaleConnected()) {
        html.replace("%STATUS_CLASS%", "connected");
        html.replace("%STATUS%", String("✅ Connected: ") + getTailscaleIP());
    } else if (vpnInitialized) {
        html.replace("%STATUS_CLASS%", "disconnected");
        html.replace("%STATUS%", "⚠️ Connecting or failed...");
    } else {
        html.replace("%STATUS_CLASS%", "disconnected");
        html.replace("%STATUS%", hasNvsConfig ? "🔧 Configured (not started)" : "❌ Not configured");
    }
    
    // Fill form values
    if (hasNvsConfig) {
        html.replace("%LOCAL_IP%", config.localIp);
        html.replace("%PRIVATE_KEY%", ""); // Don't show private key
        html.replace("%PEER_ENDPOINT%", config.peerEndpoint);
        html.replace("%PEER_PUBLIC_KEY%", config.peerPublicKey);
        html.replace("%PEER_PORT%", String(config.peerPort));
    } else {
        // Use compile-time defaults if available
#ifdef WIREGUARD_LOCAL_IP
        html.replace("%LOCAL_IP%", WIREGUARD_LOCAL_IP);
#else
        html.replace("%LOCAL_IP%", "");
#endif
        html.replace("%PRIVATE_KEY%", "");
#ifdef WIREGUARD_PEER_ENDPOINT
        html.replace("%PEER_ENDPOINT%", WIREGUARD_PEER_ENDPOINT);
#else
        html.replace("%PEER_ENDPOINT%", "");
#endif
#ifdef WIREGUARD_PEER_PUBLIC_KEY
        html.replace("%PEER_PUBLIC_KEY%", WIREGUARD_PEER_PUBLIC_KEY);
#else
        html.replace("%PEER_PUBLIC_KEY%", "");
#endif
        html.replace("%PEER_PORT%", String(WIREGUARD_PEER_PORT));
    }
    
    vpnWebServer->send(200, "text/html", html);
}

void handleVPNSave() {
    if (!vpnWebServer) return;
    
    // Load existing config first to preserve private key if not provided
    VPNConfig existingConfig;
    bool hadExistingConfig = loadVPNConfig(&existingConfig);
    
    VPNConfig config;
    strncpy(config.localIp, vpnWebServer->arg("localIp").c_str(), sizeof(config.localIp) - 1);
    strncpy(config.peerEndpoint, vpnWebServer->arg("peerEndpoint").c_str(), sizeof(config.peerEndpoint) - 1);
    strncpy(config.peerPublicKey, vpnWebServer->arg("peerPublicKey").c_str(), sizeof(config.peerPublicKey) - 1);
    config.peerPort = vpnWebServer->arg("peerPort").toInt();
    if (config.peerPort == 0) config.peerPort = 41641;
    config.configured = true;
    
    // Handle private key - keep existing if not provided
    String newPrivateKey = vpnWebServer->arg("privateKey");
    if (newPrivateKey.length() > 0) {
        strncpy(config.privateKey, newPrivateKey.c_str(), sizeof(config.privateKey) - 1);
    } else if (hadExistingConfig && strlen(existingConfig.privateKey) > 0) {
        strncpy(config.privateKey, existingConfig.privateKey, sizeof(config.privateKey) - 1);
    } else {
        vpnWebServer->send(400, "text/plain", "Private key is required");
        return;
    }
    
    // Validate required fields
    if (strlen(config.localIp) == 0 || strlen(config.peerEndpoint) == 0 || strlen(config.peerPublicKey) == 0) {
        vpnWebServer->send(400, "text/plain", "All fields are required");
        return;
    }
    
    if (saveVPNConfig(&config)) {
        // Disconnect existing VPN and reconnect with new config
        if (vpnInitialized) {
            disconnectTailscale();
        }
        
        vpnWebServer->sendHeader("Location", "/", true);
        vpnWebServer->send(302, "text/plain", "Saved! Reconnecting...");
        
        // Try to connect with new config (after a short delay)
        delay(500);
        initTailscaleFromConfig();
    } else {
        vpnWebServer->send(500, "text/plain", "Failed to save configuration");
    }
}

void handleVPNClear() {
    if (!vpnWebServer) return;
    
    clearVPNConfig();
    
    if (vpnInitialized) {
        disconnectTailscale();
    }
    
    vpnWebServer->sendHeader("Location", "/", true);
    vpnWebServer->send(302, "text/plain", "Config cleared");
}

// Handle toggling Tailscale enabled state via web endpoint
void handleVPNToggle() {
    if (!vpnWebServer) return;
    
    bool newState = toggleTailscaleEnabled();
    
    String json = "{\"enabled\":";
    json += newState ? "true" : "false";
    json += ",\"message\":\"Tailscale ";
    json += newState ? "enabled" : "disabled";
    json += ". Reboot required.\"}";
    
    vpnWebServer->send(200, "application/json", json);
}

// Handle getting current Tailscale status via API
void handleVPNStatus() {
    if (!vpnWebServer) return;
    
    String json = "{";
    json += "\"enabled\":";
    json += isTailscaleEnabled() ? "true" : "false";
    json += ",\"connected\":";
    json += isTailscaleConnected() ? "true" : "false";
    json += ",\"status\":\"" + String(getTailscaleStatus()) + "\"";
    if (isTailscaleConnected()) {
        json += ",\"ip\":\"" + String(getTailscaleIP()) + "\"";
    }
    json += "}";
    
    vpnWebServer->send(200, "application/json", json);
}

void initVPNConfigRoutes(void* server) {
    vpnWebServer = static_cast<WebServer*>(server);
    if (!vpnWebServer) return;
    
    vpnWebServer->on("/vpn", HTTP_GET, handleVPNConfigPage);
    vpnWebServer->on("/vpn/save", HTTP_POST, handleVPNSave);
    vpnWebServer->on("/vpn/clear", HTTP_POST, handleVPNClear);
    vpnWebServer->on("/vpn/toggle", HTTP_GET, handleVPNToggle);
    vpnWebServer->on("/vpn/status", HTTP_GET, handleVPNStatus);
    
    Logger.println("🔐 VPN config routes registered (/vpn, /vpn/toggle, /vpn/status)");
}
