#include "wifi_manager.h"
#include "logging.h"
#include "config.h"
#include "tailscale_manager.h"
#include "nvs_flash.h"

// WiFi Setup Variables
WebServer server(80);
DNSServer dnsServer;
Preferences wifiPrefs;
bool isConfigMode = false;
unsigned long portalStartTime = 0;

// WiFi connection callback
static WiFiConnectedCallback wifiConnectedCallback = nullptr;

// Save WiFi credentials to preferences
void saveWiFiCredentials(const String& ssid, const String& password)
{
    // Ensure NVS is initialized
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Logger.println("⚠️ NVS partition issue, erasing and reinitializing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    if (!wifiPrefs.begin("wifi", false))
    {
        Logger.println("❌ Failed to open WiFi preferences for writing");
        return;
    }
    
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("password", password);
    wifiPrefs.end();
    
    Logger.printf("✅ WiFi credentials saved for SSID: %s\n", ssid.c_str());
}

// Clear WiFi credentials from preferences
void clearWiFiCredentials()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    if (!wifiPrefs.begin("wifi", false))
    {
        Logger.println("❌ Failed to open WiFi preferences for clearing");
        return;
    }
    
    wifiPrefs.clear();
    wifiPrefs.end();
    
    Logger.println("🗑️ WiFi credentials cleared");
}

// Get saved SSID (returns empty string if not set)
String getSavedSSID()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    if (!wifiPrefs.begin("wifi", true)) {
        return "";
    }
    
    String ssid = wifiPrefs.getString("ssid", "");
    wifiPrefs.end();
    return ssid;
}

// Handle logs page request
void handleLogs()
{
    String html = Logger.getLogsAsHtml();
    server.send(200, "text/html", html);
}

// Handle WiFi credential reset
void handleWiFiClear()
{
    clearWiFiCredentials();
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "WiFi credentials cleared");
}

// Build the combined configuration page
String buildConfigPage()
{
    // Get current status info
    String currentIP = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    String savedSSID = getSavedSSID();
    String wifiMode = isConfigMode ? "AP Mode" : (WiFi.status() == WL_CONNECTED ? "Connected" : "Connecting...");
    
    // Get VPN status
    bool vpnConnected = isTailscaleConnected();
    const char* vpnIP = getTailscaleIP();
    VPNConfig vpnConfig;
    bool hasVpnConfig = loadVPNConfig(&vpnConfig);
    bool hasPrivateKey = hasVpnConfig && strlen(vpnConfig.privateKey) > 0;
    
    String html = R"(
<!DOCTYPE html><html><head><title>Bowie Phone Config</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px}
.c{max-width:500px;margin:auto}
.card{background:#16213e;padding:20px;border-radius:12px;border:1px solid #0f3460;margin-bottom:20px}
h2{margin:0 0 15px;color:#e94560;font-size:1.3em}
h3{margin:15px 0 10px;color:#4ade80;font-size:1.1em}
label{display:block;margin:10px 0 5px;color:#a0a0a0;font-size:14px}
input{width:100%;padding:10px;margin:0;border:1px solid #0f3460;border-radius:6px;background:#0f0f23;color:#eee;font-family:monospace;box-sizing:border-box}
input:focus{outline:none;border-color:#e94560}
button{width:100%;background:#e94560;color:white;padding:12px;border:none;border-radius:25px;cursor:pointer;font-size:16px;margin-top:15px}
button:hover{background:#ff6b6b}
.status{padding:10px;border-radius:6px;margin-bottom:15px;font-size:14px}
.connected{background:rgba(74,222,128,0.2);border-left:3px solid #4ade80}
.disconnected{background:rgba(233,69,96,0.2);border-left:3px solid #e94560}
.info{background:rgba(59,130,246,0.2);border-left:3px solid #3b82f6}
.help{font-size:12px;color:#666;margin-top:5px}
.btn-clear{background:#666;margin-top:10px}
.btn-logs{background:#28a745}
.row{display:flex;gap:10px}
.row>*{flex:1}
.field{margin-bottom:10px}
.key-status{color:#4ade80;font-size:12px}
</style>
</head><body>
<div class="c">
<h2>📱 Bowie Phone Configuration</h2>

<!-- System Status -->
<div class="card">
<h3>📊 System Status</h3>
<div class="status info">
<strong>Current IP:</strong> )";
    html += currentIP;
    html += R"(<br>
<strong>WiFi:</strong> )";
    html += wifiMode;
    if (savedSSID.length() > 0) {
        html += " (";
        html += savedSSID;
        html += ")";
    }
    html += R"(<br>
<strong>VPN:</strong> )";
    if (vpnConnected) {
        html += "Connected (";
        html += vpnIP ? vpnIP : "?";
        html += ")";
    } else {
        html += hasVpnConfig ? "Configured" : "Not configured";
    }
    html += R"(
</div>
</div>

<!-- WiFi Configuration -->
<div class="card">
<h3>📶 WiFi Configuration</h3>
<form action="/save" method="POST">
<div class="field">
<label>WiFi SSID</label>
<input type="text" name="ssid" placeholder="WiFi Network Name" value=")";
    html += savedSSID;
    html += R"(" required>
</div>
<div class="field">
<label>WiFi Password</label>
<input type="password" name="password" placeholder="WiFi Password">
</div>
<button type="submit">💾 Save & Connect WiFi</button>
</form>
<form action="/wifi/clear" method="POST">
<button type="submit" class="btn-clear">🗑️ Clear WiFi Settings</button>
</form>
</div>

<!-- VPN Configuration -->
<div class="card">
<h3>🔐 VPN Configuration</h3>
<div class="status )";
    html += vpnConnected ? "connected" : "disconnected";
    html += R"(">)";
    if (vpnConnected) {
        html += "✅ VPN Connected: ";
        html += vpnIP ? vpnIP : "";
    } else if (hasVpnConfig) {
        html += "🔧 Configured (not connected)";
    } else {
        html += "❌ Not configured";
    }
    html += R"(</div>
<form action="/vpn/save" method="POST">
<div class="field">
<label>Local IP (your Tailscale IP)</label>
<input type="text" name="localIp" placeholder="10.x.x.x" value=")";
    html += hasVpnConfig ? vpnConfig.localIp : "";
    html += R"(" required>
</div>
<div class="field">
<label>Private Key (base64)</label>)";
    if (hasPrivateKey) {
        html += R"(<span class="key-status">✓ Key is set</span>)";
    }
    html += R"(
<input type="password" name="privateKey" placeholder=")";
    html += hasPrivateKey ? "Enter new key to change" : "Your WireGuard private key";
    html += R"(")";
    html += hasPrivateKey ? "" : " required";
    html += R"(>
<div class="help">Leave blank to keep existing key</div>
</div>
<div class="field">
<label>Peer Endpoint</label>
<input type="text" name="peerEndpoint" placeholder="relay.tailscale.com" value=")";
    html += hasVpnConfig ? vpnConfig.peerEndpoint : "";
    html += R"(" required>
</div>
<div class="field">
<label>Peer Public Key</label>
<input type="text" name="peerPublicKey" placeholder="Peer's public key" value=")";
    html += hasVpnConfig ? vpnConfig.peerPublicKey : "";
    html += R"(" required>
</div>
<div class="field">
<label>Peer Port</label>
<input type="number" name="peerPort" placeholder="41641" value=")";
    html += hasVpnConfig ? String(vpnConfig.peerPort) : "41641";
    html += R"(">
</div>
<button type="submit">💾 Save VPN Config</button>
</form>
<form action="/vpn/clear" method="POST">
<button type="submit" class="btn-clear">🗑️ Clear VPN Config</button>
</form>
</div>

<!-- Logs -->
<div class="card">
<a href="/logs"><button class="btn-logs">📄 View System Logs</button></a>
</div>

</div>
</body></html>
)";
    return html;
}

// Web server handlers for WiFi configuration
void handleRoot()
{
    server.send(200, "text/html", buildConfigPage());
}

void handleSave()
{
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    
    if (ssid.length() > 0)
    {
        saveWiFiCredentials(ssid, password);
        
        server.send(200, "text/plain", "Connecting to " + ssid + "...\nDevice will restart.");
        
        delay(1000);
        isConfigMode = false;
        ESP.restart();
    }
    else
    {
        server.send(400, "text/plain", "SSID required");
    }
}

// Connect to WiFi using saved credentials
bool connectToWiFi()
{
    // Ensure NVS is initialized
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Logger.println("⚠️ NVS partition issue, erasing and reinitializing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        Logger.printf("❌ NVS init failed: %d\n", err);
    }
    
    if (!wifiPrefs.begin("wifi", true)) // Read-only
    {
        Logger.println("ℹ️ No WiFi preferences found (first boot?)");
        return false;
    }

    String ssid = wifiPrefs.getString("ssid", DEFAULT_SSID);
    String password = wifiPrefs.getString("password", DEFAULT_PASSWORD);
    wifiPrefs.end();
    
    if (ssid.length() == 0)
    {
        Logger.println("📡 No saved WiFi credentials found");
        return false;
    }
    
    Logger.printf("📡 Starting WiFi connection to: %s\n", ssid.c_str());
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Don't wait for connection - let main loop handle status
    Logger.println("📡 WiFi connection initiated in background");
    
    // Return true to indicate credentials were present and connection attempt started
    return true;
}

// Safer version of configuration portal startup
bool startConfigPortalSafe()
{
    Logger.println("🔧 Starting WiFi configuration portal (safe mode)...");
    
    // First, ensure we're in a clean state
    Logger.println("🔧 Disconnecting from any existing WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(2000);
    
    // Try to set mode carefully
    Logger.println("🔧 Setting WiFi mode to AP...");
    for (int retry = 0; retry < 3; retry++) {
        if (WiFi.mode(WIFI_AP)) {
            Logger.println("✅ WiFi mode set to AP");
            break;
        }
        Logger.printf("⚠️ WiFi mode retry %d/3\n", retry + 1);
        delay(1000);
        if (retry == 2) {
            Logger.println("❌ Failed to set WiFi mode after retries");
            return false;
        }
    }
    
    delay(1000);
    
    // Try to start SoftAP carefully
    Logger.println("🔧 Starting SoftAP...");
    for (int retry = 0; retry < 3; retry++) {
        if (WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD)) {
            Logger.println("✅ SoftAP started successfully");
            isConfigMode = true;
            break;
        }
        Logger.printf("⚠️ SoftAP retry %d/3\n", retry + 1);
        delay(1000);
        if (retry == 2) {
            Logger.println("❌ Failed to start SoftAP after retries");
            return false;
        }
    }
    
    delay(1000);
    
    // Now setup the web server and DNS
    IPAddress apIP = WiFi.softAPIP();
    Logger.printf("📡 WiFi configuration portal started\n");
    Logger.printf("AP Name: %s\n", WIFI_AP_NAME);
    Logger.printf("AP Password: %s\n", WIFI_AP_PASSWORD);
    Logger.printf("AP IP: %s\n", apIP.toString().c_str());
    Logger.printf("Connect to '%s' and go to %s to configure WiFi\n", WIFI_AP_NAME, apIP.toString().c_str());
    
    // Start DNS server for captive portal
    dnsServer.start(53, "*", apIP);
    
    // Setup web server routes
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/wifi/clear", HTTP_POST, handleWiFiClear);
    server.on("/logs", handleLogs);
    initVPNConfigRoutes(&server);
    server.onNotFound([]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    
    server.begin();
    Logger.println("📱 Configuration web server started");
    return true;
}

// Start WiFi configuration portal (legacy function)
void startConfigPortal()
{
    if (!startConfigPortalSafe()) {
        Logger.println("❌ Configuration portal startup failed");
        return;
    }
    
    IPAddress apIP = WiFi.softAPIP();
    Logger.printf("📡 WiFi configuration portal started\n");
    Logger.printf("AP Name: %s\n", WIFI_AP_NAME);
    Logger.printf("AP Password: %s\n", WIFI_AP_PASSWORD);
    Logger.printf("AP IP: %s\n", apIP.toString().c_str());
    Logger.printf("Connect to '%s' and go to %s to configure WiFi\n", WIFI_AP_NAME, apIP.toString().c_str());
    
    // Start DNS server for captive portal
    dnsServer.start(53, "*", apIP);
    
    // Setup web server routes
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/wifi/clear", HTTP_POST, handleWiFiClear);
    server.on("/logs", handleLogs);
    initVPNConfigRoutes(&server);
    server.onNotFound([]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    
    server.begin();
    Logger.println("📱 Configuration web server started");
}

// Initialize WiFi with auto-connect or configuration portal
void initWiFi(WiFiConnectedCallback onConnected)
{
    Logger.printf("🔧 Starting WiFi initialization (non-blocking)...\n");
    
    // Check for compile-time flag to clear WiFi credentials
#ifdef CLEAR_WIFI_ON_BOOT
    Logger.println("⚠️ CLEAR_WIFI_ON_BOOT flag set - clearing saved WiFi credentials");
    clearWiFiCredentials();
#endif
    
    // Store the callback for later use
    wifiConnectedCallback = onConnected;
    
    // Try to connect with saved credentials first (non-blocking)
    Logger.println("🔧 Checking for saved credentials...");
    bool hasCredentials = connectToWiFi(); // This now starts connection in background
    
    if (!hasCredentials)
    {
        // No saved credentials - start config portal immediately
        Logger.println("📱 No saved WiFi credentials - starting configuration portal...");
        if (startConfigPortalSafe()) {
            portalStartTime = millis();
        }
    }
    else
    {
        // WiFi connection status will be handled in handleWiFiLoop()
        isConfigMode = false;
    }
    
    Logger.println("📡 WiFi initialization complete - connection status will be monitored in background");
}

// Configure Over-The-Air (OTA) updates - setup only, begin() called when WiFi ready
void initOTA()
{
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setPort(OTA_PORT);
    
    // Minimal callbacks
    ArduinoOTA.onStart([]() { Logger.println("OTA Start"); });
    ArduinoOTA.onEnd([]() { Logger.println("OTA End"); });
    ArduinoOTA.onError([](ota_error_t error) { Logger.printf("OTA Error: %u\n", error); });
    
    Logger.println("🔄 OTA configuration complete - will start when WiFi is ready");
}

// Start OTA service when WiFi is ready
void startOTA()
{
    ArduinoOTA.begin();
    Logger.printf("✅ OTA Ready: %s:%d\n", WiFi.localIP().toString().c_str(), OTA_PORT);
}

// Stop OTA service when WiFi changes
void stopOTA()
{
    ArduinoOTA.end();
    Logger.println("🔄 OTA stopped due to WiFi change");
}

// Handle WiFi loop processing (call this in main loop)
void handleWiFiLoop()
{
    static bool connectionLogged = false;
    static bool otaStarted = false;
    static unsigned long connectionStartTime = 0;
    
    if (isConfigMode)
    {
        // Handle DNS and web server requests
        dnsServer.processNextRequest();
        server.handleClient();
        
        // Start OTA in AP mode if not already started
        if (!otaStarted)
        {
            startOTA();
            otaStarted = true;
        }
        
        // Optional: Log periodic reminder about configuration portal (every 5 minutes)
        if (portalStartTime > 0 && (millis() - portalStartTime) % 300000UL == 0)
        {
            Logger.printf("📱 WiFi configuration portal still active - connect to '%s' to configure\n", WIFI_AP_NAME);
        }
    }
    else if (WiFi.getMode() == WIFI_STA)
    {
        // Check if we're trying to connect and handle status
        if (WiFi.status() == WL_CONNECTED && !connectionLogged)
        {
            Logger.printf("✅ WiFi connected successfully!\n");
            Logger.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
            Logger.printf("Signal Strength: %d dBm\n", WiFi.RSSI());
            
            // Configure public DNS servers (Google + Cloudflare) for reliable resolution
            IPAddress dns1 = DNS_PRIMARY_IPADDRESS;
            IPAddress dns2 = DNS_SECONDARY_IPADDRESS;
            WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
            Logger.printf("🌐 DNS configured: %s, %s\n", dns1.toString().c_str(), dns2.toString().c_str());
            
            // Call the user-provided callback if set
            if (wifiConnectedCallback != nullptr)
            {
                Logger.println("📞 Calling WiFi connected callback...");
                wifiConnectedCallback();
            }
            
            // Start OTA now that we're connected
            if (!otaStarted)
            {
                startOTA();
                otaStarted = true;
            }
            connectionLogged = true;
        }
        else if (WiFi.status() != WL_CONNECTED && connectionStartTime == 0)
        {
            connectionStartTime = millis();
        }
        else if (WiFi.status() != WL_CONNECTED && connectionStartTime > 0 && 
                 (millis() - connectionStartTime) > 30000) // 30 second timeout
        {
            
            Logger.println("❌ WiFi connection timeout (including fallback) - starting configuration portal");
            
            // Stop OTA if it was running in STA mode
            if (otaStarted)
            {
                stopOTA();
                otaStarted = false;
            }
            
            connectionStartTime = 0;
            connectionLogged = false;
            
            // Start config portal since connection failed
            if (startConfigPortalSafe()) {
                portalStartTime = millis();
                // OTA will be restarted in AP mode above
            }
        }
    }
    
    // Handle OTA updates (only if started and WiFi is ready)
    if (otaStarted && (WiFi.status() == WL_CONNECTED || isConfigMode))
    {
        ArduinoOTA.handle();
    }
}