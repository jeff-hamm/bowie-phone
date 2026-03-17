#pragma once

#include <Arduino.h>
#include <Print.h>

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
    int lineCount;
    unsigned long lastFlushTime;
    char serverUrl[128];
    char deviceId[32];
    char bootId[16];     // Random hex per boot, used as session key
    bool enabled;
    bool vpnRequired;  // Only send when VPN is connected
    bool bootSent;     // True after boot notification delivered
    
    bool sendLogs(const String& logs);
    bool sendBootNotification();
    
public:
    void flush();
    RemoteLoggerClass();
    
    /**
     * Initialize remote logging
     * @param server URL of the log server (e.g., "http://10.253.0.1:3000/logs")
     * @param deviceId Unique identifier for this phone (uses MAC if empty)
     * @param requireVpn Only send logs when VPN is connected
     */
    void begin(const char* server = nullptr, const char* deviceId = nullptr, bool requireVpn = true);
    
    /**
     * Set/change the log server URL
     */
    void setServer(const char* server);
    
    /**
     * Set/change the device ID
     */
    void setDeviceId(const char* id);
    
    /**
     * Enable or disable remote logging
     */
    void setEnabled(bool enable) { enabled = enable; }
    bool isEnabled() const { return enabled; }
    
    /**
     * Print interface implementation
     */
    size_t write(uint8_t byte) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    
    
    /**
     * Get current device ID
     */
    const char* getDeviceId() const { return deviceId; }
    
    /**
     * Get current server URL
     */
    const char* getServerUrl() const { return serverUrl; }

    /**
     * Get the boot-unique session id (random hex)
     */
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
