/**
 * Minimal SD Card Test for ESP32-A1S Audio Kit V2.2
 * 
 * Tests both SPI and SD_MMC modes with detailed diagnostics
 * Press any key in Serial Monitor to retry
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <SD_MMC.h>

// SD Card pins for AI Thinker AudioKit
#define SD_CS   13
#define SD_CLK  14
#define SD_MOSI 15
#define SD_MISO 2

void printCardInfo(const char* mode, uint64_t cardSize, const char* cardType) {
    Serial.printf("✅ %s SUCCESS!\n", mode);
    Serial.printf("   Card Type: %s\n", cardType);
    Serial.printf("   Card Size: %llu MB\n", cardSize / (1024 * 1024));
    
    // Try to list root directory
    File root = (strcmp(mode, "SD_MMC") == 0) ? SD_MMC.open("/") : SD.open("/");
    if (root) {
        Serial.println("   Root directory contents:");
        File file = root.openNextFile();
        int count = 0;
        while (file && count < 10) {
            Serial.printf("   - %s (%d bytes)\n", file.name(), file.size());
            file = root.openNextFile();
            count++;
        }
        if (count == 0) {
            Serial.println("   (empty or no files found)");
        }
        root.close();
    }
}

const char* getCardType(uint8_t type) {
    switch(type) {
        case CARD_NONE: return "NONE";
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SD";
        case CARD_SDHC: return "SDHC";
        default: return "UNKNOWN";
    }
}

bool testSPI() {
    Serial.println("\n--- Testing SPI Mode ---");
    Serial.printf("Pins: CS=%d, CLK=%d, MOSI=%d, MISO=%d\n", SD_CS, SD_CLK, SD_MOSI, SD_MISO);
    
    // Initialize SPI with custom pins
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    
    // Set CS pin high initially
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(100);
    
    if (SD.begin(SD_CS)) {
        uint8_t cardType = SD.cardType();
        if (cardType != CARD_NONE) {
            printCardInfo("SPI", SD.cardSize(), getCardType(cardType));
            SD.end();
            return true;
        }
        Serial.println("❌ SPI: Card detected but type is NONE");
    } else {
        Serial.println("❌ SPI: SD.begin() failed");
    }
    
    SD.end();
    SPI.end();
    return false;
}

bool testSDMMC_1bit() {
    Serial.println("\n--- Testing SD_MMC 1-bit Mode ---");
    Serial.println("Pins: CMD=15, CLK=14, D0=2");
    Serial.println("Required switch: #3 (CMD/IO15) must be ON");
    
    if (SD_MMC.begin("/sdcard", true)) {  // true = 1-bit mode
        uint8_t cardType = SD_MMC.cardType();
        if (cardType != CARD_NONE) {
            printCardInfo("SD_MMC", SD_MMC.cardSize(), getCardType(cardType));
            SD_MMC.end();
            return true;
        }
        Serial.println("❌ SD_MMC 1-bit: Card detected but type is NONE");
    } else {
        Serial.println("❌ SD_MMC 1-bit: begin() failed");
    }
    
    SD_MMC.end();
    return false;
}

bool testSDMMC_4bit() {
    Serial.println("\n--- Testing SD_MMC 4-bit Mode ---");
    Serial.println("Pins: CMD=15, CLK=14, D0=2, D1=4, D2=12, D3=13");
    Serial.println("Required switches: #2 (Data3) and #3 (CMD) must be ON");
    
    if (SD_MMC.begin("/sdcard", false)) {  // false = 4-bit mode
        uint8_t cardType = SD_MMC.cardType();
        if (cardType != CARD_NONE) {
            printCardInfo("SD_MMC 4-bit", SD_MMC.cardSize(), getCardType(cardType));
            SD_MMC.end();
            return true;
        }
        Serial.println("❌ SD_MMC 4-bit: Card detected but type is NONE");
    } else {
        Serial.println("❌ SD_MMC 4-bit: begin() failed");
    }
    
    SD_MMC.end();
    return false;
}

void printGPIOState() {
    Serial.println("\n--- GPIO State (before SD init) ---");
    int pins[] = {2, 4, 12, 13, 14, 15};
    const char* names[] = {"D0/MISO", "D1", "D2", "D3/CS", "CLK", "CMD/MOSI"};
    
    for (int i = 0; i < 6; i++) {
        pinMode(pins[i], INPUT);
        int state = digitalRead(pins[i]);
        Serial.printf("GPIO%02d (%s): %s\n", pins[i], names[i], state ? "HIGH" : "LOW");
    }
}

void printSwitchGuide() {
    Serial.println("\n========================================");
    Serial.println("DIP SWITCH CONFIGURATION GUIDE");
    Serial.println("========================================");
    Serial.println("For SD_MMC 1-bit mode (recommended):");
    Serial.println("  Switch 1: OFF (or don't care)");
    Serial.println("  Switch 2: OFF");
    Serial.println("  Switch 3: ON  <-- CRITICAL");
    Serial.println("  Switch 4: OFF");
    Serial.println("  Switch 5: OFF");
    Serial.println("");
    Serial.println("For SPI mode:");
    Serial.println("  Switch 1: OFF");
    Serial.println("  Switch 2: ON");
    Serial.println("  Switch 3: ON");
    Serial.println("  Switch 4: OFF");
    Serial.println("  Switch 5: OFF");
    Serial.println("========================================\n");
}

void runAllTests() {
    Serial.println("\n========================================");
    Serial.println("   SD CARD DIAGNOSTIC TEST");
    Serial.println("   ESP32-A1S Audio Kit V2.2");
    Serial.println("========================================");
    
    printGPIOState();
    
    bool spiOk = testSPI();
    delay(500);
    
    bool sdmmc1Ok = testSDMMC_1bit();
    delay(500);
    
    bool sdmmc4Ok = testSDMMC_4bit();
    
    Serial.println("\n========================================");
    Serial.println("   TEST RESULTS SUMMARY");
    Serial.println("========================================");
    Serial.printf("SPI Mode:        %s\n", spiOk ? "✅ PASS" : "❌ FAIL");
    Serial.printf("SD_MMC 1-bit:    %s\n", sdmmc1Ok ? "✅ PASS" : "❌ FAIL");
    Serial.printf("SD_MMC 4-bit:    %s\n", sdmmc4Ok ? "✅ PASS" : "❌ FAIL");
    Serial.println("========================================");
    
    if (!spiOk && !sdmmc1Ok && !sdmmc4Ok) {
        Serial.println("\n⚠️  ALL MODES FAILED!");
        Serial.println("Check:");
        Serial.println("1. Is SD card inserted?");
        Serial.println("2. Is SD card formatted (FAT32)?");
        Serial.println("3. Check DIP switch positions:");
        printSwitchGuide();
        Serial.println("4. Try a different SD card");
        Serial.println("5. Check if card works in computer");
    }
    
    Serial.println("\nPress ENTER in Serial Monitor to re-run tests...");
}

void setup() {
    Serial.begin(115200);
    delay(2000);  // Wait for serial
    
    Serial.println("\n\n");
    runAllTests();
}

void loop() {
    if (Serial.available()) {
        while (Serial.available()) Serial.read();  // Clear buffer
        runAllTests();
    }
    delay(100);
}
