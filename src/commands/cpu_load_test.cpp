#include "commands_internal.h"

// ============================================================================
// CPU LOAD TEST — Goertzel task + audio playback concurrency measurement
// ============================================================================

void performGoertzelCPULoadTest() {
    extern GoertzelStream goertzel;
    extern StreamCopy goertzelCopier;
    extern AudioBoardStream kit;

    Logger.println("🔬 CPU Load Test: Goertzel Task + Audio");
    Logger.println("============================================");

    resetGoertzelState();

    Logger.println("Starting Goertzel task on core 0...");
    startGoertzelTask(goertzelCopier);
    delay(50);

    Logger.println("Starting dial tone playback...");
    getExtendedAudioPlayer().playAudioKey("dialtone");
    delay(100);

    if (!getExtendedAudioPlayer().isActive()) {
        Logger.println("❌ Failed to start audio - test aborted");
        stopGoertzelTask();
        return;
    }

    const int TEST_DURATION_MS  = 5000;
    const int SAMPLE_INTERVAL_MS = 100;

    unsigned long testStart        = millis();
    unsigned long loopCount        = 0;
    unsigned long dtmfDetectCount  = 0;
    unsigned long maxLoopTime      = 0;
    unsigned long minLoopTime      = ULONG_MAX;
    unsigned long totalLoopTime    = 0;
    unsigned long audioUnderrunCount = 0;
    unsigned long lastSample       = millis();

    size_t totalBytesWritten = 0;

    Logger.printf("Running for %d seconds (Goertzel on core 0)...\n", TEST_DURATION_MS / 1000);

    while (millis() - testStart < TEST_DURATION_MS) {
        unsigned long loopStart = micros();

        if (getExtendedAudioPlayer().isActive()) {
            getExtendedAudioPlayer().copy();
            totalBytesWritten += getExtendedAudioPlayer().getLastCopyBytes();
        } else {
            audioUnderrunCount++;
            getExtendedAudioPlayer().playAudioKey("dialtone");
        }

        char key = getGoertzelKey();
        if (key != 0) dtmfDetectCount++;

        unsigned long loopTime = micros() - loopStart;
        totalLoopTime += loopTime;
        loopCount++;

        if (loopTime > maxLoopTime) maxLoopTime = loopTime;
        if (loopTime < minLoopTime) minLoopTime = loopTime;

        if (millis() - lastSample >= SAMPLE_INTERVAL_MS * 10) {
            Logger.print(".");
            lastSample = millis();
        }

        yield();
    }

    stopGoertzelTask();
    getExtendedAudioPlayer().stop();

    unsigned long avgLoopTime      = loopCount > 0 ? totalLoopTime / loopCount : 0;
    float loopsPerSecond           = loopCount * 1000.0f / TEST_DURATION_MS;
    float expectedGoertzelRate     = 44100.0f / 512.0f;
    float expectedBytesPerSec      = (float)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
    float expectedBytes            = expectedBytesPerSec * TEST_DURATION_MS / 1000.0f;

    Logger.println();
    Logger.println("============================================");
    Logger.println("📊 Results:");
    Logger.printf("   Test duration: %lu ms\n", TEST_DURATION_MS);
    Logger.printf("   Total loops: %lu (%.1f/sec)\n", loopCount, loopsPerSecond);
    Logger.printf("   Goertzel: running on core 0 (separate task)\n");
    Logger.printf("   Expected Goertzel rate: %.1f blocks/sec\n", expectedGoertzelRate);
    Logger.printf("   DTMF keys detected: %lu (should be 0 for dial tone)\n", dtmfDetectCount);
    Logger.println();
    Logger.println("⏱️ Main Loop Timing (microseconds):");
    Logger.printf("   Min: %lu µs\n", minLoopTime);
    Logger.printf("   Max: %lu µs\n", maxLoopTime);
    Logger.printf("   Avg: %lu µs\n", avgLoopTime);
    Logger.println();
    Logger.println("🔊 Audio Throughput:");
    Logger.printf("   Bytes written: %u (%.1f KB/s)\n",
                  (unsigned)totalBytesWritten,
                  totalBytesWritten * 1000.0f / TEST_DURATION_MS / 1024.0f);
    Logger.printf("   Expected:      %.0f (%.1f KB/s @ %dHz/%dch/%dbit)\n",
                  expectedBytes, expectedBytesPerSec / 1024.0f,
                  AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS_PER_SAMPLE);
    if (totalBytesWritten > 0) {
        Logger.printf("   Throughput:    %.1f%%\n", totalBytesWritten * 100.0f / expectedBytes);
    }
    Logger.println();
    Logger.printf("⚠️ Audio restarts (underruns): %lu\n", audioUnderrunCount);
    Logger.println();

    Logger.println("📋 Assessment:");
    if (maxLoopTime > 50000) {
        Logger.println("   ❌ FAIL: Max loop time > 50ms - will cause audio glitches");
    } else if (maxLoopTime > 23000) {
        Logger.println("   ⚠️ WARN: Max loop time > 23ms - audio may stutter");
    } else {
        Logger.println("   ✅ PASS: Loop timing acceptable for audio");
    }

    if (audioUnderrunCount > 0) {
        Logger.println("   ❌ FAIL: Audio underruns detected");
    } else {
        Logger.println("   ✅ PASS: No audio underruns");
    }

    if (totalBytesWritten == 0) {
        Logger.println("   ❌ FAIL: Zero bytes written - no audio reaching DAC");
    } else if (totalBytesWritten < expectedBytes * 0.5f) {
        Logger.printf("   ⚠️ WARN: Low throughput (%.0f%%) - audio may stutter\n",
                      totalBytesWritten * 100.0f / expectedBytes);
    } else {
        Logger.printf("   ✅ PASS: Audio throughput %.0f%%\n",
                      totalBytesWritten * 100.0f / expectedBytes);
    }

    if (dtmfDetectCount > 0) {
        Logger.println("   ⚠️ WARN: False DTMF detections - increase threshold");
    } else {
        Logger.println("   ✅ PASS: No false DTMF detections");
    }

    Logger.printf("\n💾 Free heap: %u bytes\n", ESP.getFreeHeap());
    Logger.println("============================================");
}
