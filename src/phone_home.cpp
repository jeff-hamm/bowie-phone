#include "phone_home.h"
#include "config.h"
#include "logging.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include "http_utils.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "jump-phone"
#endif

static unsigned long phoneHomeInterval = UPDATE_CHECK_INTERVAL_MS;
static unsigned long lastPhoneHomeTime = 0;
static char phoneHomeStatus[64] = "Not started";
static bool phoneHomeEnabled = true;

void setPhoneHomeInterval(unsigned long intervalMs) {
    phoneHomeInterval = intervalMs;
    Logger.printf("📞 Update check interval set to %lu ms\n", intervalMs);
}

const char* getPhoneHomeStatus() {
    return phoneHomeStatus;
}

// Compare version strings (e.g., "1.0.1" vs "1.0.2")
// Returns: -1 if v1 < v2, 0 if equal, 1 if v1 > v2
static int compareVersions(const char* v1, const char* v2) {
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return major1 < major2 ? -1 : 1;
    if (minor1 != minor2) return minor1 < minor2 ? -1 : 1;
    if (patch1 != patch2) return patch1 < patch2 ? -1 : 1;
    return 0;
}

// Find a JSON string value for a given key in a JSON object substring.
// Returns the value or empty string if not found.
static String findJsonStringValue(const String& json, const char* key) {
    String needle = String("\"") + key + "\"";
    int keyStart = json.indexOf(needle);
    if (keyStart < 0) return "";
    int valQuote = json.indexOf("\"", keyStart + needle.length() + 1); // skip past ':'
    if (valQuote < 0) return "";
    valQuote++; // skip opening quote
    int valEnd = json.indexOf("\"", valQuote);
    if (valEnd <= valQuote) return "";
    return json.substring(valQuote, valEnd);
}

// Extract the JSON object block for a given top-level key from releases.json.
// releases.json format:
// {
//   "bowie-phone": { "version": "1.0.3", "firmware_url": "...", ... },
//   "default":     { "version": "1.0.3", "firmware_url": "...", ... }
// }
// Returns the inner object string (without outer braces of the parent),
// or empty string if not found.
static String findDeviceBlock(const String& json, const char* deviceKey) {
    String needle = String("\"") + deviceKey + "\"";
    int keyPos = json.indexOf(needle);
    if (keyPos < 0) return "";

    // Find the opening '{' of this device's object
    int braceOpen = json.indexOf('{', keyPos + needle.length());
    if (braceOpen < 0) return "";

    // Find matching closing '}'
    int depth = 1;
    int pos = braceOpen + 1;
    while (pos < (int)json.length() && depth > 0) {
        char c = json.charAt(pos);
        if (c == '{') depth++;
        else if (c == '}') depth--;
        pos++;
    }
    if (depth != 0) return "";
    return json.substring(braceOpen, pos);
}

// Check for updates from static JSON file (releases.json)
// New multi-device format:
// {
//   "<hostname>": {
//     "version": "1.0.3",
//     "firmware_url": "https://example.com/firmware/<hostname>/firmware.bin",
//     "release_notes": "Bug fixes",
//     "action": "none" | "ota" | "reboot",
//     "message": ""
//   },
//   "default": { ... }
// }
// Falls back to "default" key if hostname not found.
bool checkForRemoteUpdates(const char* serverUrl) {
    unsigned long now = millis();
    if (!phoneHomeEnabled || now - lastPhoneHomeTime < phoneHomeInterval)
        return false;
    lastPhoneHomeTime = now;

    if (!WiFi.isConnected()) {
        strcpy(phoneHomeStatus, "WiFi not connected");
        return false;
    }
    
    const char* url = serverUrl ? serverUrl : UPDATE_CHECK_URL;
    
    Logger.printf("📞 Checking for updates: %s\n", url);
    strcpy(phoneHomeStatus, "Checking...");
    
    HttpClient http;
    
    if (!http.get(url)) {
        snprintf(phoneHomeStatus, sizeof(phoneHomeStatus), "HTTP error: %d", http.statusCode());
        return false;
    }
    
    String response = http.getString();
    
    Logger.printf("📞 Update info: %s\n", response.c_str());
    
    // Look up this device's block by OTA_HOSTNAME, fall back to "default"
    String deviceBlock = findDeviceBlock(response, OTA_HOSTNAME);
    if (deviceBlock.length() == 0) {
        Logger.printf("📞 No entry for '%s', trying 'default'\n", OTA_HOSTNAME);
        deviceBlock = findDeviceBlock(response, "default");
    }
    if (deviceBlock.length() == 0) {
        Logger.println("⚠️ Update check: No matching device or default entry");
        strcpy(phoneHomeStatus, "No device entry");
        return false;
    }

    String serverVersion = findJsonStringValue(deviceBlock, "version");
    String firmwareUrl   = findJsonStringValue(deviceBlock, "firmware_url");
    String action        = findJsonStringValue(deviceBlock, "action");
    String message       = findJsonStringValue(deviceBlock, "message");
    
    if (action.length() == 0) action = "none";
    
    if (message.length() > 0) {
        Logger.printf("💬 Server: %s\n", message.c_str());
    }
    
    // Handle forced actions
    if (action == "reboot") {
        Logger.println("🔄 Update check: Reboot requested");
        strcpy(phoneHomeStatus, "Rebooting...");
        delay(1000);
        esp_restart();
    }
    
    // Check if update is available
    bool otaTriggered = false;
    const char* currentVersion = FIRMWARE_VERSION;
    
    if (serverVersion.length() > 0 && firmwareUrl.length() > 0) {
        int cmp = compareVersions(currentVersion, serverVersion.c_str());
        
        if (cmp < 0 || action == "ota") {
            if (cmp < 0) {
                Logger.printf("📥 Update available: %s -> %s\n", currentVersion, serverVersion.c_str());
            } else {
                Logger.printf("📥 Forced OTA to version %s\n", serverVersion.c_str());
            }
            snprintf(phoneHomeStatus, sizeof(phoneHomeStatus), "Updating to %s", serverVersion.c_str());
            otaTriggered = performPullOTA(firmwareUrl.c_str());
        } else if (cmp == 0) {
            Logger.printf("✅ Firmware up to date: %s\n", currentVersion);
            snprintf(phoneHomeStatus, sizeof(phoneHomeStatus), "Up to date: %s", currentVersion);
        } else {
            Logger.printf("ℹ️ Running newer than server: %s > %s\n", currentVersion, serverVersion.c_str());
            snprintf(phoneHomeStatus, sizeof(phoneHomeStatus), "Dev build: %s", currentVersion);
        }
    } else {
        Logger.println("⚠️ Update check: Missing version or URL in response");
        strcpy(phoneHomeStatus, "Invalid response");
    }
    
    return otaTriggered;
}
