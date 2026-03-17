#include "commands_internal.h"

// ============================================================================
// DEBUG INPUT — Full E2E integration test of the phone call state machine
// ============================================================================
//
// Test sequence:
//   0. Load raw audio from SD (or download CSV from GitHub and convert)
//   1. Clean start (on-hook)
//   2. Off-hook → validate dialtone plays and writes bytes
//   3. Let live Goertzel run 1 s (no false DTMF from dialtone)
//   4. Stop Goertzel, feed test audio → collect DTMF detections
//   5. Feed digits via main-loop pump → validate sequence + audio
//   5b. Replay test audio while locked → verify digits rejected
//   6. Wait real OFF_HOOK_TIMEOUT_MS → validate warning tone
//   6b. Replay test audio again → still locked
//   7. On-hook → validate full reset
//   8. Digits rejected while on-hook
//   9. Wait 1 s, off-hook again
//  10. Enter 6969 → validate sequence plays
//  11. Hang up → validate cleanup
// ============================================================================

#define GITHUB_RAW_BASE "https://raw.githubusercontent.com/jeff-hamm/bowie-phone/main/"

// ============================================================================
// MAIN-LOOP PUMP (private)
// Faithfully replicates the off-hook path of loop() in main.ino.
// ============================================================================

static bool s_pumpWarningPlayed = false;
static unsigned long s_pumpWarningTime = 0;

static size_t pumpMainLoop(unsigned long durationMs) {
    ExtendedAudioPlayer& ap = getExtendedAudioPlayer();
    size_t totalBytes = 0;

    unsigned long start = millis();
    while (millis() - start < durationMs) {
        if (!Phone.isOffHook()) break;

        // ── Off-hook timeout (mirrors main loop) ──
        unsigned long now          = millis();
        unsigned long lastActivity = max(getLastDigitTime(), ap.getLastActive());
        bool audioInProgress = ap.isActive() && !ap.isAudioKeyPlaying("dialtone");
        if (!s_pumpWarningPlayed && !audioInProgress && lastActivity > 0 &&
            (now - lastActivity) >= OFF_HOOK_TIMEOUT_MS) {
            Logger.println("   ⏰ Off-hook timeout fired");
            ap.playAudioKey("off_hook");
            s_pumpWarningPlayed = true;
            s_pumpWarningTime   = now;
        }
        if (s_pumpWarningPlayed) {
            if (getLastDigitTime() > s_pumpWarningTime) {
                s_pumpWarningPlayed = false;
            } else if (!ap.isActive() && (now - s_pumpWarningTime) > 2000) {
                s_pumpWarningPlayed = false;
            }
        }

        // ── Audio + Goertzel mute (mirrors main loop) ──
        if (ap.isActive()) {
            ap.copy();
            totalBytes += ap.getLastCopyBytes();
            bool playingDialtone = ap.isAudioKeyPlaying("dialtone");
            setGoertzelMuted(!playingDialtone);
        } else {
            setGoertzelMuted(isSequenceLocked());
        }

        // ── Goertzel key + sequence (mirrors main loop) ──
        char key = getGoertzelKey();
        if (key != 0) addDtmfDigit(key);
        if (isSequenceReady()) readDTMFSequence(true);

        yield();
        delay(1);
    }
    return totalBytes;
}

// Reset the pump's off-hook timeout tracking (call at start of each test phase)
static void resetPumpState() {
    s_pumpWarningPlayed = false;
    s_pumpWarningTime   = 0;
}

// ============================================================================
// GOERTZEL REPLAY HELPER (private)
// Feed a raw int16 buffer through GoertzelStream. Returns detection count.
// Detected chars are appended to out[0..outSize-1].
// ============================================================================

static int replayAudioThroughGoertzel(
    uint8_t* buffer, size_t fileSize,
    GoertzelStream& goertzel, const PhoneConfig& config,
    char* out, int outSize, int* outPos)
{
    MemoryStream memStream(buffer, fileSize, true, FLASH_RAM);
    memStream.begin();
    StreamCopy replayCopier(goertzel, memStream);
    replayCopier.resize(config.goertzelCopierBufferSize);

    size_t totalProcessed = 0;
    int detections = 0;

    while (totalProcessed < fileSize) {
        size_t copied = replayCopier.copy();
        if (copied == 0) break;
        totalProcessed += copied;
        processGoertzelBlock();

        char key = getGoertzelKey();
        if (key != 0) {
            detections++;
            float offsetSec = (float)totalProcessed / sizeof(int16_t) / AUDIO_SAMPLE_RATE;
            Logger.printf("   🎵 DTMF '%c' at %.3f s\n", key, offsetSec);
            if (out && *outPos < outSize - 1) {
                out[(*outPos)++] = key;
                out[*outPos]     = '\0';
            }
        }
        if ((totalProcessed / 4096) % 50 == 0) yield();
    }
    // Flush final pending detection
    char finalKey = getGoertzelKey();
    if (finalKey != 0) {
        detections++;
        Logger.printf("   🎵 DTMF '%c' at end\n", finalKey);
        if (out && *outPos < outSize - 1) {
            out[(*outPos)++] = finalKey;
            out[*outPos]     = '\0';
        }
    }
    return detections;
}

// ============================================================================
// CSV DOWNLOAD + CONVERT HELPER (private)
// Downloads CSV from GitHub, converts int16 CSV values to raw int16 LE,
// upsamples 2x (22050 → 44100 Hz), saves to SD at rawPath.
// ============================================================================

static bool downloadAndConvertCSV(const char* rawPath) {
    String url = String(GITHUB_RAW_BASE) + "data/" + OTA_HOSTNAME + ".csv";
    Logger.printf("   Downloading: %s\n", url.c_str());

    HttpClient http;
    if (!http.get(url)) {
        Logger.printf("   ❌ HTTP %d\n", http.statusCode());
        return false;
    }

    File outFile = SD_MMC.open(rawPath, FILE_WRITE);
    if (!outFile) {
        Logger.printf("   ❌ Cannot create %s\n", rawPath);
        http.end();
        return false;
    }

    bool inData = false;
    size_t samplesWritten = 0;
    char lineBuf[512];
    int linePos = 0;

    http.streamBody([&](const uint8_t* buf, size_t len) -> bool {
        for (size_t i = 0; i < len; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (linePos == 0) continue;
                lineBuf[linePos] = '\0';
                linePos = 0;

                if (strstr(lineBuf, "---END_AUDIO_CAPTURE---")) return false;
                if (strstr(lineBuf, "---BEGIN_AUDIO_CAPTURE---")) { inData = true; continue; }
                if (lineBuf[0] == '#') continue;
                if (!inData) continue;

                // Parse comma-separated int16 values, upsample 2x
                char* ptr = lineBuf;
                while (*ptr) {
                    while (*ptr == ',' || *ptr == ' ' || *ptr == '\t') ptr++;
                    if (*ptr == '\0') break;
                    int16_t val = (int16_t)atoi(ptr);
                    outFile.write((uint8_t*)&val, sizeof(val));
                    outFile.write((uint8_t*)&val, sizeof(val));
                    samplesWritten += 2;
                    if (*ptr == '-') ptr++;
                    while (*ptr >= '0' && *ptr <= '9') ptr++;
                }

                if (samplesWritten % 44100 == 0) yield();
            } else if (linePos < (int)sizeof(lineBuf) - 1) {
                lineBuf[linePos++] = c;
            }
        }
        return true;
    });

    outFile.close();

    Logger.printf("   ✅ Converted: %u samples (%.2f s at %d Hz) → %s\n",
                  (unsigned)samplesWritten,
                  (float)samplesWritten / AUDIO_SAMPLE_RATE,
                  AUDIO_SAMPLE_RATE, rawPath);
    return samplesWritten > 0;
}

// ============================================================================
// PERFORM DEBUG INPUT — E2E test entry point
// ============================================================================

void performDebugInput(const char* filename, const char* expectedDigits) {
    extern GoertzelStream goertzel;
    extern StreamCopy goertzelCopier;
    ExtendedAudioPlayer& ap = getExtendedAudioPlayer();

    int passed = 0, failed = 0;
    #define TEST_CHECK(label, cond) do { \
        if (cond) { Logger.printf("   ✅ PASS: %s\n", label); passed++; } \
        else      { Logger.printf("   ❌ FAIL: %s\n", label); failed++; } \
    } while(0)

    Logger.println();
    Logger.println("============================================");
    Logger.println("🔬 DEBUG INPUT — E2E INTEGRATION TEST");
    Logger.println("============================================");
    Logger.printf("   File: %s\n", filename);

    // ── Step 0: Load or download test audio ─────────────────────────────
    Logger.println();
    Logger.println("── Step 0: Load test audio ─────────────────");

    if (!SD_MMC.exists(filename)) {
        Logger.println("   File not on SD — attempting GitHub download...");
        if (!downloadAndConvertCSV(filename)) {
            Logger.println("   ❌ Download failed. Upload via: POST /upload");
            Logger.println("============================================");
            return;
        }
    }

    File file = SD_MMC.open(filename, FILE_READ);
    if (!file) {
        Logger.printf("   ❌ Cannot open %s\n", filename);
        Logger.println("============================================");
        return;
    }

    size_t fileSize       = file.size();
    size_t sampleCount    = fileSize / sizeof(int16_t);
    float fileDurationSec = (float)sampleCount / AUDIO_SAMPLE_RATE;

    Logger.printf("   Size: %u bytes (%u samples, %.2f s at %d Hz)\n",
                  (unsigned)fileSize, (unsigned)sampleCount,
                  fileDurationSec, AUDIO_SAMPLE_RATE);

    if (fileSize > ESP.getFreePsram()) {
        Logger.println("   ❌ Not enough PSRAM!");
        file.close();
        Logger.println("============================================");
        return;
    }

    uint8_t* buffer = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        Logger.println("   ❌ PSRAM allocation failed!");
        file.close();
        Logger.println("============================================");
        return;
    }

    size_t bytesRead = file.read(buffer, fileSize);
    file.close();
    if (bytesRead != fileSize) {
        Logger.printf("   ❌ Read error: got %u of %u bytes\n",
                      (unsigned)bytesRead, (unsigned)fileSize);
        heap_caps_free(buffer);
        Logger.println("============================================");
        return;
    }
    Logger.println("   ✅ File loaded into PSRAM");

    const PhoneConfig& config = getPhoneConfig();

    // ── Step 1: Ensure clean starting state (on-hook) ───────────────────
    Logger.println();
    Logger.println("── Step 1: Clean start (on-hook) ───────────");
    if (Phone.isOffHook()) {
        Phone.setOffHook(false);
        delay(50);
    }
    ap.stop();
    resetDTMFSequence();
    TEST_CHECK("Phone is on-hook",         !Phone.isOffHook());
    TEST_CHECK("Player is idle",           !ap.isActive());
    TEST_CHECK("Sequence not locked",      !isSequenceLocked());

    // ── Step 2: Go off-hook → dialtone should start ─────────────────────
    Logger.println();
    Logger.println("── Step 2: Off-hook → dialtone ─────────────");
    resetPumpState();
    Phone.setOffHook(true);
    delay(50);

    TEST_CHECK("Phone is off-hook",        Phone.isOffHook());
    TEST_CHECK("Dialtone playing",         ap.isAudioKeyPlaying("dialtone"));

    size_t dialtoneBytes = pumpMainLoop(200);
    TEST_CHECK("Dialtone writing bytes",   dialtoneBytes > 0);
    Logger.printf("   (pumped %u bytes in 200 ms)\n", (unsigned)dialtoneBytes);

    // ── Step 3: Let live Goertzel run for 1 s ───────────────────────────
    Logger.println();
    Logger.println("── Step 3: Live Goertzel for 1 s ───────────");
    Logger.println("   Goertzel task still running on core 0...");

    getGoertzelKey(); // drain stale detections

    int liveDetections = 0;
    unsigned long liveStart = millis();
    while (millis() - liveStart < 1000) {
        if (ap.isActive()) ap.copy();
        char key = getGoertzelKey();
        if (key != 0) {
            Logger.printf("   ⚠️  Live Goertzel detected '%c' during dialtone\n", key);
            liveDetections++;
        }
        delay(5);
    }
    TEST_CHECK("No false DTMF during dialtone", liveDetections == 0);

    // ── Step 4: Stop Goertzel task, feed test audio ─────────────────────
    Logger.println();
    Logger.println("── Step 4: Feed test audio → Goertzel ──────");
    stopGoertzelTask();
    delay(50);
    ap.stop();
    resetGoertzelState();
    setGoertzelMuted(false);

    Logger.printf("   Block: %d samples (%.1f ms), Threshold: %.1f, Floor: %.1f\n",
                  config.goertzelBlockSize,
                  config.goertzelBlockSize * 1000.0f / AUDIO_SAMPLE_RATE,
                  config.fundamentalMagnitudeThreshold,
                  config.minDetectionMagnitude);

    char detectedDigits[32];
    int digitPos = 0;
    detectedDigits[0] = '\0';
    unsigned long replayStart = millis();
    int detectionCount = replayAudioThroughGoertzel(
        buffer, fileSize, goertzel, config,
        detectedDigits, sizeof(detectedDigits), &digitPos);
    unsigned long replayElapsed = millis() - replayStart;

    Logger.printf("   Processed in %lu ms (%.1fx realtime)\n",
                  replayElapsed, fileDurationSec * 1000.0f / max(replayElapsed, 1UL));
    Logger.printf("   Detected digits: \"%s\" (%d total)\n", detectedDigits, detectionCount);
    TEST_CHECK("At least one DTMF detection", detectionCount > 0);
    if (expectedDigits) {
        bool matches = strcmp(detectedDigits, expectedDigits) == 0;
        if (!matches)
            Logger.printf("   Expected: \"%s\", got: \"%s\"\n", expectedDigits, detectedDigits);
        TEST_CHECK("Detected digits match expected", matches);
    }

    // ── Step 5: Feed digits via main-loop pump ──────────────────────────
    Logger.println();
    Logger.println("── Step 5: Sequence via main loop ──────────");

    resetGoertzelState();
    startGoertzelTask(goertzelCopier);

    ap.playAudioKey("dialtone");
    pumpMainLoop(50);

    // Determine if any suffix of the detected digits is a registered audio key.
    // If not, there's nothing to lock on — skip the audio/lock assertions.
    bool detectedKeyExists = false;
    for (int start = 0; start < digitPos && !detectedKeyExists; start++) {
        if (getAudioKeyRegistry().hasKey(&detectedDigits[start]))
            detectedKeyExists = true;
    }
    if (!detectedKeyExists)
        Logger.printf("   ℹ️  No registered key in \"%s\" — skipping audio/lock checks\n",
                      detectedDigits);

    if (digitPos > 0) {
        Logger.printf("   Feeding %d digits: \"%s\"\n", digitPos, detectedDigits);
        for (int i = 0; i < digitPos; i++) {
            addDtmfDigit(detectedDigits[i]);
            pumpMainLoop(10);
        }

        Logger.println("   Pumping main loop for sequence processing...");
        unsigned long seqStart = millis();
        while (millis() - seqStart < 3000) {
            pumpMainLoop(50);
            if (isSequenceLocked()) break;
            if (!isReadingSequence() && !isSequenceReady()) break;
        }

        TEST_CHECK("Dialtone stopped after first digit", !ap.isAudioKeyPlaying("dialtone"));

        if (detectedKeyExists) {
            bool audioStarted = ap.isActive();
            TEST_CHECK("Audio started after sequence", audioStarted);
            if (audioStarted) {
                Logger.printf("   Playing: %s\n",
                              ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(ringback/queue)");
            }
            TEST_CHECK("Sequence locked after match", isSequenceLocked());
        } else {
            Logger.println("   ⏭️  Skipped: audio/lock checks (no key in detected digits)");
        }
    } else {
        Logger.println("   ⚠️  No digits to feed — skipping sequence test");
    }

    // ── Step 5b: Replay while locked → no new sequences ─────────────────
    Logger.println();
    Logger.println("── Step 5b: Replay while locked ────────────");
    Logger.println("   Finishing active audio...");
    unsigned long finishStart = millis();
    while (ap.isActive() && (millis() - finishStart < 15000)) {
        pumpMainLoop(100);
    }

    if (detectedKeyExists) {
        TEST_CHECK("Sequence still locked", isSequenceLocked());
    } else {
        Logger.println("   ⏭️  Skipped: lock check (no key was triggered)");
    }
    Logger.println("   Replaying test audio (should detect nothing usable)...");

    stopGoertzelTask();
    delay(50);
    resetGoertzelState();
    setGoertzelMuted(false);

    char lockedDigits[32];
    int lockedPos = 0;
    lockedDigits[0] = '\0';
    int lockedDetections = replayAudioThroughGoertzel(
        buffer, fileSize, goertzel, config,
        lockedDigits, sizeof(lockedDigits), &lockedPos);

    Logger.printf("   Goertzel detected %d digits: \"%s\"\n", lockedDetections, lockedDigits);
    const char* seqBefore5b = getSequence();
    for (int i = 0; i < lockedPos; i++) {
        addDtmfDigit(lockedDigits[i]);
    }
    TEST_CHECK("Digits rejected while locked", strlen(getSequence()) == strlen(seqBefore5b));

    resetGoertzelState();
    startGoertzelTask(goertzelCopier);

    // ── Step 6: Wait real OFF_HOOK_TIMEOUT_MS for warning tone ──────────
    Logger.println();
    Logger.println("── Step 6: Off-hook timeout (real wait) ────");
    Logger.printf("   Waiting up to %d s for off-hook timeout...\n",
                  (OFF_HOOK_TIMEOUT_MS / 1000) + 5);
    Logger.println("   (lastActivity will age out naturally)");

    bool warningDetected = false;
    unsigned long timeoutStart = millis();
    unsigned long maxWait      = OFF_HOOK_TIMEOUT_MS + 5000;

    while (millis() - timeoutStart < maxWait) {
        pumpMainLoop(500);
        if (ap.isAudioKeyPlaying("off_hook")) {
            warningDetected = true;
            Logger.printf("   ⏰ off_hook warning detected at +%lu ms\n",
                          millis() - timeoutStart);
            break;
        }
        unsigned long elapsed = millis() - timeoutStart;
        if (elapsed % 5000 < 500) {
            Logger.printf("   ... waiting (%lu / %lu ms)\n", elapsed, maxWait);
        }
    }
    TEST_CHECK("Off-hook warning tone played", warningDetected);

    if (ap.isActive()) {
        Logger.println("   Pumping warning audio...");
        unsigned long warnPump = millis();
        while (ap.isActive() && (millis() - warnPump < 10000)) {
            pumpMainLoop(100);
        }
    }

    // ── Step 6b: Replay after timeout → still locked ────────────────────
    Logger.println();
    Logger.println("── Step 6b: Replay after timeout ───────────");
    TEST_CHECK("Still locked after timeout", isSequenceLocked());

    stopGoertzelTask();
    delay(50);
    resetGoertzelState();
    setGoertzelMuted(false);

    char timeoutDigits[32];
    int timeoutPos = 0;
    timeoutDigits[0] = '\0';
    int timeoutDetections = replayAudioThroughGoertzel(
        buffer, fileSize, goertzel, config,
        timeoutDigits, sizeof(timeoutDigits), &timeoutPos);

    Logger.printf("   Goertzel detected %d digits\n", timeoutDetections);
    const char* seqBefore6b = getSequence();
    for (int i = 0; i < timeoutPos; i++) {
        addDtmfDigit(timeoutDigits[i]);
    }
    TEST_CHECK("Digits still rejected (locked)", strlen(getSequence()) == strlen(seqBefore6b));

    resetGoertzelState();
    startGoertzelTask(goertzelCopier);

    // ── Step 7: On-hook → validate full reset ───────────────────────────
    Logger.println();
    Logger.println("── Step 7: On-hook → reset ─────────────────");
    Phone.setOffHook(false);
    delay(50);

    TEST_CHECK("Phone is on-hook",              !Phone.isOffHook());
    TEST_CHECK("Player stopped",                !ap.isActive());
    TEST_CHECK("Sequence reset (not locked)",   !isSequenceLocked());
    TEST_CHECK("Sequence buffer clear",         strlen(getSequence()) == 0);

    // ── Step 8: Digits rejected while on-hook ───────────────────────────
    Logger.println();
    Logger.println("── Step 8: Digits rejected on-hook ─────────");
    addDtmfDigit('5');
    delay(10);
    TEST_CHECK("No audio playing after on-hook digit", !ap.isActive());
    resetDTMFSequence();

    // ── Step 9: Wait 1 s, then pick up again ────────────────────────────
    Logger.println();
    Logger.println("── Step 9: Wait 1 s, off-hook again ────────");
    delay(1000);
    resetPumpState();
    Phone.setOffHook(true);
    delay(50);

    TEST_CHECK("Phone is off-hook again",       Phone.isOffHook());
    TEST_CHECK("Dialtone restarted",            ap.isAudioKeyPlaying("dialtone"));
    size_t dt2Bytes = pumpMainLoop(200);
    TEST_CHECK("Dialtone writing bytes",        dt2Bytes > 0);

    // ── Step 10: Enter 6969 → validate sequence plays ───────────────────
    Logger.println();
    Logger.println("── Step 10: Dial 6969 ──────────────────────");
    Logger.println("   Feeding digits: 6-9-6-9");
    const char* dialSequence = "6969";
    for (int i = 0; dialSequence[i]; i++) {
        addDtmfDigit(dialSequence[i]);
        pumpMainLoop(10);
    }

    Logger.println("   Pumping main loop for sequence...");
    unsigned long dialStart = millis();
    while (millis() - dialStart < 5000) {
        pumpMainLoop(50);
        if (isSequenceLocked()) break;
        if (!isReadingSequence() && !isSequenceReady()) break;
    }

    TEST_CHECK("Dialtone stopped",              !ap.isAudioKeyPlaying("dialtone"));
    bool dialAudioPlaying = ap.isActive();
    TEST_CHECK("Audio playing after 6969",      dialAudioPlaying);
    if (dialAudioPlaying) {
        Logger.printf("   Playing: %s\n",
                      ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(playlist)");
    }

    size_t dialBytes = pumpMainLoop(500);
    TEST_CHECK("Audio writing bytes",           dialBytes > 0);
    Logger.printf("   (pumped %u bytes in 500 ms)\n", (unsigned)dialBytes);

    // ── Step 11: Hang up → final cleanup ────────────────────────────────
    Logger.println();
    Logger.println("── Step 11: Hang up → cleanup ──────────────");
    Phone.setOffHook(false);
    delay(50);

    TEST_CHECK("Phone is on-hook",              !Phone.isOffHook());
    TEST_CHECK("Player stopped",                !ap.isActive());
    TEST_CHECK("Sequence reset (not locked)",   !isSequenceLocked());
    TEST_CHECK("Sequence buffer clear",         strlen(getSequence()) == 0);

    // ── Cleanup ─────────────────────────────────────────────────────────
    Logger.println();
    Logger.println("── Cleanup ─────────────────────────────────");
    heap_caps_free(buffer);
    Logger.printf("   PSRAM freed. Available: %u KB\n", ESP.getFreePsram() / 1024);

    Phone.resetDebugOverride();
    Logger.println("   Hook override cleared");

    // ── Summary ─────────────────────────────────────────────────────────
    Logger.println();
    Logger.println("============================================");
    Logger.printf("📊 E2E TEST RESULTS: %d passed, %d failed\n", passed, failed);
    if (failed == 0) {
        Logger.println("   🎉 ALL TESTS PASSED");
    } else {
        Logger.println("   ⚠️  SOME TESTS FAILED — review above");
    }
    Logger.println("============================================");

    #undef TEST_CHECK
}
