#include "commands_internal.h"

// ============================================================================
// AUDIO CAPTURE — Record ADC input to PSRAM, dump as CSV over serial/log
// ============================================================================

void performAudioCapture(int durationSec) {
    extern AudioBoardStream kit;

    if (durationSec < 1)  durationSec = 1;
    if (durationSec > 20) durationSec = 20;

    // Downsample factor: 44100 -> 22050 Hz (take every 2nd sample)
    const int DOWNSAMPLE      = 2;
    const int EFFECTIVE_RATE  = AUDIO_SAMPLE_RATE / DOWNSAMPLE; // 22050
    const size_t SAMPLES_NEEDED = EFFECTIVE_RATE * durationSec;
    const size_t BUFFER_BYTES   = SAMPLES_NEEDED * sizeof(int16_t);

    Logger.println();
    Logger.println("============================================");
    Logger.println("🎙️ AUDIO CAPTURE FOR OFFLINE ANALYSIS");
    Logger.println("============================================");
    Logger.printf("   Duration: %d seconds\n", durationSec);
    Logger.printf("   Source rate: %d Hz -> Capture rate: %d Hz\n", AUDIO_SAMPLE_RATE, EFFECTIVE_RATE);
    Logger.printf("   Samples: %u  Buffer: %u KB\n", SAMPLES_NEEDED, BUFFER_BYTES / 1024);
    Logger.printf("   Free PSRAM: %u KB\n", ESP.getFreePsram() / 1024);
    Logger.printf("   Free heap: %u KB\n", ESP.getFreeHeap() / 1024);

    if (BUFFER_BYTES > ESP.getFreePsram()) {
        Logger.println("   ❌ Not enough PSRAM! Reduce duration.");
        return;
    }

    int16_t* captureBuf = (int16_t*)heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    if (!captureBuf) {
        Logger.println("   ❌ PSRAM allocation failed!");
        return;
    }

    Logger.println("   Stopping Goertzel task...");
    stopGoertzelTask();
    getExtendedAudioPlayer().stop();

    Logger.println("   Disabling remote logger...");
    bool wasRemoteEnabled = RemoteLogger.isEnabled();
    RemoteLogger.setEnabled(false);

    delay(50);

    // Drain stale I2S input data
    {
        uint8_t drain[1024];
        unsigned long drainStart = millis();
        while (kit.available() > 0 && (millis() - drainStart) < 200) {
            kit.readBytes(drain, min((int)sizeof(drain), kit.available()));
        }
    }

    Logger.println("   🔴 RECORDING...");
    Logger.flush();

    const size_t READ_CHUNK = 1024;
    uint8_t readBuf[READ_CHUNK];
    size_t capturedSamples   = 0;
    size_t totalSourceSamples = 0;
    size_t sourceSkipCounter  = 0;
    unsigned long captureStart = millis();
    unsigned long lastDot      = captureStart;
    size_t readFailCount       = 0;

    while (capturedSamples < SAMPLES_NEEDED) {
        int avail = kit.available();
        if (avail <= 0) {
            delayMicroseconds(100);
            readFailCount++;
            if (readFailCount > 100000) {
                Logger.printf("\n   ⚠️ I2S read stalled at %u samples\n", capturedSamples);
                break;
            }
            continue;
        }
        readFailCount = 0;

        size_t toRead = min((size_t)avail, READ_CHUNK);
        toRead &= ~1;   // ensure even (16-bit samples)
        if (toRead == 0) continue;

        size_t bytesRead   = kit.readBytes(readBuf, toRead);
        size_t samplesRead = bytesRead / sizeof(int16_t);
        int16_t* samples   = (int16_t*)readBuf;

        for (size_t i = 0; i < samplesRead && capturedSamples < SAMPLES_NEEDED; i++) {
            totalSourceSamples++;
            if (sourceSkipCounter == 0) {
                captureBuf[capturedSamples++] = samples[i];
            }
            sourceSkipCounter++;
            if (sourceSkipCounter >= (size_t)DOWNSAMPLE) {
                sourceSkipCounter = 0;
            }
        }

        if (millis() - lastDot >= 1000) {
            Logger.print(".");
            lastDot = millis();
        }
    }

    if (wasRemoteEnabled) {
        Logger.println("   Re-enabling remote logger...");
        RemoteLogger.setEnabled(true);
    }

    unsigned long captureTime = millis() - captureStart;
    Logger.println();
    Logger.printf("   ✅ Captured %u samples in %lu ms\n", capturedSamples, captureTime);
    Logger.printf("   Source samples read: %u (expected ~%u)\n",
                  totalSourceSamples, (unsigned)(AUDIO_SAMPLE_RATE * durationSec));

    // Signal statistics
    int32_t minVal = 32767, maxVal = -32768;
    int64_t sumAbs = 0;
    for (size_t i = 0; i < capturedSamples; i++) {
        int16_t s = captureBuf[i];
        if (s < minVal) minVal = s;
        if (s > maxVal) maxVal = s;
        sumAbs += abs(s);
    }
    int32_t avgAbs = capturedSamples > 0 ? (int32_t)(sumAbs / capturedSamples) : 0;

    Logger.println();
    Logger.println("📊 Signal Statistics:");
    Logger.printf("   Min: %d  Max: %d  Avg|x|: %d\n", minVal, maxVal, avgAbs);
    Logger.printf("   Peak: %.1f dBFS\n",
                  maxVal > 0 ? 20.0f * log10f((float)max(abs(minVal), abs(maxVal)) / 32768.0f) : -96.0f);

    Logger.println();
    Logger.println("📤 Dumping audio data (CSV signed int16)...");
    Logger.println("   Copy between BEGIN/END markers.");
    Logger.println("   Python: np.loadtxt('file.csv', delimiter=',', dtype=np.int16)");
    Logger.flush();
    delay(100);

    Logger.writeRawLine("---BEGIN_AUDIO_CAPTURE---");

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "# rate=%d,bits=16,channels=1,samples=%u,duration_ms=%lu",
             EFFECTIVE_RATE, capturedSamples, captureTime);
    Logger.writeRawLine(hdr);

    const int SAMPLES_PER_LINE = 20;
    char lineBuf[256];
    size_t dumped = 0;

    while (dumped < capturedSamples) {
        int linePos   = 0;
        int lineCount = 0;

        while (lineCount < SAMPLES_PER_LINE && dumped < capturedSamples) {
            if (lineCount > 0) lineBuf[linePos++] = ',';
            int written = snprintf(lineBuf + linePos, sizeof(lineBuf) - linePos, "%d", captureBuf[dumped]);
            linePos += written;
            dumped++;
            lineCount++;
        }
        lineBuf[linePos] = '\0';
        Logger.writeRawLine(lineBuf);

        if ((dumped / SAMPLES_PER_LINE) % 100 == 0) {
            yield();
            delay(1);
        }
    }

    Logger.writeRawLine("---END_AUDIO_CAPTURE---");
    Logger.printf("\n   ✅ Dumped %u samples (%u lines)\n",
                  dumped, (dumped + SAMPLES_PER_LINE - 1) / SAMPLES_PER_LINE);

    heap_caps_free(captureBuf);
    Logger.printf("   💾 Buffer freed. Free PSRAM: %u KB\n", ESP.getFreePsram() / 1024);

    Logger.println("   Restarting Goertzel task...");
    extern StreamCopy goertzelCopier;
    resetGoertzelState();
    startGoertzelTask(goertzelCopier);

    Logger.println("============================================");
}
