#include "remote_logger.h"
#include "logging.h"
#include "tailscale_manager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>

// Global instance
RemoteLoggerClass RemoteLogger;

// NVS namespace for remote logger config
#define REMOTE_LOG_NVS_NAMESPACE "remotelog"

static Preferences remoteLogPrefs;

RemoteLoggerClass::RemoteLoggerClass() 
    : lineCount(0), lastFlushTime(0), enabled(false), vpnRequired(true) {
    serverUrl[0] = '\0';
    deviceId[0] = '\0';
    logBuffer.reserve(REMOTE_LOG_BUFFER_SIZE);
}

void RemoteLoggerClass::begin(const char* server, const char* devId, bool requireVpn) {
    vpnRequired = requireVpn;
    
    // Set server URL
    if (server && strlen(server) > 0) {
        setServer(server);
    } else if (strlen(REMOTE_LOG_SERVER) > 0) {
        setServer(REMOTE_LOG_SERVER);
    }
    
    // Set device ID
    if (devId && strlen(devId) > 0) {
        setDeviceId(devId);
    } else if (strlen(REMOTE_LOG_DEVICE_ID) > 0) {
        setDeviceId(REMOTE_LOG_DEVICE_ID);
    } else {
        // Generate device ID from MAC address
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(deviceId, sizeof(deviceId), "phone-%02X%02X%02X", 
                 mac[3], mac[4], mac[5]);
    }
    
    // Only enable if server is configured
    enabled = (strlen(serverUrl) > 0);
    
    if (enabled) {
        Serial.printf("📡 Remote Logger: %s -> %s\n", deviceId, serverUrl);
    }
    
    lastFlushTime = millis();
}

void RemoteLoggerClass::setServer(const char* server) {
    if (server) {
        strncpy(serverUrl, server, sizeof(serverUrl) - 1);
        serverUrl[sizeof(serverUrl) - 1] = '\0';
    }
}

void RemoteLoggerClass::setDeviceId(const char* id) {
    if (id) {
        strncpy(deviceId, id, sizeof(deviceId) - 1);
        deviceId[sizeof(deviceId) - 1] = '\0';
    }
}

size_t RemoteLoggerClass::write(uint8_t byte) {
    if (!enabled) return 1;
    
    logBuffer += (char)byte;
    
    if (byte == '\n') {
        lineCount++;
    }
    
    // Check if we should flush
    if (lineCount >= REMOTE_LOG_BATCH_SIZE || 
        logBuffer.length() >= REMOTE_LOG_BUFFER_SIZE - 256) {
        flush();
    }
    
    return 1;
}

size_t RemoteLoggerClass::write(const uint8_t* buffer, size_t size) {
    if (!enabled) return size;
    
    for (size_t i = 0; i < size; i++) {
        logBuffer += (char)buffer[i];
        if (buffer[i] == '\n') {
            lineCount++;
        }
    }
    
    // Check if we should flush
    if (lineCount >= REMOTE_LOG_BATCH_SIZE || 
        logBuffer.length() >= REMOTE_LOG_BUFFER_SIZE - 256) {
        flush();
    }
    
    return size;
}

void RemoteLoggerClass::loop() {
    if (!enabled || logBuffer.length() == 0) return;
    
    unsigned long now = millis();
    if (now - lastFlushTime >= REMOTE_LOG_FLUSH_INTERVAL_MS) {
        flush();
    }
}

void RemoteLoggerClass::flush() {
    if (logBuffer.length() == 0) return;
    
    // Check VPN requirement
    if (vpnRequired && !isTailscaleConnected()) {
        // Don't flush, keep buffering (up to limit)
        if (logBuffer.length() >= REMOTE_LOG_BUFFER_SIZE - 256) {
            // Buffer full, drop oldest data
            int newlinePos = logBuffer.indexOf('\n');
            if (newlinePos > 0) {
                logBuffer = logBuffer.substring(newlinePos + 1);
                lineCount--;
            }
        }
        return;
    }
    
    // Try to send logs
    if (sendLogs(logBuffer)) {
        logBuffer = "";
        lineCount = 0;
    }
    
    lastFlushTime = millis();
}

bool RemoteLoggerClass::sendLogs(const String& logs) {
    if (!WiFi.isConnected() || strlen(serverUrl) == 0) {
        return false;
    }
    
    HTTPClient http;
    http.setTimeout(5000);  // 5 second timeout
    
    if (!http.begin(serverUrl)) {
        return false;
    }
    
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-ID", deviceId);
    
    // Build JSON payload
    String json = "{";
    json += "\"device\":\"" + String(deviceId) + "\",";
    json += "\"timestamp\":" + String(millis()) + ",";
    json += "\"uptime_sec\":" + String(millis() / 1000) + ",";
    
    // Get Tailscale IP if connected
    const char* tsIp = getTailscaleIP();
    if (tsIp) {
        json += "\"tailscale_ip\":\"" + String(tsIp) + "\",";
    }
    
    // Add WiFi RSSI for diagnostics
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    
    // Escape and add logs
    json += "\"logs\":\"";
    for (size_t i = 0; i < logs.length(); i++) {
        char c = logs[i];
        switch (c) {
            case '"':  json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\n': json += "\\n"; break;
            case '\r': json += "\\r"; break;
            case '\t': json += "\\t"; break;
            default:
                if (c >= 32 && c < 127) {
                    json += c;
                } else {
                    // Skip non-printable characters
                }
        }
    }
    json += "\"}";
    
    int httpCode = http.POST(json);
    http.end();
    
    return (httpCode >= 200 && httpCode < 300);
}

// ============================================================================
// Configuration Storage
// ============================================================================

struct RemoteLogConfig {
    char server[128];
    char deviceId[32];
    bool enabled;
};

static bool loadRemoteLogConfig(RemoteLogConfig* config) {
    if (!config) return false;
    
    memset(config, 0, sizeof(RemoteLogConfig));
    
    if (!remoteLogPrefs.begin(REMOTE_LOG_NVS_NAMESPACE, true)) {
        return false;
    }
    
    config->enabled = remoteLogPrefs.getBool("enabled", false);
    String server = remoteLogPrefs.getString("server", "");
    String devId = remoteLogPrefs.getString("deviceId", "");
    
    strncpy(config->server, server.c_str(), sizeof(config->server) - 1);
    strncpy(config->deviceId, devId.c_str(), sizeof(config->deviceId) - 1);
    
    remoteLogPrefs.end();
    return config->enabled;
}

static bool saveRemoteLogConfig(const RemoteLogConfig* config) {
    if (!config) return false;
    
    if (!remoteLogPrefs.begin(REMOTE_LOG_NVS_NAMESPACE, false)) {
        return false;
    }
    
    remoteLogPrefs.putString("server", config->server);
    remoteLogPrefs.putString("deviceId", config->deviceId);
    remoteLogPrefs.putBool("enabled", config->enabled);
    
    remoteLogPrefs.end();
    return true;
}

// ============================================================================
// Initialization
// ============================================================================

void initRemoteLogger() {
    // Try to load from NVS first
    RemoteLogConfig config;
    if (loadRemoteLogConfig(&config) && config.enabled && strlen(config.server) > 0) {
        RemoteLogger.begin(config.server, 
                          strlen(config.deviceId) > 0 ? config.deviceId : nullptr,
                          true);
    } else if (strlen(REMOTE_LOG_SERVER) > 0) {
        // Fall back to compile-time config
        RemoteLogger.begin(REMOTE_LOG_SERVER, 
                          strlen(REMOTE_LOG_DEVICE_ID) > 0 ? REMOTE_LOG_DEVICE_ID : nullptr,
                          true);
    }
    
    // Add to logger if enabled
    if (RemoteLogger.isEnabled()) {
        Logger.addLogger(RemoteLogger);
        Serial.println("📡 Remote logging enabled");
    }
}

// ============================================================================
// Web Configuration
// ============================================================================

static const char REMOTE_LOG_CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<title>Remote Logging</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px}
.c{max-width:500px;margin:auto;background:#16213e;padding:20px;border-radius:12px;border:1px solid #0f3460}
h2{margin:0 0 20px;color:#e94560}
label{display:block;margin:15px 0 5px;color:#a0a0a0;font-size:14px}
input,select{width:100%;padding:10px;margin:0;border:1px solid #0f3460;border-radius:6px;background:#0f0f23;color:#eee;font-family:monospace;box-sizing:border-box}
input:focus{outline:none;border-color:#e94560}
button{width:100%;background:#e94560;color:white;padding:12px;border:none;border-radius:25px;cursor:pointer;font-size:16px;margin-top:20px}
button:hover{background:#ff6b6b}
.status{padding:10px;border-radius:6px;margin-bottom:15px;font-size:14px}
.enabled{background:rgba(74,222,128,0.2);border-left:3px solid #4ade80}
.disabled{background:rgba(233,69,96,0.2);border-left:3px solid #e94560}
.help{font-size:12px;color:#666;margin-top:5px}
.toggle{display:flex;align-items:center;gap:10px;margin:15px 0}
.toggle input{width:auto}
.back{display:block;text-align:center;margin-top:15px;color:#e94560}
.test{background:#0f3460;margin-top:10px}=
</style>
</head><body>
<div class="c">
<h2>📡 Remote Logging</h2>
<div class="status %STATUS_CLASS%">%STATUS%</div>
<form action="/remotelog/save" method="POST">
<div class="toggle">
<input type="checkbox" name="enabled" id="enabled" %ENABLED_CHECKED%>
<label for="enabled" style="margin:0">Enable Remote Logging</label>
</div>

<label>Log Server URL</label>
<input type="text" name="server" value="%SERVER%" placeholder="http://10.253.0.1:3000/logs">
<div class="help">HTTP endpoint to receive logs (via VPN tunnel)</div>

<label>Device ID</label>
<input type="text" name="deviceId" value="%DEVICE_ID%" placeholder="phone-ABC123">
<div class="help">Unique identifier for this phone (auto-generated from MAC if empty)</div>

<button type="submit">💾 Save Configuration</button>
</form>
<form action="/remotelog/test" method="POST">
<button type="submit" class="test">🧪 Send Test Log</button>
</form>
<a href="/" class="back">← Back</a>
</div>
</body></html>
)rawliteral";

static WebServer* remoteLogWebServer = nullptr;

static void handleRemoteLogPage() {
    if (!remoteLogWebServer) return;
    
    String html = FPSTR(REMOTE_LOG_CONFIG_PAGE);
    
    // Status
    if (RemoteLogger.isEnabled()) {
        html.replace("%STATUS_CLASS%", "enabled");
        html.replace("%STATUS%", String("✅ Enabled: ") + RemoteLogger.getDeviceId() + " → " + RemoteLogger.getServerUrl());
    } else {
        html.replace("%STATUS_CLASS%", "disabled");
        html.replace("%STATUS%", "❌ Disabled");
    }
    
    // Form values
    html.replace("%ENABLED_CHECKED%", RemoteLogger.isEnabled() ? "checked" : "");
    html.replace("%SERVER%", RemoteLogger.getServerUrl());
    html.replace("%DEVICE_ID%", RemoteLogger.getDeviceId());
    
    remoteLogWebServer->send(200, "text/html", html);
}

static void handleRemoteLogSave() {
    if (!remoteLogWebServer) return;
    
    RemoteLogConfig config;
    config.enabled = remoteLogWebServer->hasArg("enabled");
    strncpy(config.server, remoteLogWebServer->arg("server").c_str(), sizeof(config.server) - 1);
    strncpy(config.deviceId, remoteLogWebServer->arg("deviceId").c_str(), sizeof(config.deviceId) - 1);
    
    if (saveRemoteLogConfig(&config)) {
        // Update running config
        if (config.enabled && strlen(config.server) > 0) {
            RemoteLogger.setServer(config.server);
            if (strlen(config.deviceId) > 0) {
                RemoteLogger.setDeviceId(config.deviceId);
            }
            RemoteLogger.setEnabled(true);
            
            // Add to logger if not already added
            Logger.addLogger(RemoteLogger);
        } else {
            RemoteLogger.setEnabled(false);
            Logger.removeLogger(RemoteLogger);
        }
        
        remoteLogWebServer->sendHeader("Location", "/remotelog", true);
        remoteLogWebServer->send(302, "text/plain", "Saved!");
    } else {
        remoteLogWebServer->send(500, "text/plain", "Failed to save");
    }
}

static void handleRemoteLogTest() {
    if (!remoteLogWebServer) return;
    
    if (RemoteLogger.isEnabled()) {
        Logger.println("🧪 Test log message from remote logger web interface");
        RemoteLogger.forceFlush();
        remoteLogWebServer->send(200, "text/plain", "Test log sent!");
    } else {
        remoteLogWebServer->send(400, "text/plain", "Remote logging is disabled");
    }
}

void initRemoteLoggerRoutes(void* server) {
    remoteLogWebServer = static_cast<WebServer*>(server);
    if (!remoteLogWebServer) return;
    
    remoteLogWebServer->on("/remotelog", HTTP_GET, handleRemoteLogPage);
    remoteLogWebServer->on("/remotelog/save", HTTP_POST, handleRemoteLogSave);
    remoteLogWebServer->on("/remotelog/test", HTTP_POST, handleRemoteLogTest);
    
    Serial.println("📡 Remote log config routes registered (/remotelog)");
}
