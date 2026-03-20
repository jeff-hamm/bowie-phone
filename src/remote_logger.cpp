#include "remote_logger.h"
#include "logging.h"
#include "tailscale_manager.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include "http_utils.h"
#include <Preferences.h>
#include <WebServer.h>
#include <esp_system.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

// Global instance
RemoteLoggerClass RemoteLogger;

// NVS namespace for remote logger config
#define REMOTE_LOG_NVS_NAMESPACE "remotelog"

static Preferences remoteLogPrefs;

static const char* REMOTE_LOG_DROPPED_LINE =
    "[I] StreamCopy.h : 187 - StreamCopy::copy  2048 -> 2048 -> 2048 bytes - in 1 hops";

RemoteLoggerClass::RemoteLoggerClass() 
    : lineCount(0), lastFlushTime(0), enabled(false), vpnRequired(true),
      bootSent(false), _streamingEnabled(true), _postPending(false),
      _consecutiveFailures(0), _backoffUntil(0),
      _serverTcpEnabled(false), _serverConnected(false),
      _serverIsTelnetClient(false), _serverTcpPort(REMOTE_LOG_TCP_PORT),
      _tcpConsecutiveFailures(0), _tcpBackoffUntil(0) {
    serverUrl[0] = '\0';
    deviceId[0] = '\0';
    bootId[0] = '\0';
    _serverHost[0] = '\0';
    // NOTE: Do NOT call esp_random() or String::reserve() here.
    // Global constructors run before FreeRTOS starts — heap allocs and
    // hardware API calls at this stage can prevent the idle-task stack
    // from being allocated, causing a boot crash.  Deferred to begin().
}

bool RemoteLoggerClass::isDroppedRemoteLogLine(const String& line) {
    return line == REMOTE_LOG_DROPPED_LINE;
}

void RemoteLoggerClass::trimLogBuffer() {
    bool trimmed = false;
    while (logBuffer.length() >= REMOTE_LOG_BUFFER_SIZE - 256) {
        int nl = logBuffer.indexOf('\n');
        if (nl < 0) {
            // Keep recent bytes if we don't have a full line break yet.
            if (logBuffer.length() > REMOTE_LOG_BUFFER_SIZE / 2) {
                logBuffer.remove(0, logBuffer.length() - (REMOTE_LOG_BUFFER_SIZE / 2));
            }
            break;
        }
        logBuffer.remove(0, nl + 1);
        if (lineCount > 0) {
            lineCount--;
        }
        trimmed = true;
    }
    // Indicate that lines were dropped so the receiver knows the log is incomplete
    if (trimmed && !logBuffer.startsWith("...\n")) {
        logBuffer = "...\n" + logBuffer;
    }
}

void RemoteLoggerClass::appendFilteredTo(String& targetBuffer, const uint8_t* buffer, size_t size, bool countLines) {
    for (size_t i = 0; i < size; i++) {
        char c = (char)buffer[i];
        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            if (!isDroppedRemoteLogLine(lineAssembleBuffer)) {
                targetBuffer += lineAssembleBuffer;
                targetBuffer += '\n';
                if (countLines) {
                    lineCount++;
                }
            }
            lineAssembleBuffer = "";
            continue;
        }

        lineAssembleBuffer += c;
    }
}

void RemoteLoggerClass::begin(const char* server, const char* devId, bool requireVpn) {
    vpnRequired = requireVpn;

    // Deferred from constructor — safe now that FreeRTOS + heap are running
    if (bootId[0] == '\0') {
        uint32_t r1 = esp_random();
        uint32_t r2 = esp_random();
        snprintf(bootId, sizeof(bootId), "%08x%04x", r1, (uint16_t)(r2 & 0xFFFF));
        logBuffer.reserve(REMOTE_LOG_BUFFER_SIZE);
        preConnectBuffer.reserve(REMOTE_LOG_BUFFER_SIZE);
    }
    
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
    
    // Extract host from server URL for TCP connections
    parseServerHost();
    
    if (enabled) {
        Serial.printf("📡 Remote Logger: %s -> %s (boot %s)\n", deviceId, serverUrl, bootId);
        // Move any pre-connect logs into the main buffer so they ship on first flush
        if (preConnectBuffer.length() > 0) {
            logBuffer = preConnectBuffer;
            preConnectBuffer = "";
            lineCount = 0;
            for (size_t i = 0; i < logBuffer.length(); i++) {
                if (logBuffer[i] == '\n') lineCount++;
            }
        }
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
    const uint8_t oneByte[] = {byte};

    if (!enabled) {
        // Capture early boot logs before begin() enables us
        appendFilteredTo(preConnectBuffer, oneByte, 1, false);
        if (preConnectBuffer.length() > REMOTE_LOG_BUFFER_SIZE) {
            preConnectBuffer.remove(0, preConnectBuffer.length() - REMOTE_LOG_BUFFER_SIZE);
        }
        return 1;
    }

    appendFilteredTo(logBuffer, oneByte, 1, true);
    trimLogBuffer();
    
    return 1;
}

size_t RemoteLoggerClass::write(const uint8_t* buffer, size_t size) {
    if (!enabled) {
        appendFilteredTo(preConnectBuffer, buffer, size, false);
        if (preConnectBuffer.length() > REMOTE_LOG_BUFFER_SIZE) {
            preConnectBuffer.remove(0, preConnectBuffer.length() - REMOTE_LOG_BUFFER_SIZE);
        }
        return size;
    }

    appendFilteredTo(logBuffer, buffer, size, true);
    trimLogBuffer();
    
    return size;
}

void RemoteLoggerClass::flush() {
    if (!enabled || !_streamingEnabled || logBuffer.length() == 0 ||
        millis() - lastFlushTime < REMOTE_LOG_FLUSH_INTERVAL_MS)
        return;

    // Try to establish / maintain TCP connection to server (runs on core 1, short timeout)
    maintainServerConnection();

    // ── Priority 1: Persistent TCP stream to server ──────────────────────────
    // Plain text, no JSON, no core 0 task — lowest overhead.
    if (_serverConnected && _serverSocket.connected()) {
        size_t written = _serverSocket.print(logBuffer);
        if (written > 0) {
            logBuffer = "";
            lineCount = 0;
            lastFlushTime = millis();
            return;
        }
        // Write failed — connection probably dead, will be caught by maintainServerConnection next time
        _serverConnected = false;
    }

    // ── Priority 2: Server connected inbound to phone's telnet ───────────────
    // Logs are already flowing through Logger → telnet → server.
    if (_serverIsTelnetClient) {
        // Still consume the buffer so it doesn't grow forever
        logBuffer = "";
        lineCount = 0;
        lastFlushTime = millis();
        return;
    }

    // ── Priority 3: HTTP POST with backoff (fallback) ────────────────────────
    // Don't double-post
    if (_postPending)
        return;

    // Exponential backoff: 30s, 60s, 120s, 240s … capped at 5 min
    if (_consecutiveFailures > 0 && millis() < _backoffUntil) {
        return;
    }

    // Check connectivity: WiFi must be up
    if (!WiFi.isConnected()) return;

    // Check VPN requirement
    if (vpnRequired && !isTailscaleConnected()) {
        // Don't flush, keep buffering (up to limit)
        if (logBuffer.length() >= REMOTE_LOG_BUFFER_SIZE - 256) {
            int newlinePos = logBuffer.indexOf('\n');
            if (newlinePos > 0) {
                logBuffer.remove(0, newlinePos + 1);
                lineCount--;
            }
        }
        return;
    }

    if (strlen(serverUrl) == 0) return;

    // Send boot notification on first flush (fire-and-forget async)
    if (!bootSent) {
        String bootJson;
        if (buildBootJson(bootJson)) {
            HttpClient::postAsync(serverUrl, std::move(bootJson),
                                  onBootPostDone, this,
                                  "application/json",
                                  "X-Device-ID", deviceId);
        }
    }

    // Snapshot the buffer and send via async POST on core 0
    String json;
    if (!buildLogsJson(json, logBuffer)) return;

    _postPending = true;
    logBuffer = "";
    lineCount = 0;
    lastFlushTime = millis();

    HttpClient::postAsync(serverUrl, std::move(json),
                          onLogPostDone, this,
                          "application/json",
                          "X-Device-ID", deviceId,
                          HTTP_TIMEOUT_LOG_MS);
}

bool RemoteLoggerClass::buildLogsJson(String& out, const String& logs) {
    out.reserve(200 + logs.length() * 2);

    char header[160];
    snprintf(header, sizeof(header),
             "{\"device\":\"%s\",\"boot_id\":\"%s\",\"uptime_sec\":%lu,\"logs\":\"",
             deviceId, bootId, millis() / 1000);
    out += header;

    // Escape log content
    for (size_t i = 0; i < logs.length(); i++) {
        char c = logs[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 32 && c < 127) {
                    out += c;
                }
        }
    }
    out += "\"}";
    return true;
}

bool RemoteLoggerClass::buildBootJson(String& out) {
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "unknown";
    switch (reason) {
        case ESP_RST_POWERON:  reasonStr = "power_on"; break;
        case ESP_RST_SW:       reasonStr = "software"; break;
        case ESP_RST_PANIC:    reasonStr = "panic"; break;
        case ESP_RST_INT_WDT:  reasonStr = "int_wdt"; break;
        case ESP_RST_TASK_WDT: reasonStr = "task_wdt"; break;
        case ESP_RST_WDT:      reasonStr = "wdt"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "deep_sleep"; break;
        case ESP_RST_BROWNOUT: reasonStr = "brownout"; break;
        default: break;
    }

    const char* tsIp = getTailscaleIP();

    out = "{";
    out += "\"device\":\"" + String(deviceId) + "\",";
    out += "\"boot_id\":\"" + String(bootId) + "\",";
    out += "\"boot\":true,";
    out += "\"boot_reason\":\"" + String(reasonStr) + "\",";
    out += "\"firmware\":\"" FIRMWARE_VERSION "\",";
    out += "\"uptime_sec\":" + String(millis() / 1000) + ",";
    out += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    out += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    out += "\"wifi_ip\":\"" + WiFi.localIP().toString() + "\",";
    if (tsIp) {
        out += "\"tailscale_ip\":\"" + String(tsIp) + "\",";
    }
    out += "\"logs\":\"BOOT firmware=" FIRMWARE_VERSION " reason=";
    out += String(reasonStr);
    out += "\"}";
    return true;
}

void RemoteLoggerClass::onLogPostDone(bool success, int statusCode, void* userData) {
    auto* self = static_cast<RemoteLoggerClass*>(userData);
    self->_postPending = false;
    if (success) {
        self->_consecutiveFailures = 0;
    } else {
        self->_consecutiveFailures++;
        // Dramatic backoff: 30s, 60s, 120s, 240s … capped at 5 min
        unsigned long delay = 30000UL * (1UL << min(self->_consecutiveFailures - 1, 3));
        if (delay > 300000UL) delay = 300000UL;
        self->_backoffUntil = millis() + delay;
    }
    // No logging here — callback runs on core 0, Logger writes are core-1 only
}

void RemoteLoggerClass::onBootPostDone(bool success, int statusCode, void* userData) {
    auto* self = static_cast<RemoteLoggerClass*>(userData);
    if (success) {
        self->bootSent = true;
    }
}

// ============================================================================
// Persistent TCP Log Stream
// ============================================================================

void RemoteLoggerClass::parseServerHost() {
    _serverHost[0] = '\0';
    if (strlen(serverUrl) == 0) return;

    // serverUrl is like "http://10.253.0.1:3000/logs"
    const char* p = serverUrl;
    // Skip scheme
    const char* schemeEnd = strstr(p, "://");
    if (schemeEnd) p = schemeEnd + 3;
    // Copy host (stop at : / or end)
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i < sizeof(_serverHost) - 1) {
        _serverHost[i++] = *p++;
    }
    _serverHost[i] = '\0';
}

void RemoteLoggerClass::setServerTcpEnabled(bool enable) {
    _serverTcpEnabled = enable;
    if (!enable && _serverConnected) {
        _serverSocket.stop();
        _serverConnected = false;
    }
    // Persist to NVS
    if (remoteLogPrefs.begin(REMOTE_LOG_NVS_NAMESPACE, false)) {
        remoteLogPrefs.putBool("tcpEnabled", enable);
        remoteLogPrefs.end();
    }
}

bool RemoteLoggerClass::sendTcpHandshake() {
    // Send: BOWIE-LOG device=<id> boot=<bootId> firmware=<ver>\n
    char handshake[160];
    snprintf(handshake, sizeof(handshake),
             "BOWIE-LOG device=%s boot=%s firmware=%s\n",
             deviceId, bootId, FIRMWARE_VERSION);

    size_t written = _serverSocket.print(handshake);
    if (written == 0) return false;

    // Wait for BOWIE-ACK\n (up to 2 seconds)
    unsigned long start = millis();
    String response;
    while (millis() - start < 2000) {
        if (_serverSocket.available()) {
            char c = _serverSocket.read();
            response += c;
            if (c == '\n') break;
        }
        delay(10);
    }

    return response.startsWith("BOWIE-ACK");
}

void RemoteLoggerClass::maintainServerConnection() {
    if (!_serverTcpEnabled || !enabled) return;
    if (!WiFi.isConnected()) return;
    if (vpnRequired && !isTailscaleConnected()) return;
    if (_serverHost[0] == '\0') return;

    // Check existing connection health
    if (_serverConnected) {
        if (!_serverSocket.connected()) {
            _serverConnected = false;
            _tcpConsecutiveFailures++;
            unsigned long backoff = REMOTE_LOG_TCP_RECONNECT_MS * (1UL << min(_tcpConsecutiveFailures - 1, 4));
            if (backoff > 300000UL) backoff = 300000UL;  // cap 5 min
            _tcpBackoffUntil = millis() + backoff;
            Serial.println("📡 TCP log stream disconnected, will reconnect");
        }
        return;
    }

    // Apply backoff
    if (_tcpConsecutiveFailures > 0 && millis() < _tcpBackoffUntil) return;

    // Attempt connection
    _serverSocket.setTimeout(REMOTE_LOG_TCP_CONNECT_TIMEOUT_MS);
    if (!_serverSocket.connect(_serverHost, _serverTcpPort)) {
        _tcpConsecutiveFailures++;
        unsigned long backoff = REMOTE_LOG_TCP_RECONNECT_MS * (1UL << min(_tcpConsecutiveFailures - 1, 4));
        if (backoff > 300000UL) backoff = 300000UL;
        _tcpBackoffUntil = millis() + backoff;
        return;
    }

    // Connected — perform handshake
    if (!sendTcpHandshake()) {
        _serverSocket.stop();
        _tcpConsecutiveFailures++;
        unsigned long backoff = REMOTE_LOG_TCP_RECONNECT_MS * (1UL << min(_tcpConsecutiveFailures - 1, 4));
        if (backoff > 300000UL) backoff = 300000UL;
        _tcpBackoffUntil = millis() + backoff;
        return;
    }

    _serverConnected = true;
    _tcpConsecutiveFailures = 0;
    Serial.printf("📡 TCP log stream connected to %s:%d\n", _serverHost, _serverTcpPort);
}

// ============================================================================
// Configuration Storage
// ============================================================================

struct RemoteLogConfig {
    char server[128];
    char deviceId[32];
    bool enabled;
    bool tcpEnabled;
};

static bool loadRemoteLogConfig(RemoteLogConfig* config) {
    if (!config) return false;
    
    memset(config, 0, sizeof(RemoteLogConfig));
    
    if (!remoteLogPrefs.begin(REMOTE_LOG_NVS_NAMESPACE, true)) {
        return false;
    }
    
    config->enabled = remoteLogPrefs.getBool("enabled", false);
    config->tcpEnabled = remoteLogPrefs.getBool("tcpEnabled", false);
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
    remoteLogPrefs.putBool("tcpEnabled", config->tcpEnabled);
    
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
        if (config.tcpEnabled) RemoteLogger.setServerTcpEnabled(true);
    } else if (strlen(REMOTE_LOG_SERVER) > 0) {
        // Fall back to compile-time config
        RemoteLogger.begin(REMOTE_LOG_SERVER, 
                          strlen(REMOTE_LOG_DEVICE_ID) > 0 ? REMOTE_LOG_DEVICE_ID : nullptr,
                          true);
    }
    
    if (RemoteLogger.isEnabled()) {
        // Logger.addLogger was already called early in setup() so pre-boot logs
        // are captured.  begin() moved the pre-connect buffer into the send queue.
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

<div class="toggle">
<input type="checkbox" name="tcpEnabled" id="tcpEnabled" %TCP_ENABLED_CHECKED%>
<label for="tcpEnabled" style="margin:0">Enable Persistent TCP Stream (port %TCP_PORT%)</label>
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
        String statusMsg = String("✅ Enabled: ") + RemoteLogger.getDeviceId() + " → " + RemoteLogger.getServerUrl();
        if (RemoteLogger.isServerTcpEnabled()) {
            statusMsg += RemoteLogger.isServerConnected() ? "<br>🔌 TCP stream: connected" : "<br>🔌 TCP stream: waiting";
        }
        html.replace("%STATUS%", statusMsg);
    } else {
        html.replace("%STATUS_CLASS%", "disabled");
        html.replace("%STATUS%", "❌ Disabled");
    }
    
    // Form values
    html.replace("%ENABLED_CHECKED%", RemoteLogger.isEnabled() ? "checked" : "");
    html.replace("%TCP_ENABLED_CHECKED%", RemoteLogger.isServerTcpEnabled() ? "checked" : "");
    html.replace("%TCP_PORT%", String(REMOTE_LOG_TCP_PORT));
    html.replace("%SERVER%", RemoteLogger.getServerUrl());
    html.replace("%DEVICE_ID%", RemoteLogger.getDeviceId());
    
    remoteLogWebServer->send(200, "text/html", html);
}

static void handleRemoteLogSave() {
    if (!remoteLogWebServer) return;
    
    RemoteLogConfig config;
    config.enabled = remoteLogWebServer->hasArg("enabled");
    config.tcpEnabled = remoteLogWebServer->hasArg("tcpEnabled");
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
            RemoteLogger.setServerTcpEnabled(config.tcpEnabled);
            
            // Add to logger if not already added
            Logger.addLogger(RemoteLogger);
        } else {
            RemoteLogger.setEnabled(false);
            RemoteLogger.setServerTcpEnabled(false);
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
        RemoteLogger.flush();
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
