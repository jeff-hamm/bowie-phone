#include "wifi_manager.h"
#include "logging.h"
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

// Handle logs page request
void handleLogs()
{
    String html = Logger.getLogsAsHtml();
    server.send(200, "text/html", html);
}

// Web server handlers for WiFi configuration - Minimal version
void handleRoot()
{
    const char* html = R"(
<!DOCTYPE html><html><head><title>WiFi Setup</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font-family:Arial;margin:20px;background:#f0f0f0}
.c{max-width:300px;margin:auto;background:white;padding:20px;border-radius:5px}
input{width:100%;padding:8px;margin:5px 0;border:1px solid #ddd}
button{width:100%;background:#007cba;color:white;padding:10px;border:none;cursor:pointer;margin:5px 0}
.logs-btn{background:#28a745;text-decoration:none;display:block;text-align:center}
</style></head><body><div class="c"><h2>📱 WiFi Config</h2>
<form action="/save" method="POST">
<input type="text" name="ssid" placeholder="WiFi SSID" required>
<input type="password" name="password" placeholder="Password">
<button type="submit">Connect to WiFi</button></form>
<a href="/logs" class="logs-btn button">📄 View System Logs</a>
</div></body></html>
)";
    
    server.send(200, "text/html", html);
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
    
    String ssid = wifiPrefs.getString("ssid", "");
    String password = wifiPrefs.getString("password", "");
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
    server.on("/logs", handleLogs);
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
    server.on("/logs", handleLogs);
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
            Logger.println("❌ WiFi connection timeout - starting configuration portal");
            
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