#include "commands_internal.h"

// ============================================================================
// AUDIO OUTPUT TEST — Verify I2S data flow without physical speaker access
// ============================================================================

void performAudioOutputTest() {
    extern AudioBoardStream kit;
    ExtendedAudioPlayer& ap = getExtendedAudioPlayer();

    Logger.println();
    Logger.println("============================================");
    Logger.println("🔊 AUDIO OUTPUT TEST");
    Logger.println("============================================");

    // ── 1. Current player state ──────────────────────────────────────────
    Logger.println();
    Logger.println("📋 Player State:");
    Logger.printf("   Active:       %s\n", ap.isActive() ? "YES" : "no");
    Logger.printf("   Current key:  %s\n", ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(none)");
    Logger.printf("   Stream type:  %d\n", (int)ap.getCurrentStreamType());
    Logger.printf("   Queue depth:  %d\n", (int)ap.getQueueSize());
    Logger.printf("   Volume:       %.2f\n", ap.getVolume());
    Logger.printf("   Hook state:   %s\n", Phone.isOffHook() ? "OFF HOOK" : "ON HOOK");
    Logger.printf("   Download Q:   %d pending\n", getDownloadQueueCount());

    // ── 2. Registry contents ──────────────────────────────────────────────
    AudioKeyRegistry* reg = ap.getRegistry();
    if (!reg) {
        Logger.println("❌ No registry set on player!");
        Logger.println("============================================");
        return;
    }
    Logger.printf("   Registry:     %d keys\n", (int)reg->size());
    if (reg->size() == 0) {
        Logger.println("❌ Registry is empty - audio catalog not loaded");
        Logger.println("   Check SD card and WiFi connectivity");
        Logger.println("============================================");
        return;
    }
    Logger.println();
    Logger.println("🔑 Registered keys:");
    for (auto it = reg->begin(); it != reg->end(); ++it) {
        Logger.printf("   %s -> %s (type:%d)\n", it->first.c_str(),
                      it->second.getPath() ? it->second.getPath() : "(no path)",
                      (int)it->second.type);
    }

    // ── 3. Check dialtone key ────────────────────────────────────────────
    bool hasDT = ap.hasAudioKey("dialtone");
    Logger.printf("\n   dialtone key: %s\n", hasDT ? "registered" : "NOT FOUND");
    Logger.printf("   dialtone key: %s\n", hasDT ? "registered" : "NOT FOUND");
    if (!hasDT) {
        Logger.println("❌ Cannot test - 'dialtone' audioKey not registered");
        Logger.println("============================================");
        return;
    }

    // ── 4. Start dialtone playback ───────────────────────────────────────
    Logger.println();
    Logger.println("▶️  Starting dialtone playback...");
    bool started = ap.playAudioKey("dialtone");
    Logger.printf("   playAudioKey returned: %s\n", started ? "true" : "FALSE");
    if (!started) {
        Logger.println("❌ Failed to start dialtone - check logs above");
        Logger.println("============================================");
        return;
    }
    delay(50);

    Logger.printf("   Active after start: %s\n", ap.isActive() ? "YES" : "no");
    Logger.printf("   Playing key:        %s\n", ap.getCurrentAudioKey());

    // ── 5. I2S buffer flow test ──────────────────────────────────────────
    const int TEST_DURATION_MS   = 3000;
    const int SAMPLE_INTERVAL_MS = 50;
    const int SAMPLES            = TEST_DURATION_MS / SAMPLE_INTERVAL_MS;

    Logger.println();
    Logger.printf("🔬 Monitoring I2S output for %d ms...\n", TEST_DURATION_MS);

    int copyTrueCount       = 0;
    int copyFalseCount      = 0;
    int playerActiveCount   = 0;
    int playerInactiveCount = 0;
    long totalBytesWritten  = 0;
    int maxAvail  = 0;
    int minAvail  = INT_MAX;
    long totalAvail = 0;
    int restartCount = 0;
    unsigned long testStart = millis();

    for (int i = 0; i < SAMPLES; i++) {
        int availBefore = kit.availableForWrite();
        bool copyResult = ap.copy();
        int availAfter  = kit.availableForWrite();

        int delta = availBefore - availAfter;
        if (delta > 0) totalBytesWritten += delta;

        if (copyResult) copyTrueCount++; else copyFalseCount++;
        if (ap.isActive()) playerActiveCount++; else playerInactiveCount++;

        if (availAfter < minAvail) minAvail = availAfter;
        if (availAfter > maxAvail) maxAvail = availAfter;
        totalAvail += availAfter;

        if (!ap.isActive() && i < SAMPLES - 1) {
            restartCount++;
            ap.playAudioKey("dialtone");
            delay(10);
        }

        delay(SAMPLE_INTERVAL_MS);
    }

    unsigned long elapsed = millis() - testStart;

    // ── 6. Results ───────────────────────────────────────────────────────
    Logger.println();
    Logger.println("📊 Results:");
    Logger.printf("   Duration:          %lu ms\n", elapsed);
    Logger.printf("   copy() true/false: %d / %d\n", copyTrueCount, copyFalseCount);
    Logger.printf("   Player active/not: %d / %d\n", playerActiveCount, playerInactiveCount);
    Logger.printf("   Restarts needed:   %d\n", restartCount);
    Logger.println();
    Logger.println("🔊 I2S TX Buffer (availableForWrite):");
    Logger.printf("   Min: %d  Max: %d  Avg: %ld\n", minAvail, maxAvail,
                  SAMPLES > 0 ? totalAvail / SAMPLES : 0);
    Logger.printf("   Est. bytes written: %ld\n", totalBytesWritten);

    float expectedBytesPerSec = (float)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
    float expectedBytes       = expectedBytesPerSec * elapsed / 1000.0f;
    Logger.printf("   Expected bytes:     %.0f (%.0f B/s @ %dHz/%dch/%dbit)\n",
                  expectedBytes, expectedBytesPerSec,
                  AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS_PER_SAMPLE);

    if (totalBytesWritten > 0) {
        float ratio = totalBytesWritten / expectedBytes * 100.0f;
        Logger.printf("   Throughput ratio:   %.1f%%\n", ratio);
    }

    // ── 7. Final state ───────────────────────────────────────────────────
    Logger.println();
    Logger.printf("   Player still active: %s\n", ap.isActive() ? "YES" : "no");
    Logger.printf("   Current key:         %s\n",
                  ap.getCurrentAudioKey() ? ap.getCurrentAudioKey() : "(none)");

    // ── 8. Assessment ────────────────────────────────────────────────────
    Logger.println();
    Logger.println("📋 Assessment:");
    if (copyTrueCount == 0) {
        Logger.println("   ❌ FAIL: copy() never returned true - no audio pipeline activity");
    } else if (copyFalseCount > copyTrueCount) {
        Logger.println("   ⚠️  WARN: copy() mostly false - pipeline stalling");
    } else {
        Logger.printf("   ✅ PASS: copy() active %d/%d samples\n", copyTrueCount, SAMPLES);
    }

    if (totalBytesWritten == 0) {
        Logger.println("   ❌ FAIL: Zero bytes written to I2S - no audio reaching DAC");
    } else if (totalBytesWritten < expectedBytes * 0.3f) {
        Logger.printf("   ⚠️  WARN: Low throughput (%.0f%% of expected)\n",
                      totalBytesWritten / expectedBytes * 100.0f);
    } else {
        Logger.println("   ✅ PASS: Audio data flowing to I2S");
    }

    if (restartCount > 0) {
        Logger.printf("   ⚠️  WARN: Player stopped %d times during test\n", restartCount);
    } else {
        Logger.println("   ✅ PASS: Playback sustained for full test");
    }

    Logger.printf("\n💾 Free heap: %u bytes\n", ESP.getFreeHeap());
    Logger.println("============================================");

    if (!Phone.isOffHook()) {
        ap.stop();
        Logger.println("⏹️  Stopped dialtone (phone is on-hook)");
    } else {
        Logger.println("📞 Leaving dialtone running (phone is off-hook)");
    }
}
