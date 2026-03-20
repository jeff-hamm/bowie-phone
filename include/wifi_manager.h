#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

// Type definition for WiFi connection callbacks
typedef void (*WiFiConnectedCallback)();
typedef void (*WiFiDisconnectedCallback)();

/**
 * Set callback for WiFi disconnect events
 * @param callback Function to call when WiFi disconnects
 */
void setWiFiDisconnectCallback(WiFiDisconnectedCallback callback);

// WiFi configuration - Use build flags or defaults
#ifndef WIFI_AP_NAME
#define WIFI_AP_NAME "Bowie-Phone-Setup"
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "bowie123"
#endif

#ifndef WIFI_PORTAL_TIMEOUT
#define WIFI_PORTAL_TIMEOUT 180
#endif

// OTA configuration - Use build flags or defaults
#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "bowie-phone"
#endif

#ifndef OTA_PASSWORD
#define OTA_PASSWORD "bowie-ota-2024"
#endif

#ifndef OTA_PORT
#define OTA_PORT 3232
#endif

// Function declarations
void handleLogs();
void initWiFi(WiFiConnectedCallback onConnected = nullptr);
void initOTA();
void startOTA();
void stopOTA();
bool connectToWiFi();
void saveWiFiCredentials(const String& ssid, const String& password);
void startConfigPortal();
bool startConfigPortalSafe();
void handleRoot();
void handleSave();
void handleNetworkLoop();
void initTelnet();
void clearWiFiCredentials();
String getSavedSSID();

// OTA preparation - sets timeout for auto-reboot if OTA doesn't start
void setOtaPrepareTimeout();

// Pull-based OTA - download and install firmware from URL (works over VPN)
bool performPullOTA(const char* firmwareUrl);

// Global variables
extern WebServer server;
extern bool isConfigMode;

#endif // WIFI_MANAGER_H