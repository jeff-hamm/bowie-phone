#pragma once

#include <Arduino.h>
#include <Print.h>
#include <WiFiClient.h>

/**
 * Remote Logger for ESP32
 * 
 * Sends logs via HTTP to a remote server over WireGuard/Tailscale VPN.
 * Each phone is identified by a configurable device ID.
 * 
 * Usage:
 *   1. Configure REMOTE_LOG_SERVER and REMOTE_LOG_DEVICE_ID in platformio.ini
 *   2. Call initRemoteLogger() after WireGuard is connected
 *   3. Logs will be sent automatically in batches
 */

// Configuration defaults (override in platformio.ini build_flags)
#ifndef REMOTE_LOG_SERVER
#define REMOTE_LOG_SERVER ""  // e.g., "http://10.253.0.1:3000/logs"
#endif

#ifndef REMOTE_LOG_DEVICE_ID
#define REMOTE_LOG_DEVICE_ID ""  // Will use MAC address if empty
#endif

#ifndef REMOTE_LOG_BATCH_SIZE
#define REMOTE_LOG_BATCH_SIZE 10  // Number of log lines to batch before sending
#endif

#ifndef REMOTE_LOG_FLUSH_INTERVAL_MS
#define REMOTE_LOG_FLUSH_INTERVAL_MS 5000  // Force flush every 5 seconds
#endif

#ifndef REMOTE_LOG_BUFFER_SIZE
#define REMOTE_LOG_BUFFER_SIZE 4096  // Max buffer size before forced flush
#endif

/**
 * Remote Logger Print class
 * Add this to the Logger system to send logs remotely
 */
class RemoteLoggerClass : public Print {
private:
    String logBuffer;
    String preConnectBuffer;  // Logs captured before VPN is ready
    String lineAssembleBuffer; // Partial line buffer for per-line filtering
    int lineCount;
    unsigned long lastFlushTime;
    char serverUrl[128];
    char deviceId[32];
    char bootId[16];     // Random hex per boot, used as session key
    bool enabled;
    bool vpnRequired;  // Only send when VPN is connected
    bool bootSent;     // True after boot notification delivered
    bool _streamingEnabled; // Runtime toggle for log streaming (debug_commands)
    bool _postPending;      // True while an async POST is in flight
    int _consecutiveFailures;        // POST failure counter (for backoff)
    unsigned long _backoffUntil;     // millis() timestamp before which we skip POSTs

    // Persistent TCP log stream to server
    WiFiClient _serverSocket;
    bool _serverTcpEnabled;          // Feature flag (NVS-stored)
    bool _serverConnected;           // Outbound TCP to server is live
    bool _serverIsTelnetClient;      // Server connected inbound to phone:23
    char _serverHost[64];            // Extracted from serverUrl
    int _serverTcpPort;
    int _tcpConsecutiveFailures;
    unsigned long _tcpBackoffUntil;

    static bool isDroppedRemoteLogLine(const String& line);
    void trimLogBuffer();
    void appendFilteredTo(String& targetBuffer, const uint8_t* buffer, size_t size, bool countLines);
    
    bool buildLogsJson(String& out, const String& logs);
    bool buildBootJson(String& out);

    static void onLogPostDone(bool success, int statusCode, void* userData);
    static void onBootPostDone(bool success, int statusCode, void* userData);

    void parseServerHost();
    void maintainServerConnection();
    bool sendTcpHandshake();
    
public:
    void flush();
    RemoteLoggerClass();
    
    void begin(const char* server = nullptr, const char* deviceId = nullptr, bool requireVpn = true);
    void setServer(const char* server);
    void setDeviceId(const char* id);
    void setEnabled(bool enable) { enabled = enable; }
    bool isEnabled() const { return enabled; }

    void setStreamingEnabled(bool enable) { _streamingEnabled = enable; }
    bool isStreamingEnabled() const { return _streamingEnabled; }

    void setServerTcpEnabled(bool enable);
    bool isServerTcpEnabled() const { return _serverTcpEnabled; }
    bool isServerConnected() const { return _serverConnected || _serverIsTelnetClient; }
    void setServerIsTelnetClient(bool v) { _serverIsTelnetClient = v; }
    const char* getServerHost() const { return _serverHost; }
    
    size_t write(uint8_t byte) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    
    const char* getDeviceId() const { return deviceId; }
    const char* getServerUrl() const { return serverUrl; }
    const char* getBootId() const { return bootId; }
};

// Global instance
extern RemoteLoggerClass RemoteLogger;

/**
 * Initialize remote logging with default or configured values
 * Call this after WireGuard is connected
 */
void initRemoteLogger();

/**
 * Add remote logger web configuration routes
 * @param server Pointer to WebServer instance
 */
void initRemoteLoggerRoutes(void* server);
