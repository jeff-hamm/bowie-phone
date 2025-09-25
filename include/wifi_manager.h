#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

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
void initWiFi();
void initOTA();
bool connectToWiFi();
void saveWiFiCredentials(const String& ssid, const String& password);
void startConfigPortal();
void handleRoot();
void handleSave();
void handleWiFiLoop();

// External variables (defined in wifi_manager.cpp)
extern WebServer server;
extern DNSServer dnsServer;
extern Preferences wifiPrefs;
extern bool isConfigMode;

#endif // WIFI_MANAGER_H