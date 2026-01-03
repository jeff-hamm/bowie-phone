# 1 "C:\\Users\\Jumper\\AppData\\Local\\Temp\\tmpykmm5k3t"
#include <Arduino.h>
# 1 "C:/Users/Jumper/Projects/bowie-phone/src/main.ino"
#include <config.h>
#include "dtmf_decoder.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioLibs/AudioRealFFT.h"
#include "AudioTools/Disk/AudioSourceSD.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#define DEBUG 
#include "sequence_processor.h"
#include "special_command_processor.h"
#include "audio_file_manager.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "logging.h"
#include "phone_service.h"
#include "audio_player.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>

AudioBoardStream kit(AudioKitEs8388V1);
AudioSourceSD *source = nullptr;
AudioRealFFT fft;
StreamCopy copier(fft, kit);
MultiDecoder multi_decoder;
MP3DecoderHelix mp3_decoder;
WAVDecoder wav_decoder;
int channels = 2;
int samples_per_second = 44100;
int bits_per_sample = 16;


#ifdef DEBUG
static char serialDebugBuffer[64];
static int serialDebugPos = 0;
void processSerialDebugInput();
void setup();
void loop();
#line 43 "C:/Users/Jumper/Projects/bowie-phone/src/main.ino"
void processSerialDebugInput() {
    while (Serial.available()) {
        char c = Serial.read();


        if (c == '\n' || c == '\r') {
            if (serialDebugPos > 0) {
                serialDebugBuffer[serialDebugPos] = '\0';
                String cmd = String(serialDebugBuffer);
                cmd.trim();

                if (cmd.equalsIgnoreCase("hook")) {

                    bool newState = !Phone.isOffHook();
                    Phone.setOffHook(newState);
                    Logger.printf("🔧 [DEBUG] Hook toggled to: %s\n", newState ? "OFF HOOK" : "ON HOOK");
                }
                else if (cmd.equalsIgnoreCase("help") || cmd.equals("?")) {
                    Logger.println("🔧 [DEBUG] Serial Debug Commands:");
                    Logger.println("   hook         - Toggle hook state");
                    Logger.println("   level <0-2>  - Set log level (0=quiet, 1=normal, 2=debug)");
                    Logger.println("   <digits>     - Simulate DTMF sequence (e.g., 6969, 888)");
                    Logger.println("   <freq>hz     - FFT test tone (e.g., 697hz)");
                    Logger.println("   state        - Show current state");
                    Logger.println("   help         - Show this help");
                }
                else if (cmd.equalsIgnoreCase("state")) {
                    Logger.printf("🔧 [DEBUG] State: Hook=%s, Audio=%s\n",
                        Phone.isOffHook() ? "OFF_HOOK" : "ON_HOOK",
                        isAudioActive() ? "PLAYING" : "IDLE");
                }
                else if (cmd.startsWith("level ")) {

                    int level = cmd.substring(6).toInt();
                    if (level >= 0 && level <= 2) {
                        Logger.setLogLevel((LogLevel)level);
                        Logger.printf("🔧 [DEBUG] Log level set to: %d\n", level);
                    }
                }
                else if (cmd.endsWith("hz") || cmd.endsWith("Hz") || cmd.endsWith("HZ")) {

                    String freqStr = cmd.substring(0, cmd.length() - 2);
                    int freq = freqStr.toInt();
                    if (freq > 0 && freq < 20000) {
                        Logger.printf("🔧 [DEBUG] FFT test tone: %d Hz (not yet implemented)\n", freq);

                    }
                }
                else {

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
                            simulateDTMFDigit(cmd.charAt(i));
                        }
                    } else if (cmd.length() > 0) {
                        Logger.printf("🔧 [DEBUG] Unknown command: %s\n", cmd.c_str());
                        Logger.println("🔧 [DEBUG] Available commands: hook, level <0-2>, <digits>, <freq>hz");
                    }
                }

                serialDebugPos = 0;
            }
        }
        else if (serialDebugPos < (int)sizeof(serialDebugBuffer) - 1) {
            serialDebugBuffer[serialDebugPos++] = c;
        }
    }
}
#endif





#define SD_CS 13
#define SD_CLK 14
#define SD_MOSI 15
#define SD_MISO 2

const char *startFilePath="/audio";
static bool audioKitInitialized = false;




void setup()
{
    Serial.begin(115200);
    delay(2000);


    Logger.addLogger(Serial);

    Logger.printf("\n\n=== Bowie Phone Starting ===\n");
    AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);



    Logger.println("🔧 Initializing AudioKit (RXTX_MODE)...");
    auto cfg = kit.defaultConfig(RXTX_MODE);
    cfg.channels = channels;
    cfg.sample_rate = samples_per_second;
    cfg.bits_per_sample = bits_per_sample;

#ifdef AUDIO_INPUT_DEVICE
    cfg.input_device = AUDIO_INPUT_DEVICE;
#else
    cfg.input_device = ADC_INPUT_ALL;
#endif

    cfg.sd_active = false;

    if (!kit.begin(cfg))
    {
        Logger.println("❌ Failed to initialize AudioKit");
    }
    else
    {
        Logger.println("✅ AudioKit initialized successfully");



        kit.setInputVolume(100);
        Logger.println("🔊 Input volume set to 100%");


        kit.setVolume(100);
        Logger.println("🔊 Output volume set to 100%");
    }






    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);

    bool sdCardAvailable = false;
    for (int attempt = 1; attempt <= 3 && !sdCardAvailable; attempt++) {
        Logger.printf("🔧 SD SPI initialization attempt %d/3...\n", attempt);
        delay(attempt * 300);

        if (SD.begin(SD_CS, SPI)) {
            uint8_t cardType = SD.cardType();
            if (cardType != CARD_NONE) {
                uint64_t cardSize = SD.cardSize() / (1024 * 1024);
                Logger.printf("✅ SD card initialized (SPI mode, %lluMB)\n", cardSize);
                sdCardAvailable = true;
            } else {
                Logger.println("❌ No SD card detected");
            }
        } else {
            Logger.println("❌ SD.begin() failed");
        }
    }

    if (!sdCardAvailable) {
        Logger.println("⚠️ SD initialization failed - continuing without SD card");
    } else {


        source = new AudioSourceSD(startFilePath, "na", SD_CS, SPI);
        Logger.println("✅ AudioSourceSD created");
   }



    initializeAudioFileManager(SD_CS, false, sdCardAvailable);


    multi_decoder.addDecoder(mp3_decoder, "audio/mpeg");
    multi_decoder.addDecoder(wav_decoder, "audio/wav");

#if FORCE_URL_STREAMING

    Logger.println("🌐 FORCE_URL_STREAMING enabled - using URL streaming mode");
    initAudioPlayerURLMode(kit, multi_decoder);
    Logger.println("✅ Audio player initialized (URL streaming mode - forced)");
#else
    if (source != nullptr) {
        initAudioPlayer(*source, kit, multi_decoder);

        Logger.println("✅ Audio player initialized (SD card mode)");
    } else {

        Logger.println("🌐 SD card not available - using URL streaming mode");
        initAudioPlayerURLMode(kit, multi_decoder);
        Logger.println("✅ Audio player initialized (URL streaming mode)");
    }
#endif


    auto tcfg = fft.defaultConfig();
    tcfg.length = 8192;
    tcfg.channels = channels;
    tcfg.sample_rate = samples_per_second;
    tcfg.bits_per_sample = bits_per_sample;
    tcfg.callback = &fftResult;
    fft.begin(tcfg);


    copier.resize(AUDIO_COPY_BUFFER_SIZE);
    Logger.printf("🎤 Copier buffer resized to %d bytes\n", AUDIO_COPY_BUFFER_SIZE);

    Logger.println("🎤 Audio system ready!");


    Logger.println("🔧 Starting WiFi initialization...");

    initWiFi([]() {


        Logger.println("🌐 Downloading audio catalog before VPN...");
        if (downloadAudio()) {
            Logger.println("✅ Audio catalog downloaded successfully");
        } else {
            Logger.println("⚠️ Audio catalog download failed - will retry later");
        }


        Logger.println("🔐 WiFi connected - initializing Tailscale...");
        initTailscaleFromConfig();
    });


    setTailscaleSkipCallback([]() -> bool {
        return Phone.isOffHook();
    });


    initOTA();


    initializeSpecialCommands();


    Phone.begin();
    Phone.setHookCallback([](bool isOffHook) {
        if (isOffHook) {

            Logger.println("⚡ Event: Phone Off Hook - Playing Dial Tone");
            playAudioBySequence("dialtone");
        } else {

            Logger.println("⚡ Event: Phone On Hook");
            stopAudio();
            resetDTMFSequence();
        }
    });

    Logger.println("✅ Bowie Phone Ready!");

#ifdef DEBUG
    Logger.println("🔧 Serial Debug Mode ACTIVE - type 'help' for commands");
#endif


    if (Phone.isOffHook())
    {
        Logger.println("📞 Phone is off hook at boot - playing dial tone");
        playAudioBySequence("dialtone");
    }
}

void loop()
{

    handleWiFiLoop();


    handleTailscaleLoop();

#ifdef DEBUG

    processSerialDebugInput();
#endif



    static bool audioDownloadComplete = false;
    static unsigned long lastAudioDownloadAttempt = 0;
    const unsigned long audioDownloadRetryMs = 60000;



    if (!audioDownloadComplete && getAudioKeyCount() == 0 &&
        WiFi.status() == WL_CONNECTED && !isTailscaleConnected())
    {
        unsigned long now = millis();
        if (lastAudioDownloadAttempt == 0 || (now - lastAudioDownloadAttempt) > audioDownloadRetryMs)
        {
            lastAudioDownloadAttempt = now;
            if (downloadAudio())
            {
                audioDownloadComplete = true;
            }
        }
    }


    if (!audioDownloadComplete && getAudioKeyCount() > 0) {
        audioDownloadComplete = true;
    }




    Phone.loop();
# 369 "C:/Users/Jumper/Projects/bowie-phone/src/main.ino"
    static unsigned long lastDownloadCheck = 0;
    if (millis() - lastDownloadCheck > 1000)
    {
        processAudioDownloadQueue();
        lastDownloadCheck = millis();
    }


    if (!Phone.isOffHook())
    {
        return;
    }


    size_t bytesCopied = copier.copy();
    if (bytesCopied > 0) {
        Logger.debugf("🎤 Copied %u bytes to FFT\n", bytesCopied);
    }


    if (isAudioActive())
    {
        processAudio();
    }


    const char* audioPath = readDTMFSequence();
    if (audioPath)
    {
        playAudioPath(audioPath);
    }
}