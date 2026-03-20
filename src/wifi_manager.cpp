#include <ESPTelnetStream.h>
#include "wifi_manager.h"
#include "logging.h"
#include "config.h"
#include "tailscale_manager.h"
#include "remote_logger.h"
#include "notifications.h"
#include "phone_home.h"
#include "nvs_flash.h"
#ifndef DIAG_BUILD
#include "extended_audio_player.h"
#endif
#include "special_command_processor.h"  // For shutdownAudioForOTA
#include <SD.h>
#include <SPI.h>
#include "driver/spi_common.h"
#include "soc/spi_reg.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"  // For ESP-IDF OTA info
#include <Update.h>       // For HTTP OTA
#include "http_utils.h"

// Default OTA hostname if not specified in build flags
#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "jump-phone"
#endif
ESPTelnetStream telnet; // Telnet server for remote logging

// WiFi Setup Variables
WebServer server(80);
DNSServer dnsServer;
Preferences wifiPrefs;
bool isConfigMode = false;
unsigned long portalStartTime = 0;

// OTA preparation tracking - reboot if OTA fails or times out
static bool otaPrepared = false;
static unsigned long otaPrepareTime = 0;
static const unsigned long OTA_PREPARE_TIMEOUT_MS = 300000; // 5 minutes

// Track one-time WiFi clear per build version
static bool shouldClearWiFiForBuild()
{
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

    // Ensure NVS is initialized
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    Preferences bootPrefs;
    if (!bootPrefs.begin("bootflags", false)) {
        Logger.println("❌ Failed to open bootflags preferences; will clear WiFi to be safe");
        return true;
    }

    // NVS keys max 15 chars - use "wifi_clr_ver" (12 chars)
    String lastCleared = bootPrefs.getString("wifi_clr_ver", "");
    const String currentVersion = FIRMWARE_VERSION;
    bool shouldClear = (lastCleared != currentVersion);
    if (shouldClear) {
        bootPrefs.putString("wifi_clr_ver", currentVersion);
    }
    bootPrefs.end();
    return shouldClear;
}

// Keep SoftAP alive after STA tests
static void ensureAPAndDNS()
{
    if (WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP);
    }
    WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD);
    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(53, "*", apIP);
}

// WiFi connection callback
static WiFiConnectedCallback wifiConnectedCallback = nullptr;

// WiFi disconnect callback
static WiFiDisconnectedCallback wifiDisconnectedCallback = nullptr;

void setWiFiDisconnectCallback(WiFiDisconnectedCallback callback) {
    wifiDisconnectedCallback = callback;
}

// Escape JSON strings minimally (quotes and backslashes)
static String escapeJson(const String& in)
{
    String out;
    out.reserve(in.length() + 4);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

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

// Handle WiFi scan (JSON)
void handleWiFiScan()
{
    int count = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
    String json = "[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + escapeJson(WiFi.SSID(i)) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    server.send(200, "application/json", json);
}

// Test WiFi credentials without committing/rebooting
void handleWiFiTest()
{
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    if (ssid.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"message\":\"SSID required\"}");
        return;
    }

    // Switch to AP+STA to keep portal alive during test
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD);
    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(53, "*", apIP);

    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    wl_status_t status = WL_IDLE_STATUS;
    while ((millis() - start) < 10000) { // 10s timeout
        status = WiFi.status();
        if (status == WL_CONNECTED) break;
        delay(200);
    }

    bool ok = (status == WL_CONNECTED);
    String message;
    if (ok) {
        message = "Connected. IP " + WiFi.localIP().toString();
    } else {
        message = "Failed (status " + String(static_cast<int>(status)) + ")";
    }

    // Disconnect station and restore AP-only mode for the portal
    WiFi.disconnect(true, true);
    ensureAPAndDNS();

    String resp = "{\"ok\":";
    resp += ok ? "true" : "false";
    resp += ",\"message\":\"" + escapeJson(message) + "\"}";
    server.send(ok ? 200 : 500, "application/json", resp);
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

    // Compile-time defaults for VPN (used when NVS empty)
#ifdef WIREGUARD_LOCAL_IP
    const char* defaultLocalIp = WIREGUARD_LOCAL_IP;
#else
    const char* defaultLocalIp = "";
#endif
#ifdef WIREGUARD_PEER_ENDPOINT
    const char* defaultPeerEndpoint = WIREGUARD_PEER_ENDPOINT;
#else
    const char* defaultPeerEndpoint = "";
#endif
#ifdef WIREGUARD_PEER_PUBLIC_KEY
    const char* defaultPeerPublicKey = WIREGUARD_PEER_PUBLIC_KEY;
#else
    const char* defaultPeerPublicKey = "";
#endif
    uint16_t defaultPeerPort = WIREGUARD_PEER_PORT;

    bool hasPrivateKey = (hasVpnConfig && strlen(vpnConfig.privateKey) > 0);
#ifdef WIREGUARD_PRIVATE_KEY
    if (!hasPrivateKey) {
        hasPrivateKey = true; // compile-time private key available (not shown)
    }
#endif
    
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
<select id="ssid-select"></select>
<input type="hidden" id="ssid-hidden" name="ssid" value=")";
        html += savedSSID;
        html += R"(">
<input type="text" id="ssid-manual" placeholder="Enter SSID" style="display:none;margin-top:8px">
</div>
<div class="field">
<label>WiFi Password</label>
<div class="row">
    <input type="password" id="wifi-password" name="password" placeholder="WiFi Password">
    <button type="button" id="toggle-password" style="max-width:140px">👁️ Show</button>
</div>
</div>
<div class="row">
    <button type="button" id="test-wifi">🧪 Test WiFi</button>
    <button type="submit">💾 Save & Connect WiFi</button>
</div>
<div id="wifi-status" class="status info" style="display:none;margin-top:10px"></div>
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
    html += hasVpnConfig ? vpnConfig.localIp : defaultLocalIp;
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
    html += hasVpnConfig ? vpnConfig.peerEndpoint : defaultPeerEndpoint;
    html += R"(" required>
</div>
<div class="field">
<label>Peer Public Key</label>
<input type="text" name="peerPublicKey" placeholder="Peer's public key" value=")";
    html += hasVpnConfig ? vpnConfig.peerPublicKey : defaultPeerPublicKey;
    html += R"(" required>
</div>
<div class="field">
<label>Peer Port</label>
<input type="number" name="peerPort" placeholder="41641" value=")";
    html += hasVpnConfig ? String(vpnConfig.peerPort) : String(defaultPeerPort);
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
<script>
    const savedSSID = ")";
        html += escapeJson(savedSSID);
        html += R"(";
    const ssidSelect = document.getElementById('ssid-select');
    const ssidHidden = document.getElementById('ssid-hidden');
    const ssidManual = document.getElementById('ssid-manual');
    const pwdInput = document.getElementById('wifi-password');
    const togglePwd = document.getElementById('toggle-password');
    const testBtn = document.getElementById('test-wifi');
    const wifiStatus = document.getElementById('wifi-status');

    function setStatus(msg, ok) {
        wifiStatus.textContent = msg;
        wifiStatus.className = 'status ' + (ok ? 'connected' : 'disconnected');
        wifiStatus.style.display = 'block';
    }

    function populateSSIDs(list) {
        ssidSelect.innerHTML = '';
        const placeholder = document.createElement('option');
        placeholder.textContent = 'Select network';
        placeholder.disabled = true; placeholder.selected = true;
        ssidSelect.appendChild(placeholder);

        list.forEach(item => {
            const opt = document.createElement('option');
            opt.value = item.ssid;
            opt.textContent = `${item.ssid} ${item.secure ? '🔒' : ''} (${item.rssi} dBm)`;
            ssidSelect.appendChild(opt);
        });

        const other = document.createElement('option');
        other.value = '__other__';
        other.textContent = 'Other (enter manually)';
        ssidSelect.appendChild(other);

        // Preselect saved SSID if present
        if (savedSSID) {
            const match = Array.from(ssidSelect.options).find(o => o.value === savedSSID);
            if (match) {
                match.selected = true;
                ssidHidden.value = savedSSID;
            } else {
                other.selected = true;
                ssidManual.style.display = 'block';
                ssidManual.value = savedSSID;
                ssidHidden.value = savedSSID;
            }
        }
    }

    function loadSSIDs() {
        ssidSelect.innerHTML = '<option>Scanning...</option>';
        fetch('/wifi/scan').then(r => r.json()).then(data => {
            populateSSIDs(data);
        }).catch(() => {
            populateSSIDs([]);
            setStatus('Scan failed; enter SSID manually.', false);
        });
    }

    ssidSelect.addEventListener('change', () => {
        if (ssidSelect.value === '__other__') {
            ssidManual.style.display = 'block';
            ssidHidden.value = ssidManual.value;
        } else {
            ssidManual.style.display = 'none';
            ssidHidden.value = ssidSelect.value;
        }
    });

    ssidManual.addEventListener('input', () => {
        ssidHidden.value = ssidManual.value;
    });

    togglePwd.addEventListener('click', () => {
        const showing = pwdInput.type === 'text';
        pwdInput.type = showing ? 'password' : 'text';
        togglePwd.textContent = showing ? '👁️ Show' : '🙈 Hide';
    });

    testBtn.addEventListener('click', () => {
        const formData = new FormData();
        formData.append('ssid', ssidHidden.value);
        formData.append('password', pwdInput.value);
        setStatus('Testing...', true);
        fetch('/wifi/test', { method: 'POST', body: formData })
            .then(r => r.json())
            .then(res => setStatus(res.message || (res.ok ? 'Success' : 'Failed'), !!res.ok))
            .catch(() => setStatus('Test failed (network error)', false));
    });

    // Ensure hidden SSID matches selection before submit
    document.querySelector('form[action="/save"]').addEventListener('submit', () => {
        if (ssidSelect.value === '__other__') {
            ssidHidden.value = ssidManual.value;
        } else {
            ssidHidden.value = ssidSelect.value;
        }
    });

    loadSSIDs();
</script>
</body></html>
")";
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

// ============================================================================
// MULTIPLE FALLBACK WIFI SUPPORT
// ============================================================================

// Structure for WiFi credentials
struct WiFiCredential {
    const char* ssid;
    const char* password;
};

// Fallback WiFi networks - tried in order
// First: NVS saved credentials (if any)
// Then: Compile-time defaults (can define multiple via build flags)
static const WiFiCredential fallbackNetworks[] = {
#ifdef DEFAULT_SSID
    { DEFAULT_SSID, DEFAULT_PASSWORD },
#endif
#ifdef FALLBACK_SSID_1
    { FALLBACK_SSID_1, FALLBACK_PASSWORD_1 },
#endif
#ifdef FALLBACK_SSID_2
    { FALLBACK_SSID_2, FALLBACK_PASSWORD_2 },
#endif
#ifdef FALLBACK_SSID_3
    { FALLBACK_SSID_3, FALLBACK_PASSWORD_3 },
#endif
};
static const int numFallbackNetworks = sizeof(fallbackNetworks) / sizeof(fallbackNetworks[0]);

// Track current network being tried (for fallback enumeration)
static int currentNetworkIndex = -1;  // -1 = trying saved credentials
static bool triedSavedCredentials = false;

// Get next WiFi credentials to try
// Returns false if no more networks to try
bool getNextWiFiCredentials(String& ssid, String& password) {
    // First try: saved credentials from NVS
    if (!triedSavedCredentials) {
        triedSavedCredentials = true;
        
        if (wifiPrefs.begin("wifi", true)) {
            String savedSsid = wifiPrefs.getString("ssid", "");
            String savedPassword = wifiPrefs.getString("password", "");
            wifiPrefs.end();
            
            if (savedSsid.length() > 0) {
                ssid = savedSsid;
                password = savedPassword;
                Logger.printf("📡 Trying saved WiFi: %s\n", ssid.c_str());
                return true;
            }
        }
    }
    
    // Then try: fallback networks from compile-time
    currentNetworkIndex++;
    if (currentNetworkIndex < numFallbackNetworks) {
        ssid = fallbackNetworks[currentNetworkIndex].ssid;
        password = fallbackNetworks[currentNetworkIndex].password;
        Logger.printf("📡 Trying fallback WiFi %d/%d: %s\n", 
            currentNetworkIndex + 1, numFallbackNetworks, ssid.c_str());
        return true;
    }
    
    return false;  // No more networks to try
}

// Reset fallback iteration (call when starting fresh connection attempt)
void resetWiFiFallback() {
    currentNetworkIndex = -1;
    triedSavedCredentials = false;
}

// Check if there are more networks to try
bool hasMoreNetworksToTry() {
    if (!triedSavedCredentials) return true;
    return (currentNetworkIndex + 1) < numFallbackNetworks;
}

// Connect to WiFi using saved credentials or fallbacks
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
    
    // Reset fallback state for fresh attempt
    resetWiFiFallback();
    
    String ssid, password;
    if (!getNextWiFiCredentials(ssid, password)) {
        Logger.println("📡 No WiFi credentials found (no saved or default)");
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

// Register all HTTP server routes - called by both config portal and normal OTA mode
static void registerWebServerRoutes()
{
    // Config UI
    server.on("/", HTTP_GET, handleRoot);
    server.on("/logs", HTTP_GET, handleLogs);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/wifi/clear", HTTP_POST, handleWiFiClear);
    server.on("/wifi/scan", HTTP_GET, handleWiFiScan);
    server.on("/wifi/test", HTTP_POST, handleWiFiTest);

    // Deployment API
    server.on("/api/wifi", HTTP_POST, []() {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        if (ssid.length() == 0) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"SSID required\"}");
            return;
        }
        Logger.printf("📡 API: Saving WiFi credentials for: %s\n", ssid.c_str());
        saveWiFiCredentials(ssid, password);
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Credentials saved, rebooting...\"}");
        delay(500);
        ESP.restart();
    });

    server.on("/api/status", HTTP_GET, []() {
        String json = "{";
        json += "\"ap_name\":\"" + String(WIFI_AP_NAME) + "\",";
        json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
        json += "\"config_mode\":" + String(isConfigMode ? "true" : "false") + ",";
        json += "\"fallback_networks\":" + String(numFallbackNetworks);
        json += "}";
        server.send(200, "application/json", json);
    });

    // OTA preparation endpoint - call before OTA to release SD/SPI
    server.on("/prepareota", HTTP_GET, []() {
        Logger.println("🔄 HTTP: Preparing for OTA update...");
        shutdownAudioForOTA();
        delay(100);
        SD.end();  // Unmount SD card only - don't touch SPI or GPIO!
        delay(500);
        otaPrepared = true;
        otaPrepareTime = millis();
        Logger.println("✅ HTTP: Ready for OTA (5 min timeout)");
        server.send(200, "text/plain", "OK - Ready for OTA (5 min timeout, will reboot if no OTA)");
    });

    // HTTP OTA upload endpoint
    // Use: curl -F "firmware=@.pio/build/esp32dev/firmware.bin" http://DEVICE_IP/update
    server.on("/update", HTTP_POST, []() {
        if (Update.hasError()) {
            server.send(500, "text/plain", "FAIL - Update error");
            delay(1000);
            esp_restart();
        } else {
            server.send(200, "text/plain", "OK - Update successful, rebooting...");
            delay(1000);
            esp_restart();
        }
    }, []() {
        HTTPUpload& upload = server.upload();
        static bool otaBeginOk = false;
        if (upload.status == UPLOAD_FILE_START) {
            Logger.printf("🔄 HTTP OTA: Receiving %s\n", upload.filename.c_str());
            // Flush remote logs before OTA overwrites firmware
            Logger.flush();
            esp_task_wdt_delete(NULL);
            if (!otaPrepared) {
                shutdownAudioForOTA();
                delay(100);
                SD.end();  // Unmount SD card only - don't touch SPI or GPIO!
                delay(500);
            } else {
                Logger.println("ℹ️ OTA already prepared, skipping shutdown");
            }
            const esp_partition_t* otaPart = esp_ota_get_next_update_partition(NULL);
            if (otaPart) {
                Logger.printf("📋 OTA target partition: %s, size: %u bytes (%u KB)\n",
                    otaPart->label, otaPart->size, otaPart->size / 1024);
            } else {
                Logger.println("⚠️ No OTA partition found!");
            }
            Logger.printf("📋 Free heap: %u bytes, largest block: %u bytes\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            size_t firmwareSize = UPDATE_SIZE_UNKNOWN;
            if (server.hasArg("size")) {
                firmwareSize = server.arg("size").toInt();
            }
            Logger.printf("📦 HTTP OTA: Firmware size: %s (%u bytes)\n",
                firmwareSize == UPDATE_SIZE_UNKNOWN ? "unknown" : "explicit", firmwareSize);
            otaBeginOk = Update.begin(firmwareSize);
            if (!otaBeginOk) {
                Logger.printf("❌ HTTP OTA: Begin failed: %s\n", Update.errorString());
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (!otaBeginOk) return;
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Logger.printf("❌ HTTP OTA: Write failed: %s\n", Update.errorString());
            } else {
                static int lastPercent = -1;
                size_t total = Update.size();
                if (total > 0) {
                    int percent = (Update.progress() * 100) / total;
                    if (percent != lastPercent && percent % 10 == 0) {
                        Logger.printf("📤 HTTP OTA Progress: %d%%\n", percent);
                        lastPercent = percent;
                    }
                }
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (!otaBeginOk) {
                // Upload ended but never started successfully — restore WDT
                esp_task_wdt_add(NULL);
                return;
            }
            if (Update.end(true)) {
                Logger.printf("✅ HTTP OTA: Complete (%u bytes)\n", upload.totalSize);
            } else {
                Logger.printf("❌ HTTP OTA: End failed: %s\n", Update.errorString());
                // Flash failed — restore WDT so main loop stays protected
                esp_task_wdt_add(NULL);
            }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            // Connection dropped mid-upload — restore WDT
            Logger.println("⚠️ HTTP OTA: Upload aborted");
            esp_task_wdt_add(NULL);
        }
    });

    // VPN toggle endpoints
    server.on("/vpn/on", HTTP_GET, []() {
        Logger.println("🔐 HTTP: Enabling WireGuard VPN...");
        if (initTailscaleFromConfig()) {
            server.send(200, "text/plain", "OK - VPN enabled");
        } else {
            server.send(500, "text/plain", "FAIL - VPN init failed");
        }
    });

    server.on("/vpn/off", HTTP_GET, []() {
        Logger.println("🔓 HTTP: Disabling WireGuard VPN...");
        disconnectTailscale();
        server.send(200, "text/plain", "OK - VPN disabled");
    });

    server.on("/vpn/status", HTTP_GET, []() {
        bool connected = isTailscaleConnected();
        String status = connected ? "connected" : "disconnected";
        String ip = connected ? getTailscaleIP() : "N/A";
        server.send(200, "application/json",
            "{\"vpn\":\"" + status + "\",\"ip\":\"" + ip + "\"}");
    });

    // Device status endpoint
    server.on("/status", HTTP_GET, []() {
        String json = "{";
        json += "\"wifi_ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"vpn_connected\":" + String(isTailscaleConnected() ? "true" : "false") + ",";
        json += "\"vpn_ip\":\"";
        const char* vpnIp = getTailscaleIP();
        json += (vpnIp != nullptr) ? vpnIp : "N/A";
        json += "\",";
        json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"heap_largest_block\":" + String(ESP.getMaxAllocHeap()) + ",";
        const esp_partition_t* runPart = esp_ota_get_running_partition();
        const esp_partition_t* nextPart = esp_ota_get_next_update_partition(NULL);
        if (runPart) {
            json += "\"running_partition\":\"" + String(runPart->label) + "\",";
            json += "\"running_partition_size\":" + String(runPart->size) + ",";
        }
        if (nextPart) {
            json += "\"ota_partition\":\"" + String(nextPart->label) + "\",";
            json += "\"ota_partition_size\":" + String(nextPart->size) + ",";
        }
        json += "\"uptime\":" + String(millis() / 1000);
        json += "}";
        server.send(200, "application/json", json);
    });

    // Reboot endpoint
    server.on("/reboot", HTTP_GET, []() {
        server.send(200, "text/plain", "OK - Rebooting...");
        delay(500);
        esp_restart();
    });

    // File upload endpoint — write raw data to SD card
    // Usage: curl -X POST --data-binary @debug_audio.raw http://DEVICE_IP/upload/debug_audio.raw
    server.on("/upload", HTTP_POST, []() {
        // Final response sent in upload handler
    }, []() {
        static File uploadFile;
        static size_t totalWritten;
        HTTPUpload& upload = server.upload();

        if (upload.status == UPLOAD_FILE_START) {
            // Use the uploaded filename as the SD path
            String path = "/" + upload.filename;
            Logger.printf("📤 Upload start: %s → SD:%s\n", upload.filename.c_str(), path.c_str());
            uploadFile = SD_MMC.open(path.c_str(), FILE_WRITE);
            totalWritten = 0;
            if (!uploadFile) {
                Logger.printf("❌ Cannot open %s for writing\n", path.c_str());
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (uploadFile) {
                size_t written = uploadFile.write(upload.buf, upload.currentSize);
                totalWritten += written;
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (uploadFile) {
                uploadFile.close();
                Logger.printf("✅ Upload complete: %u bytes → SD\n", (unsigned)totalWritten);
                server.send(200, "application/json",
                    "{\"ok\":true,\"bytes\":" + String(totalWritten) + "}");
            } else {
                server.send(500, "application/json",
                    "{\"ok\":false,\"error\":\"Failed to open file on SD\"}");
            }
        }
    });

    // File delete endpoint — remove a file from the SD card
    // Usage: curl -X DELETE "http://DEVICE_IP/delete?path=/audio/audio_40a38a0f.m4a"
    server.on("/delete", HTTP_DELETE, []() {
        if (!server.hasArg("path")) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing 'path' parameter\"}");
            return;
        }
        String path = server.arg("path");
        if (path.indexOf("..") >= 0) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid path\"}");
            return;
        }
        if (!SD_MMC.exists(path.c_str())) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"File not found\"}");
            return;
        }
        if (SD_MMC.remove(path.c_str())) {
            Logger.printf("🗑️ Deleted file: %s\n", path.c_str());
            server.send(200, "application/json", "{\"ok\":true}");
        } else {
            server.send(500, "application/json", "{\"ok\":false,\"error\":\"Failed to delete file\"}");
        }
    });

    initVPNConfigRoutes(&server);
    initRemoteLoggerRoutes(&server);
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
    
    // Register all web server routes
    registerWebServerRoutes();
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
    server.on("/wifi/scan", HTTP_GET, handleWiFiScan);
    server.on("/wifi/test", HTTP_POST, handleWiFiTest);
    server.on("/logs", handleLogs);
    initVPNConfigRoutes(&server);
    initRemoteLoggerRoutes(&server);
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
    // Check if Tailscale/VPN should be enabled (checks compile-time flag and saved state)
    shouldEnableTailscale();

    // Check for compile-time flag to clear WiFi credentials (once per build)
#ifdef CLEAR_WIFI_ON_BOOT
    if (shouldClearWiFiForBuild()) {
        Logger.println("⚠️ CLEAR_WIFI_ON_BOOT flag set - clearing saved WiFi credentials for this build");
        clearWiFiCredentials();
        Logger.println("⚠️ CLEAR_WIFI_ON_BOOT flag set - clearing WireGuard/VPN config for this build");
        clearVPNConfig();
    } else {
        Logger.println("ℹ️ CLEAR_WIFI_ON_BOOT already applied for this build; skipping clear");
    }
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
        // WiFi connection status will be handled in handleNetworkLoop()
        isConfigMode = false;
    }
    
    // Initialize OTA update callbacks (actual service starts when WiFi connects)
    initOTA();
    
    Logger.println("📡 WiFi initialization complete - connection status will be monitored in background");
}

// Configure Over-The-Air (OTA) updates - setup only, begin() called when WiFi ready
void initOTA()
{
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setPort(OTA_PORT);
    
    // Prepare system before OTA flash write
    ArduinoOTA.onStart([]() { 
        Logger.println("🔄 OTA Start - TCP callback connected, preparing system...");
        Logger.printf("   WiFi IP: %s, WG: %s, Heap: %u\n",
            WiFi.localIP().toString().c_str(),
            isTailscaleConnected() ? getTailscaleIP() : "N/A",
            ESP.getFreeHeap());
        
        // Flush remote logs so the server has the latest before we reboot
        Logger.flush();
        
        // Clear OTA prepare timeout - we're now in an actual OTA
        otaPrepared = false;
        
        // Disable watchdog to give more time for flash operations
        esp_task_wdt_delete(NULL);
        
        // Shut down audio and SD - ONLY SD.end(), no SPI/GPIO manipulation
        shutdownAudioForOTA();
        SD.end();
        delay(500);  // Let SD card fully release
        
        Logger.println("✅ System prepared for OTA");
        Logger.println("⏳ Starting flash write...");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static int lastPercent = -1;
        int percent = (progress / (total / 100));
        if (percent != lastPercent && percent % 10 == 0) {
            Logger.printf("📤 OTA Progress: %u%%\n", percent);
            lastPercent = percent;
        }
    });
    ArduinoOTA.onEnd([]() { Logger.println("✅ OTA End - Rebooting..."); });
    ArduinoOTA.onError([](ota_error_t error) { 
        const char* errMsg = "Unknown";
        switch (error) {
            case OTA_AUTH_ERROR: errMsg = "Auth Failed"; break;
            case OTA_BEGIN_ERROR: errMsg = "Begin Failed"; break;
            case OTA_CONNECT_ERROR: errMsg = "Connect Failed (TCP callback to sender)"; break;
            case OTA_RECEIVE_ERROR: errMsg = "Receive Failed"; break;
            case OTA_END_ERROR: errMsg = "End Failed"; break;
        }
        Logger.printf("❌ OTA Error[%u]: %s\n", error, errMsg);
        Logger.printf("   WiFi: %s, WG: %s, Heap: %u\n",
            WiFi.localIP().toString().c_str(),
            isTailscaleConnected() ? getTailscaleIP() : "N/A",
            ESP.getFreeHeap());
        Logger.println("   Rebooting in 3 seconds...");
        delay(3000);
        esp_restart();
    });
    
    Logger.println("🔄 OTA configuration complete - will start when WiFi is ready");
}

// Start OTA service and minimal web server when WiFi is ready
void startOTA()
{
    ArduinoOTA.begin();
    
    // Register all web server routes
    registerWebServerRoutes();

    server.begin();
    Logger.printf("✅ OTA Ready: %s:%d\n", WiFi.localIP().toString().c_str(), OTA_PORT);
    Logger.println("🌐 HTTP server started");
}

// Stop OTA service when WiFi changes
void stopOTA()
{
    ArduinoOTA.end();
    Logger.println("🔄 OTA stopped due to WiFi change");
}

// Pull-based OTA: Download and install firmware from a URL
// This works over WireGuard VPN because it's an OUTBOUND connection
bool performPullOTA(const char* firmwareUrl)
{
    Logger.printf("🔄 Pull OTA: Fetching firmware from %s\n", firmwareUrl);
    
    // Prepare system for OTA - ONLY SD.end(), no SPI/GPIO manipulation
    esp_task_wdt_delete(NULL);
    shutdownAudioForOTA();
    SD.end();
    delay(500);  // Let SD card fully release
    
    const esp_partition_t* otaPart = esp_ota_get_next_update_partition(NULL);
    if (otaPart) {
        Logger.printf("📋 OTA target partition: %s, size: %u bytes (%u KB)\n",
            otaPart->label, otaPart->size, otaPart->size / 1024);
    } else {
        Logger.println("⚠️ No OTA partition found!");
    }
    Logger.printf("📋 Free heap: %u bytes, largest block: %u bytes\n",
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    
    HttpClient http(HTTP_TIMEOUT_OTA_MS);
    int written = http.getUpdate(firmwareUrl);
    if (written < 0) {
        // BUG-6 fix: restore WDT so the main loop is still protected
        esp_task_wdt_add(NULL);
        return false;
    }
    
    Logger.println("🔄 Rebooting in 2 seconds...");
    delay(2000);
    esp_restart();
    return true;  // Won't reach here
}

// Set OTA prepare timeout - call from serial command or HTTP endpoint
void setOtaPrepareTimeout()
{
    otaPrepared = true;
    otaPrepareTime = millis();
    Logger.println("⏱️ OTA prepare timeout set (5 minutes)");
}

// Handle WiFi loop processing (call this in main loop)
void handleNetworkLoop()
{
    static bool connectionLogged = false;
    static bool otaStarted = false;
    static unsigned long connectionStartTime = 0;
    // Handle phone home periodic check-in (for remote OTA, status, etc.)

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
            
            // OTA rollback protection: confirm this firmware is good
            // If we booted from a new OTA partition and WiFi works, mark it valid
            // so the bootloader won't roll back to the previous partition
            esp_ota_mark_app_valid_cancel_rollback();
            Logger.println("✅ OTA rollback protection: firmware marked valid");
            
            // Configure public DNS servers (Google + Cloudflare) for reliable resolution
            IPAddress dns1 = DNS_PRIMARY_IPADDRESS;
            IPAddress dns2 = DNS_SECONDARY_IPADDRESS;
            WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
            Logger.printf("🌐 DNS configured: %s, %s\n", dns1.toString().c_str(), dns2.toString().c_str());
            
            // Notify WiFi connected (turns on green LED)
            notify(NotificationType::WiFiConnected, true);
            
            // Initialize Tailscale VPN FIRST (if enabled) to ensure remote access
            // This ensures we can always reach the device via WireGuard for OTA updates
            if (isTailscaleEnabled()) {
                Logger.println("🔐 WiFi connected - initializing Tailscale VPN...");
                initTailscaleFromConfig();
                Logger.println("✅ Tailscale VPN initialized - device should be reachable");
                
                // Initialize remote logging (sends logs to server over VPN)
                initRemoteLogger();
            } else {
                Logger.println("🌐 Tailscale skipped (not enabled)");
            }
            initTelnet();
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
            // WiFi just disconnected - call disconnect callback if we were connected
            if (connectionLogged) {
                Logger.println("📵 WiFi disconnected");
                // Notify WiFi disconnected (turns off green LED)
                notify(NotificationType::WiFiConnected, false);
                if (wifiDisconnectedCallback != nullptr) {
                    wifiDisconnectedCallback();
                }
                connectionLogged = false;
            }
            connectionStartTime = millis();
        }
        else if (WiFi.status() != WL_CONNECTED && connectionStartTime > 0 && 
                 (millis() - connectionStartTime) > 15000) // 15 second timeout per network
        {
            // Try next fallback network if available
            String nextSsid, nextPassword;
            if (hasMoreNetworksToTry() && getNextWiFiCredentials(nextSsid, nextPassword)) {
                Logger.printf("📡 Connection timeout, trying next network: %s\n", nextSsid.c_str());
                WiFi.disconnect(true);
                delay(500);
                WiFi.begin(nextSsid.c_str(), nextPassword.c_str());
                connectionStartTime = millis();  // Reset timeout for new network
            } else {
                Logger.println("❌ WiFi connection timeout (all networks tried) - starting configuration portal");
                
                // Stop OTA if it was running in STA mode
                if (otaStarted)
                {
                    stopOTA();
                    otaStarted = false;
                }
                
                connectionStartTime = 0;
                connectionLogged = false;
                
                // Reset fallback for next attempt
                resetWiFiFallback();
                
                // Start config portal since connection failed
                if (startConfigPortalSafe()) {
                    portalStartTime = millis();
                    // OTA will be restarted in AP mode above
                }
            }
        }
    }
    
    #if ENABLE_REMOTE_UPDATES
    checkForRemoteUpdates();
    #endif

    // Handle OTA updates (only if started and WiFi is ready)
    if (otaStarted && (WiFi.status() == WL_CONNECTED || isConfigMode))
    {
        // Periodic OTA/network diagnostic heartbeat (every 60s)
        static unsigned long lastOtaDiag = 0;
        unsigned long otaDiagNow = millis();
        if (otaDiagNow - lastOtaDiag >= 60000) {
            lastOtaDiag = otaDiagNow;
            Logger.printf("💓 OTA: %s:%d | WG: %s | uptime: %lus | RSSI: %d dBm | Heap: %u\n",
                WiFi.localIP().toString().c_str(), OTA_PORT,
                isTailscaleConnected() ? getTailscaleIP() : "OFF",
                otaDiagNow / 1000, WiFi.RSSI(),
                ESP.getFreeHeap());
        }
        
        ArduinoOTA.handle();
        // Also handle HTTP server requests in STA mode (for OTA, VPN, status endpoints)
        if (!isConfigMode) {
            server.handleClient();
        }
    }
    
    // Check OTA prepare timeout - reboot if no OTA received within timeout
    if (otaPrepared && (millis() - otaPrepareTime > OTA_PREPARE_TIMEOUT_MS))
    {
        Logger.println("⏰ OTA prepare timeout - no OTA received. Rebooting...");
        delay(1000);
        esp_restart();
    }

    handleTailscaleLoop();
    // Handle telnet server (process incoming connections)
    telnet.loop();
}

void initTelnet()
{
    // Start telnet server for remote logging
    telnet.onConnect([](String ip)
                     {
                         Logger.printf("📡 Telnet client connected from: %s\n", ip.c_str());
                         Logger.printf("🔧 Firmware: %s  Build: %s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);
                         Logger.addLogger(telnet); // Add telnet as a log output stream
                         addDebugStream(telnet);
                     });
    telnet.onConnectionAttempt([](String ip)
                               { Logger.printf("📡 Telnet connection attempt from: %s\n", ip.c_str()); });
    telnet.onReconnect([](String ip)
                       { Logger.printf("📡 Telnet client reconnected from: %s\n", ip.c_str()); });
    telnet.onDisconnect([](String ip)
                        {
                            Logger.printf("📡 Telnet client disconnected from: %s\n", ip.c_str());
                            Logger.removeLogger(telnet); // Remove from logger streams
                            removeDebugStream(telnet);
                        });

    if (telnet.begin(23))
    {
        Logger.println("✅ Telnet server started on port 23");
    }
    else
    {
        Logger.println("❌ Failed to start telnet server");
    }
}
