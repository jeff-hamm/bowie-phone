#pragma once
#include "esp_system.h"
#include <stdint.h>

// ============================================================================
// CRASH COUNTER — boot-loop protection via RTC memory
// ============================================================================
// RTC_NOINIT_ATTR survives soft resets (panic, WDT) but is undefined after
// power-on or brownout.  A magic sentinel validates stored data.

#ifndef CRASH_SAFE_MODE_THRESHOLD
#define CRASH_SAFE_MODE_THRESHOLD 3
#endif
#ifndef CRASH_STABILITY_MS
#define CRASH_STABILITY_MS 60000   // 60 s normal uptime = stable → clear counter
#endif
#ifndef SAFE_MODE_REBOOT_MS
#define SAFE_MODE_REBOOT_MS 600000 // 10 min in safe mode before retrying normal boot
#endif

// True while the device is running in safe mode (set by evaluateCrashCounter).
extern bool inSafeMode;

// Number of consecutive crash resets since last clean boot.
extern uint32_t rtcCrashCount;

// Evaluate crash history; returns true if safe mode should be entered.
// Call once near the top of setup() after Logger is ready.
bool evaluateCrashCounter();

// Mark the safe-mode retry flag so the next boot attempts normal startup.
// Call immediately before ESP.restart() in the safe-mode loop.
void markSafeModeRetry();

// Call at the top of loop(). Returns true if the device is in safe mode
// (caller should service telnet/network then return). Handles the retry-boot
// timer internally — restarts the device when SAFE_MODE_REBOOT_MS elapses.
bool tickSafeMode();

// Call in the loop() maintenance block.
// Clears the crash counter once uptime exceeds CRASH_STABILITY_MS.
void tickCrashStabilityCheck();
