#include "wifi_manager.h"
#include "nvs_flash.h"

// WiFi Setup Variables
WebServer server(80);
DNSServer dnsServer;
Preferences wifiPrefs;
bool isConfigMode = false;

// Save WiFi credentials to preferences
void saveWiFiCredentials(const String& ssid, const String& password)
{
    if (!wifiPrefs.begin("wifi", false))
    {
        Serial.println("❌ Failed to open WiFi preferences for writing");
        return;
    }
    
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("password", password);
    wifiPrefs.end();
    
    Serial.printf("✅ WiFi credentials saved for SSID: %s\n", ssid.c_str());
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
button{width:100%;background:#007cba;color:white;padding:10px;border:none;cursor:pointer}
</style></head><body><div class="c"><h2>Bowie Phone WiFi</h2>
<form action="/save" method="POST">
<input type="text" name="ssid" placeholder="WiFi SSID" required>
<input type="password" name="password" placeholder="Password">
<button type="submit">Connect</button></form></div></body></html>
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
    if (!wifiPrefs.begin("wifi", true)) // Read-only
    {
        Serial.println("❌ Failed to open WiFi preferences");
        return false;
    }
    
    String ssid = wifiPrefs.getString("ssid", "");
    String password = wifiPrefs.getString("password", "");
    wifiPrefs.end();
    
    if (ssid.length() == 0)
    {
        Serial.println("📡 No saved WiFi credentials found");
        return false;
    }
    
    Serial.printf("📡 Attempting to connect to WiFi: %s\n", ssid.c_str());
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    const int maxAttempts = 30; // 15 seconds timeout (500ms * 30)
    
    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts)
    {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.printf("✅ WiFi connected successfully!\n");
        Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("Signal Strength: %d dBm\n", WiFi.RSSI());
        return true;
    }
    else
    {
        Serial.printf("❌ Failed to connect to WiFi: %s\n", ssid.c_str());
        return false;
    }
}

// Safer version of configuration portal startup
bool startConfigPortalSafe()
{
    Serial.println("🔧 Starting WiFi configuration portal (safe mode)...");
    
    // First, ensure we're in a clean state
    Serial.println("🔧 Disconnecting from any existing WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(2000);
    
    // Try to set mode carefully
    Serial.println("🔧 Setting WiFi mode to AP...");
    for (int retry = 0; retry < 3; retry++) {
        if (WiFi.mode(WIFI_AP)) {
            Serial.println("✅ WiFi mode set to AP");
            break;
        }
        Serial.printf("⚠️ WiFi mode retry %d/3\n", retry + 1);
        delay(1000);
        if (retry == 2) {
            Serial.println("❌ Failed to set WiFi mode after retries");
            return false;
        }
    }
    
    delay(1000);
    
    // Try to start SoftAP carefully
    Serial.println("🔧 Starting SoftAP...");
    for (int retry = 0; retry < 3; retry++) {
        if (WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD)) {
            Serial.println("✅ SoftAP started successfully");
            isConfigMode = true;
            break;
        }
        Serial.printf("⚠️ SoftAP retry %d/3\n", retry + 1);
        delay(1000);
        if (retry == 2) {
            Serial.println("❌ Failed to start SoftAP after retries");
            return false;
        }
    }
    
    delay(1000);
    
    // Now setup the web server and DNS
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("📡 WiFi configuration portal started\n");
    Serial.printf("AP Name: %s\n", WIFI_AP_NAME);
    Serial.printf("AP Password: %s\n", WIFI_AP_PASSWORD);
    Serial.printf("AP IP: %s\n", apIP.toString().c_str());
    Serial.printf("Connect to '%s' and go to %s to configure WiFi\n", WIFI_AP_NAME, apIP.toString().c_str());
    
    // Start DNS server for captive portal
    dnsServer.start(53, "*", apIP);
    
    // Setup web server routes
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound([]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    
    server.begin();
    Serial.println("📱 Configuration web server started");
    return true;
}

// Start WiFi configuration portal (legacy function)
void startConfigPortal()
{
    if (!startConfigPortalSafe()) {
        Serial.println("❌ Configuration portal startup failed");
        return;
    }
    
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("📡 WiFi configuration portal started\n");
    Serial.printf("AP Name: %s\n", WIFI_AP_NAME);
    Serial.printf("AP Password: %s\n", WIFI_AP_PASSWORD);
    Serial.printf("AP IP: %s\n", apIP.toString().c_str());
    Serial.printf("Connect to '%s' and go to %s to configure WiFi\n", WIFI_AP_NAME, apIP.toString().c_str());
    
    // Start DNS server for captive portal
    dnsServer.start(53, "*", apIP);
    
    // Setup web server routes
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound([]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    
    server.begin();
    Serial.println("📱 Configuration web server started");
}

// Initialize WiFi with auto-connect or configuration portal
void initWiFi()
{
    Serial.printf("🔧 Starting WiFi initialization...\n");
    
    // Longer initialization delay to allow system stabilization
    delay(3000);
    
    // Initialize NVS if not already done
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        Serial.printf("❌ NVS initialization failed: %s\n", esp_err_to_name(nvs_err));
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            Serial.println("🔄 Erasing NVS flash and retrying...");
            nvs_flash_erase();
            nvs_err = nvs_flash_init();
            if (nvs_err != ESP_OK) {
                Serial.printf("❌ NVS retry failed: %s\n", esp_err_to_name(nvs_err));
                return;
            }
        } else {
            return;
        }
    }
    
    Serial.println("✅ NVS initialized successfully");
    delay(1000);
    
    // Try to connect with saved credentials first
    Serial.println("🔧 Attempting to connect with saved credentials...");
    if (connectToWiFi())
    {
        isConfigMode = false;
        Serial.println("✅ Connected to saved WiFi network");
        return; // Successfully connected
    }
    
    // If connection failed, start configuration portal with more delays
    Serial.println("🔧 No saved credentials found, starting configuration portal...");
    delay(2000);
    
    // Try to start config portal in a more defensive way
    if (!startConfigPortalSafe()) {
        Serial.println("❌ Failed to start configuration portal");
        return;
    }
    
    // Wait for configuration with timeout
    unsigned long portalStartTime = millis();
    unsigned long timeout = WIFI_PORTAL_TIMEOUT * 1000UL;
    
    while (isConfigMode && (millis() - portalStartTime) < timeout)
    {
        dnsServer.processNextRequest();
        server.handleClient();
        delay(10);
    }
    
    if (isConfigMode)
    {
        Serial.printf("⏰ Configuration portal timed out after %d seconds\n", WIFI_PORTAL_TIMEOUT);
        Serial.printf("🔄 Restarting device...\n");
        delay(2000);
        ESP.restart();
    }
}

// Initialize Over-The-Air (OTA) updates - Minimal version
void initOTA()
{
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setPort(OTA_PORT);
    
    // Minimal callbacks
    ArduinoOTA.onStart([]() { Serial.println("OTA Start"); });
    ArduinoOTA.onEnd([]() { Serial.println("OTA End"); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA Error: %u\n", error); });
    
    ArduinoOTA.begin();
    Serial.printf("OTA Ready: %s:%d\n", WiFi.localIP().toString().c_str(), OTA_PORT);
}

// Handle WiFi loop processing (call this in main loop)
void handleWiFiLoop()
{
    if (isConfigMode)
    {
        dnsServer.processNextRequest();
        server.handleClient();
    }
    
    // Handle OTA updates
    ArduinoOTA.handle();
}