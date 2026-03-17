// Minimal diagnostic firmware — reuses real wifi_manager + tailscale_manager
// Reports partition table, memory info, and keeps /update OTA endpoint
// Build with: pio run -e diag

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "logging.h"
#include "config.h"
#include "notifications.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "remote_logger.h"

// ============================================================================
// STUBS — replace heavy audio dependencies with no-ops
// ============================================================================

// wifi_manager.cpp calls shutdownAudioForOTA() via special_command_processor.h
// In diag firmware there's no audio, so this is a no-op.
void shutdownAudioForOTA() {
    Logger.println("(diag) shutdownAudioForOTA: no audio to shut down");
}

// special_command_processor.h also declares enterFirmwareUpdateMode()
void enterFirmwareUpdateMode() {
    Logger.println("(diag) enterFirmwareUpdateMode: rebooting");
    delay(500);
    ESP.restart();
}

// ============================================================================
// PARTITION DIAGNOSTIC
// ============================================================================

String getPartitionReport() {
    String r;
    r += "=== ESP32 Partition Diagnostic ===\n";
    r += "Firmware: diag (no audio)\n\n";

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        r += "Running from: " + String(running->label) +
             " (offset 0x" + String(running->address, HEX) +
             ", size " + String(running->size) + " = " + String(running->size / 1024) + " KB)\n";
    }

    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        r += "OTA target:   " + String(next->label) +
             " (offset 0x" + String(next->address, HEX) +
             ", size " + String(next->size) + " = " + String(next->size / 1024) + " KB)\n";
    }

    r += "\n--- Full partition table ---\n";
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t* p = esp_partition_get(it);
        r += String(p->label) +
             "  type=" + String(p->type) +
             " subtype=0x" + String(p->subtype, HEX) +
             " offset=0x" + String(p->address, HEX) +
             " size=" + String(p->size) + " (" + String(p->size / 1024) + " KB)\n";
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    r += "\n--- Memory ---\n";
    r += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    r += "Largest block: " + String(ESP.getMaxAllocHeap()) + " bytes\n";
    r += "Total PSRAM: " + String(ESP.getPsramSize()) + " bytes\n";
    r += "Free PSRAM: " + String(ESP.getFreePsram()) + " bytes\n";
    r += "Flash size: " + String(ESP.getFlashChipSize()) + " bytes (" +
         String(ESP.getFlashChipSize() / 1024 / 1024) + " MB)\n";

    r += "\n--- OTA capacity ---\n";
    if (next) {
        r += "OTA partition size: " + String(next->size / 1024) + " KB (" + String(next->size) + " bytes)\n";
        r += "A 1741 KB firmware needs 1782784 bytes → " +
             String(next->size >= 1782784 ? "FITS" : "TOO SMALL") + "\n";
    }

    r += "\n--- Network ---\n";
    r += "WiFi IP: " + WiFi.localIP().toString() + "\n";
    r += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    r += "VPN connected: " + String(isTailscaleConnected() ? "yes" : "no") + "\n";
    const char* vpnIp = getTailscaleIP();
    r += "VPN IP: " + String(vpnIp ? vpnIp : "N/A") + "\n";

    return r;
}

// ============================================================================
// SETUP & LOOP — mirrors main firmware init order
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Logger.addLogger(Serial);
    Logger.println("\n=== DIAG FIRMWARE STARTING ===");
    Logger.println("This is a diagnostic build — partition check + OTA only");

    // Init LEDs (same as main firmware)
    initNotifications();

    // Init WiFi exactly like main firmware:
    //   - reads NVS saved creds first
    //   - falls back to compile-time DEFAULT_SSID, FALLBACK_SSID_1, etc.
    //   - sets DNS to 8.8.8.8 / 1.1.1.1
    //   - starts WireGuard if TAILSCALE_ALWAYS_ENABLED
    //   - starts OTA + HTTP server (/update, /status, /prepareota, etc.)
    //   - remote logger over VPN
    initWiFi([]() {
        // WiFi connected callback — print partition report
        Logger.println("=== WiFi connected — partition report ===");
        Logger.println(getPartitionReport());
    });

    Logger.println("Setup complete — waiting for WiFi + VPN in loop");
}

void loop() {
    // Identical to main firmware loop timing
    static unsigned long lastCheck = 0;
    unsigned long now = millis();

    if (now - lastCheck >= 100) {
        lastCheck = now;
        handleWiFiLoop();        // WiFi status, fallbacks, OTA, HTTP server
        handleTailscaleLoop();   // WireGuard reconnection
    }

    // Also check for updates periodically
    handlePhoneHomeLoop();
}

