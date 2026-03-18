#include "commands_internal.h"
#ifdef TEST_MODE
// ============================================================================
// SD CARD DEBUG — Test various initialization methods and pin configurations
// ============================================================================

void performSDCardDebug() {
    Logger.println();
    Logger.println("============================================");
    Logger.println("💾 SD CARD INITIALIZATION DEBUG");
    Logger.println("============================================");

    getExtendedAudioPlayer().stop();
    delay(100);

    Logger.printf("📋 Build Config: SD_USE_MMC=%d ", SD_USE_MMC);
    Logger.println(SD_USE_MMC ? "(compiled for SD_MMC)" : "(compiled for SPI)");
    Logger.printf("   Config pins: CS=%d CLK=%d MOSI=%d MISO=%d\n",
                  SD_CS_PIN, SD_CLK_PIN, SD_MOSI_PIN, SD_MISO_PIN);
    Logger.println("   Testing ALL methods with pin variations...");
    Logger.println();

    // ── SD_MMC tests ────────────────────────────────────────────────────
    Logger.println("════════════════════════════════════════════");
    Logger.println("SD_MMC MODE TESTS (Hardware SDMMC)");
    Logger.println("════════════════════════════════════════════");
    Logger.println();

    Logger.println("Test 1: SD_MMC.begin(\"/sdcard\", true) - 1-bit, no format");
    if (SD_MMC.begin("/sdcard", true)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d ", cardType);
        switch(cardType) {
            case 0: Logger.println("(NONE)"); break;
            case 1: Logger.println("(MMC)");  break;
            case 2: Logger.println("(SD)");   break;
            case 3: Logger.println("(SDHC)"); break;
            default: Logger.println("(UNKNOWN)"); break;
        }
        if (cardType != 0) {
            Logger.printf("   Card Size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
            Logger.printf("   Used: %llu MB\n", SD_MMC.usedBytes() / (1024 * 1024));
        }
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);

    Logger.println();
    Logger.println("Test 2: SD_MMC.begin(\"/sdcard\", true, true) - 1-bit, format on fail");
    if (SD_MMC.begin("/sdcard", true, true)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0)
            Logger.printf("   Card Size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);

    Logger.println();
    Logger.println("Test 3: SD_MMC.begin(\"/sdcard\", false) - 4-bit mode");
    if (SD_MMC.begin("/sdcard", false)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0)
            Logger.printf("   Card Size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);

    Logger.println();
    Logger.println("Test 4: SD_MMC.begin(\"/sd\", true) - 1-bit, different mount");
    if (SD_MMC.begin("/sd", true)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0)
            Logger.printf("   Card Size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);

    Logger.println();
    Logger.println("Test 5: SD_MMC.begin(\"/sdcard\", true, false, SDMMC_FREQ_DEFAULT, 5)");
    if (SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
        uint8_t cardType = SD_MMC.cardType();
        Logger.printf("   ✅ SUCCESS - Card Type: %d\n", cardType);
        if (cardType != 0)
            Logger.printf("   Card Size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
        SD_MMC.end();
    } else {
        Logger.println("   ❌ FAILED");
    }
    delay(500);

    // ── SPI SD tests ─────────────────────────────────────────────────────
    Logger.println();
    Logger.println("════════════════════════════════════════════");
    Logger.println("SPI SD MODE TESTS (Software SPI)");
    Logger.println("════════════════════════════════════════════");
    Logger.println("Testing multiple pin configurations...");
    Logger.println();

    struct SPIPinConfig {
        const char* name;
        int cs, clk, mosi, miso;
    };

    SPIPinConfig spiConfigs[] = {
        {"Config.h default",       SD_CS_PIN, SD_CLK_PIN, SD_MOSI_PIN, SD_MISO_PIN},
        {"Alt 1 (13,14,15,2)",     13, 14, 15, 2},
        {"Alt 2 (VSPI)",            5, 18, 23, 19},
        {"Alt 3 (HSPI)",           15, 14, 13, 12},
        {"Alt 4 (MOSI/MISO swap)", SD_CS_PIN, SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN},
        {"Alt 5 (5,14,15,2)",       5, 14, 15, 2},
        {"Alt 6 (4,14,15,2)",       4, 14, 15, 2},
    };

    int testNum = 6;
    bool spiSuccess = false;

    for (int cfg = 0; cfg < 7; cfg++) {
        SPIPinConfig& pins = spiConfigs[cfg];

        Logger.printf("Test %d: %s - CS=%d CLK=%d MOSI=%d MISO=%d\n",
                      testNum++, pins.name, pins.cs, pins.clk, pins.mosi, pins.miso);

        SPI.begin(pins.clk, pins.miso, pins.mosi, pins.cs);
        delay(100);

        if (SD.begin(pins.cs, SPI, 400000)) {
            uint8_t cardType = SD.cardType();
            Logger.printf("   ✅ SUCCESS! Card Type: %d ", cardType);
            switch(cardType) {
                case 0: Logger.println("(NONE)"); break;
                case 1: Logger.println("(MMC)");  break;
                case 2: Logger.println("(SD)");   break;
                case 3: Logger.println("(SDHC)"); break;
                default: Logger.println("(UNKNOWN)"); break;
            }
            if (cardType != 0) {
                Logger.printf("   Card Size: %llu MB\n", SD.cardSize() / (1024 * 1024));
                Logger.printf("   Used: %llu MB\n", SD.usedBytes() / (1024 * 1024));

                File root = SD.open("/");
                if (root && root.isDirectory()) {
                    Logger.println("   Root directory files:");
                    File file = root.openNextFile();
                    int fileCount = 0;
                    while (file && fileCount < 5) {
                        Logger.printf("      - %s (%llu bytes)\n", file.name(), file.size());
                        file = root.openNextFile();
                        fileCount++;
                    }
                    if (fileCount == 0) Logger.println("      (empty)");
                }

                Logger.println();
                Logger.println("   🎯 WORKING CONFIGURATION FOUND!");
                Logger.printf("   Use: CS=%d CLK=%d MOSI=%d MISO=%d\n",
                              pins.cs, pins.clk, pins.mosi, pins.miso);
                spiSuccess = true;
            }
            SD.end();
        } else {
            Logger.println("   ❌ FAILED");
        }
        SPI.end();
        delay(500);

        if (spiSuccess) break;
    }

    if (!spiSuccess) {
        Logger.println();
        Logger.println("⚠️  No SPI pin configuration worked");
        Logger.println("   Trying additional diagnostics...");
        Logger.println();

        Logger.println("Test: SPI bus basic functionality");
        SPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
        pinMode(SD_CS_PIN, OUTPUT);
        digitalWrite(SD_CS_PIN, HIGH);
        delay(10);
        digitalWrite(SD_CS_PIN, LOW);
        SPI.transfer(0xFF);
        digitalWrite(SD_CS_PIN, HIGH);
        Logger.println("   SPI transfer completed (bus functional)");
        SPI.end();
        delay(500);
    }

    // ── AudioKit SD_ACTIVE test ──────────────────────────────────────────
    Logger.println();
    Logger.println("═══════════════════════════════════════════=");
    Logger.println("AUDIOKIT SD_ACTIVE TEST");
    Logger.println("═══════════════════════════════════════════=");
    Logger.println();

    extern AudioBoardStream kit;

    Logger.println("Test: AudioKit with cfg.sd_active = true");
    Logger.println("   Restarting AudioKit...");
    kit.end();
    delay(500);

    auto cfg = kit.defaultConfig(RXTX_MODE);
    cfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    cfg.sd_active = true;

    if (!kit.begin(cfg)) {
        Logger.println("   ❌ AudioKit init failed");
    } else {
        Logger.println("   AudioKit restarted");
        delay(1000);

        bool sdWorks = false;
#if SD_USE_MMC
        if (SD_MMC.cardType() != CARD_NONE) {
            Logger.printf("   ✅ SD_MMC accessible! Card Type: %d\n", SD_MMC.cardType());
            Logger.printf("   Card Size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
            sdWorks = true;
        }
#else
        if (SD.cardType() != CARD_NONE) {
            Logger.printf("   ✅ SD accessible! Card Type: %d\n", SD.cardType());
            Logger.printf("   Card Size: %llu MB\n", SD.cardSize() / (1024 * 1024));
            sdWorks = true;
        }
#endif
        if (!sdWorks) {
            Logger.println("   ❌ SD card not accessible via AudioKit");
        }
    }

    Logger.println();
    Logger.println("   Restoring AudioKit to sd_active=false...");
    kit.end();
    delay(500);
    cfg.sd_active = false;
    kit.begin(cfg);
    delay(500);
    SPI.end();
    delay(500);

    // ── Analysis and recommendations ────────────────────────────────────
    Logger.println();
    Logger.println("════════════════════════════════════════════");
    Logger.println("💡 ANALYSIS & RECOMMENDATIONS");
    Logger.println("════════════════════════════════════════════");
    Logger.println();
    Logger.println("Pin Information:");
    Logger.println("   SD_MMC hardware pins (ESP32 default):");
    Logger.println("      CLK=GPIO14, CMD=GPIO15, D0=GPIO2");
    Logger.println("      D1=GPIO4, D2=GPIO12, D3=GPIO13 (4-bit mode)");
    Logger.println("   SPI software pins (configurable):");
    Logger.printf("      Current config.h: CS=%d CLK=%d MOSI=%d MISO=%d\n",
                  SD_CS_PIN, SD_CLK_PIN, SD_MOSI_PIN, SD_MISO_PIN);
    Logger.println();
    Logger.println("DIP Switch Requirements:");
    Logger.println("   SD_MMC mode: Check ESP32-A1S schematic for switches");
    Logger.println("   SPI mode:    DIP switches 2,3,4 UP, 5 DOWN (typical)");
    Logger.println("   ⚠️  Wrong DIP switches = card not detected");
    Logger.println();
    Logger.println("Card Type Values:");
    Logger.println("   0 = No card detected (or wrong DIP/pins/power)");
    Logger.println("   1 = MMC (rare)");
    Logger.println("   2 = SD");
    Logger.println("   3 = SDHC (most common for >2GB cards)");
    Logger.println();
    Logger.println("Troubleshooting Steps:");
    Logger.println("   1. Verify card is properly seated in slot");
    Logger.println("   2. Check DIP switch settings match chosen mode");
    Logger.println("   3. Measure 3.3V on card socket (power issue?)");
    Logger.println("   4. Try different SD card (some are picky)");
    Logger.println("   5. Reformat card as FAT32 on computer");
    Logger.println("   6. Check board schematic for actual pin connections");
    Logger.println();
    if (spiSuccess) {
        Logger.println("✅ SPI mode working - update config.h with working pins!");
    } else {
        Logger.println("❌ No working configuration found");
        Logger.println("   Next steps:");
        Logger.println("   - Double-check DIP switches");
        Logger.println("   - Consult ESP32-A1S AudioKit schematic");
        Logger.println("   - Try different SD card");
        Logger.println("   - Check for cold solder joints on SD socket");
    }
    Logger.println();
    Logger.printf("Current Build: SD_USE_MMC=%d %s\n",
                  SD_USE_MMC,
                  SD_USE_MMC ? "(Using SD_MMC in production)" : "(Using SPI in production)");
    Logger.println();
    Logger.println("Build Flags:");
    Logger.println("   -DRUN_SD_DEBUG_FIRST  Run this test before all init");
    Logger.println("   -DSD_USE_MMC=1        Force SD_MMC mode");
    Logger.println("   -DSD_USE_MMC=0        Force SPI mode");
    Logger.println("════════════════════════════════════════════");
    Logger.println();
    Logger.println("⚠️  Reboot required to restore normal SD operation");
}

#endif