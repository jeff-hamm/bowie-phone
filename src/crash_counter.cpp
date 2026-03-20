#include "crash_counter.h"
#include <Arduino.h>
#include "logging.h"

static const uint32_t CRASH_COUNTER_MAGIC   = 0xDEAD0042;
static const uint32_t SAFE_MODE_RETRY_MAGIC = 0xBEEF0001;

RTC_NOINIT_ATTR static uint32_t rtcCrashMagic;
RTC_NOINIT_ATTR        uint32_t rtcCrashCount;
RTC_NOINIT_ATTR static uint32_t rtcSafeModeRetry;

bool inSafeMode = false;

// Evaluate crash history and return true if safe mode should be entered.
// Called once at the very top of setup(), after Logger is ready.
bool evaluateCrashCounter() {
    esp_reset_reason_t reason = esp_reset_reason();

    // Invalid magic or power-on/brownout → RTC data is garbage, initialize
    if (rtcCrashMagic != CRASH_COUNTER_MAGIC ||
        reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
        rtcCrashMagic = CRASH_COUNTER_MAGIC;
        rtcCrashCount = 0;
        rtcSafeModeRetry = 0;
        return false;
    }

    // Safe-mode retry flag set before last reboot → try normal boot once
    if (rtcSafeModeRetry == SAFE_MODE_RETRY_MAGIC) {
        rtcSafeModeRetry = 0;
        Logger.printf("🔄 Safe-mode retry: attempting normal boot (crash count: %d)\n",
                      rtcCrashCount);
        return false;
    }

    // Crash-type resets → increment
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            rtcCrashCount++;
            Logger.printf("⚠️ Crash detected (reason %d), count: %d/%d\n",
                          reason, rtcCrashCount, CRASH_SAFE_MODE_THRESHOLD);
            break;
        default:
            break;  // ESP_RST_SW etc. — preserve counter, don't increment
    }

    return rtcCrashCount >= CRASH_SAFE_MODE_THRESHOLD;
}

void markSafeModeRetry() {
    rtcSafeModeRetry = SAFE_MODE_RETRY_MAGIC;
}

bool tickSafeMode() {
    if (!inSafeMode) return false;
    if (millis() >= SAFE_MODE_REBOOT_MS) {
        Logger.println("\xF0\x9F\x94\x84 Safe mode: retrying normal boot...");
        delay(500);
        markSafeModeRetry();
        ESP.restart();
    }
    return true;
}

void tickCrashStabilityCheck() {
    static bool crashCounterCleared = false;
    if (!crashCounterCleared && millis() >= CRASH_STABILITY_MS) {
        if (rtcCrashCount > 0) {
            Logger.printf("✅ Stable for %ds — crash counter cleared (was %d)\n",
                          CRASH_STABILITY_MS / 1000, rtcCrashCount);
        }
        rtcCrashCount = 0;
        crashCounterCleared = true;
    }
}
