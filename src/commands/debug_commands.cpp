#include "commands_internal.h"

// ============================================================================
// MODULE-PRIVATE STATE
// ============================================================================

// Serial/Telnet debug input line buffer
static char debugInputBuffer[64];
static int debugInputPos = 0;

// Audio capture on next off-hook. Can be preset at build time with
// -DCAPTURE_ON_OFFHOOK=<seconds>.
#ifdef CAPTURE_ON_OFFHOOK
static bool captureOnNextOffHook = true;
  #if CAPTURE_ON_OFFHOOK == 1
    static int captureRequestedDuration = 20;
  #else
    static int captureRequestedDuration = CAPTURE_ON_OFFHOOK;
  #endif
#else
static bool captureOnNextOffHook = false;
static int captureRequestedDuration = 0;
#endif

// NVS keys for audio capture persistence
#define NVS_KEY_CAPTURE_ARMED    "cap_armed"
#define NVS_KEY_CAPTURE_DURATION "cap_dur"

static Preferences preferences;

// ============================================================================
// NVS CAPTURE STATE HELPERS (private)
// ============================================================================

static void saveAudioCaptureState() {
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Logger.println("⚠️  Failed to save audio capture state to NVS");
        return;
    }
    preferences.putBool(NVS_KEY_CAPTURE_ARMED, captureOnNextOffHook);
    preferences.putInt(NVS_KEY_CAPTURE_DURATION, captureRequestedDuration);
    preferences.end();
}

static void loadAudioCaptureState() {
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
        return;
    }

    bool hasNVSState = preferences.isKey(NVS_KEY_CAPTURE_ARMED);

    if (hasNVSState) {
        captureOnNextOffHook = preferences.getBool(NVS_KEY_CAPTURE_ARMED, false);
        captureRequestedDuration = preferences.getInt(NVS_KEY_CAPTURE_DURATION, 20);
    }
    // else: keep static variable values (defaults or build-flag init)

    preferences.end();

    if (captureOnNextOffHook) {
        if (hasNVSState) {
            Logger.printf("📋 Restored audio capture from NVS: %d seconds on next off-hook\n",
                          captureRequestedDuration);
        } else {
#ifdef CAPTURE_ON_OFFHOOK
            Logger.printf("📋 Audio capture armed from build flag: %d seconds on next off-hook\n",
                          captureRequestedDuration);
            saveAudioCaptureState();
#endif
        }
    }
}

static void clearAudioCaptureState() {
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        return;
    }
    preferences.remove(NVS_KEY_CAPTURE_ARMED);
    preferences.remove(NVS_KEY_CAPTURE_DURATION);
    preferences.end();
}

// ============================================================================
// DEBUG COMMAND DISPATCH (private — only called from processDebugInput)
// ============================================================================

static void processDebugCommand(const String& cmd);

// ============================================================================
// PUBLIC API
// ============================================================================

void processDebugInput(Stream& input) {
    while (input.available()) {
        char c = input.read();

        if (c == '\n' || c == '\r') {
            if (debugInputPos > 0) {
                debugInputBuffer[debugInputPos] = '\0';
                String cmd = String(debugInputBuffer);
                cmd.trim();
                processDebugCommand(cmd);
                debugInputPos = 0;
            }
        }
        else if (debugInputPos < (int)sizeof(debugInputBuffer) - 1) {
            debugInputBuffer[debugInputPos++] = c;
        }
    }
}

void initAudioCaptureState() {
    loadAudioCaptureState();
}

bool checkAndExecuteOffHookCapture() {
    if (captureOnNextOffHook) {
        Logger.printf("🎙️ Auto-triggering audio capture (%d seconds)...\n",
                      captureRequestedDuration);
        performAudioCapture(captureRequestedDuration);

        captureOnNextOffHook = false;
        captureRequestedDuration = 0;
        clearAudioCaptureState();
        return true;
    }
    return false;
}

void enterFirmwareUpdateMode() {
    Logger.println();
    Logger.println("============================================");
    Logger.println("🔧 ENTERING FIRMWARE UPDATE MODE");
    Logger.println("============================================");
    Logger.println("   The device will now restart into bootloader.");
    Logger.println("   You can now upload new firmware.");
    Logger.println();
    Logger.println("   After upload, device will boot normally.");
    Logger.println("============================================");
    Logger.flush();
    delay(500);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    delay(100);

    Logger.println("   Restarting... Press upload now!");
    Logger.flush();
    delay(200);

    esp_restart();
}

void shutdownAudioForOTA() {
    Logger.println("🔇 Shutting down audio for OTA...");

    getExtendedAudioPlayer().stop();
    delay(50);

    // Note: We intentionally do NOT call kit.end() here because:
    // 1. It can crash if SPI was already released elsewhere
    // 2. The OTA onStart already calls SD.end() and SPI.end()
    // 3. GPIO pins are reset separately

    Logger.println("✅ Audio stopped for OTA");
}

// ============================================================================
// DEBUG COMMAND DISPATCH (definition)
// ============================================================================

static void processDebugCommand(const String& cmd) {
    if (cmd.equalsIgnoreCase("hook")) {
        bool newState = !Phone.isOffHook();
        Phone.setOffHook(newState);
        Logger.printf("🔧 [DEBUG] Hook toggled to: %s\n", newState ? "OFF HOOK" : "ON HOOK");
    }
    else if (cmd.equalsIgnoreCase("hook auto")) {
        Phone.resetDebugOverride();
        Logger.println("🔧 [DEBUG] Hook detection reset to automatic");
    }
    else if (cmd.equalsIgnoreCase("cpuload") || cmd.equalsIgnoreCase("perftest")
          || cmd.equalsIgnoreCase("cpuload-goertzel") || cmd.equalsIgnoreCase("perftest-goertzel")) {
        performGoertzelCPULoadTest();
    }
    else if (cmd.equalsIgnoreCase("help") || cmd.equals("?")) {
        Logger.println("🔧 [DEBUG] Serial/Telnet Commands:");
        Logger.println("   hook          - Toggle hook state");
        Logger.println("   hook auto     - Reset to automatic hook detection");
        Logger.println("   cpuload       - Test CPU load (Goertzel DTMF + audio)");
        Logger.println("   level <0-2>   - Set log level (0=quiet, 1=normal, 2=debug)");
        Logger.println("   state         - Show current state");
        Logger.println("   debugaudio [s] - Arm audio capture on next off-hook (1-60s, default 20)");
        Logger.println("   debuginput [f] - Full E2E test: hook→dialtone→Goertzel→sequence→timeout→reset");
        Logger.println("   audiotest      - Test audio output (verify I2S data flow)");
        Logger.println("   sddebug       - Test SD card initialization methods");
        Logger.println("   scan          - Scan for WiFi networks");
        Logger.println("   dns           - Test DNS resolution");
        Logger.println("   tailscale     - Toggle Tailscale VPN on/off");
        Logger.println("   pullota <url> - Pull firmware from URL");
        Logger.println("   update        - Enter firmware bootloader mode");
        Logger.println("   <digits>      - Simulate DTMF sequence");
        Logger.println();
        Logger.println("📱 Phone Commands (dial these):");
        Logger.println("   *123#  - System Status");
        Logger.println("   *789#  - Reboot Device");
        Logger.println("   *#06#  - Device Info");
        Logger.println("   clear-cache  - Clear Cache & Reboot");
        Logger.println("   *#07#  - Refresh Audio");
        Logger.println("   *#08#  - Prepare for OTA");
        Logger.println("   *#09#  - Phone Home Check-in");
        Logger.println("   *#88#  - Tailscale Status");
        Logger.println("   *#00#  - List All Commands");
    }
    else if (cmd.equalsIgnoreCase("scan") || cmd.equalsIgnoreCase("wifiscan")) {
        Logger.println("🔧 [DEBUG] Scanning for WiFi networks...");
        Logger.printf("   Current WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        if (WiFi.status() == WL_CONNECTED) {
            Logger.printf("   Connected to: %s\n", WiFi.SSID().c_str());
            Logger.printf("   Signal: %d dBm\n", WiFi.RSSI());
        }
        Logger.println();

        int n = WiFi.scanNetworks();
        Logger.printf("   Found %d networks:\n", n);
        Logger.println();

        for (int i = 0; i < n; i++) {
            Logger.printf("   %2d: %-32s | Ch:%2d | %4d dBm | %s\n",
                i + 1,
                WiFi.SSID(i).c_str(),
                WiFi.channel(i),
                WiFi.RSSI(i),
                WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open" : "Secure");
        }
        Logger.println();
        WiFi.scanDelete();
    }
    else if (cmd.equalsIgnoreCase("dns")) {
        Logger.println("🔧 [DEBUG] Testing DNS resolution...");
        Logger.printf("   WiFi status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        Logger.printf("   Local IP: %s\n", WiFi.localIP().toString().c_str());
        Logger.printf("   DNS1: %s\n", WiFi.dnsIP(0).toString().c_str());
        Logger.printf("   DNS2: %s\n", WiFi.dnsIP(1).toString().c_str());

        IPAddress resolved;
        Logger.print("   Resolving www.googleapis.com... ");
        if (WiFi.hostByName("www.googleapis.com", resolved)) {
            Logger.printf("OK -> %s\n", resolved.toString().c_str());
        } else {
            Logger.println("FAILED");
        }
    }
    else if (cmd.equalsIgnoreCase("state")) {
        Logger.printf("🔧 [DEBUG] State: Hook=%s, Audio=%s\n",
            Phone.isOffHook() ? "OFF_HOOK" : "ON_HOOK",
            getExtendedAudioPlayer().isActive() ? "PLAYING" : "IDLE");
        Logger.printf("   WiFi: %s, IP: %s\n",
            WiFi.isConnected() ? "Connected" : "Disconnected",
            WiFi.localIP().toString().c_str());
        if (isTailscaleConnected()) {
            Logger.printf("   VPN: %s\n", getTailscaleIP());
        }
        Logger.printf("   Tailscale: %s (saved state)\n", isTailscaleEnabled() ? "enabled" : "disabled");
    }
    else if (cmd.equalsIgnoreCase("tailscale") || cmd.equalsIgnoreCase("vpn")) {
        bool newState = toggleTailscaleEnabled();
        Logger.printf("🔐 Tailscale toggled to: %s\n", newState ? "ENABLED" : "DISABLED");
        Logger.println("   Reboot required for change to take effect");
    }
    else if (cmd.equalsIgnoreCase("fft") || cmd.equalsIgnoreCase("fftdebug")) {
        bool newState = !isFFTDebugEnabled();
        setFFTDebugEnabled(newState);
        Logger.printf("🎵 FFT debug output: %s\n", newState ? "ENABLED" : "DISABLED");
    }
    else if (cmd.startsWith("debugaudio") || cmd.startsWith("debugAudio") || cmd.equalsIgnoreCase("audiodebug")) {
        int durationSec = 20;
        int spaceIdx = cmd.indexOf(' ');
        if (spaceIdx > 0) {
            int val = cmd.substring(spaceIdx + 1).toInt();
            if (val >= 1 && val <= 60) {
                durationSec = val;
            } else {
                Logger.println("⚠️  Invalid duration (1-60 seconds), using default 20s");
            }
        }

        captureOnNextOffHook = true;
        captureRequestedDuration = durationSec;
        saveAudioCaptureState();
        Logger.printf("✅ Audio capture armed: will capture %d seconds on next off-hook\n", durationSec);
        Logger.println("   Pick up the phone to trigger capture");
        Logger.println("   (Saved to NVS - survives reboot)");
    }
#ifdef TEST_MODE

    else if (cmd.equalsIgnoreCase("audiotest") || cmd.equalsIgnoreCase("atest")) {
        performAudioOutputTest();
    }
    else if (cmd.startsWith("debuginput") || cmd.startsWith("replayaudio")) {
        // Usage: debuginput [<filename>] [expected:<digits>]
        // e.g.: debuginput /debug_audio.raw expected:#1
        String arg = cmd.substring(cmd.indexOf(' ') + 1);
        arg.trim();

        const char* filename = "/debug_audio.raw";
        const char* expectedDigits = nullptr;
        static char s_argFilename[64];
        static char s_argExpected[32];

        if (arg.length() > 0 && arg != cmd) {
            int expIdx = arg.indexOf("expected:");
            if (expIdx >= 0) {
                String expStr = arg.substring(expIdx + 9);
                expStr.trim();
                strncpy(s_argExpected, expStr.c_str(), sizeof(s_argExpected) - 1);
                s_argExpected[sizeof(s_argExpected) - 1] = '\0';
                expectedDigits = s_argExpected;
                arg = arg.substring(0, expIdx);
                arg.trim();
            }
            if (arg.length() > 0) {
                strncpy(s_argFilename, arg.c_str(), sizeof(s_argFilename) - 1);
                s_argFilename[sizeof(s_argFilename) - 1] = '\0';
                filename = s_argFilename;
            }
        }

        performDebugInput(filename, expectedDigits);
    }
    else if (cmd.equalsIgnoreCase("sddebug") || cmd.equalsIgnoreCase("sdtest")) {
        performSDCardDebug();
    }
    #endif
    else if (cmd.startsWith("pullota ") || cmd.startsWith("otapull ")) {
        String url = cmd.substring(cmd.indexOf(' ') + 1);
        url.trim();
        if (url.length() > 0) {
            Logger.printf("📥 Starting pull OTA from: %s\n", url.c_str());
            if (!performPullOTA(url.c_str())) {
                Logger.println("❌ Pull OTA failed");
            }
        } else {
            Logger.println("❌ Usage: pullota <firmware_url>");
        }
    }
    else if (cmd.equalsIgnoreCase("bootloader") || cmd.equalsIgnoreCase("flash") || cmd.equalsIgnoreCase("update")) {
        enterFirmwareUpdateMode();
    }
    else if (cmd.startsWith("level ")) {
        int level = cmd.substring(6).toInt();
        if (level >= 0 && level <= 2) {
            Logger.setLogLevel((LogLevel)level);
            Logger.printf("🔧 [DEBUG] Log level set to: %d\n", level);
        }
    }
    else {
        // Treat as DTMF digit sequence
        bool validSequence = true;
        for (size_t i = 0; i < cmd.length() && validSequence; i++) {
            char digit = cmd.charAt(i);
            if (!((digit >= '0' && digit <= '9') || digit == '#' || digit == '*')) {
                validSequence = false;
            }
        }

        if (validSequence && cmd.length() > 0) {
            Logger.printf("🔧 [DEBUG] Simulating DTMF sequence: %s\n", cmd.c_str());
            for (size_t i = 0; i < cmd.length(); i++) {
                addDtmfDigit(cmd.charAt(i));
            }
        } else if (cmd.length() > 0) {
            Logger.printf("🔧 [DEBUG] Unknown command: %s (type 'help' for list)\n", cmd.c_str());
        }
    }
}
