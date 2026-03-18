# 1 "C:\\Users\\Jumper\\AppData\\Local\\Temp\\tmp0yjidqy3"
#include <Arduino.h>
# 1 "C:/Users/Jumper/Projects/bowie-phone/src/main.ino"
#include <config.h>
#include "dtmf_goertzel.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#if SD_USE_MMC
  #include "AudioTools/Disk/AudioSourceSDMMC.h"
#else
  #include "AudioTools/Disk/AudioSourceSD.h"
#endif
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecAACHelix.h"
#include "AudioTools/AudioCodecs/ContainerM4A.h"
#include "AudioTools/CoreAudio/AudioMetaData/MimeDetector.h"
#include "sequence_processor.h"
#include "special_command_processor.h"
#include "audio_file_manager.h"
#include "audio_key_registry.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "remote_logger.h"
#include "logging.h"
#include "phone_service.h"
#include "extended_audio_player.h"
#include "notifications.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <ESPTelnetStream.h>

ESPTelnetStream telnet;
AudioBoardStream kit(AudioKitEs8388V1);
AudioSource *source = nullptr;
MultiDecoder multi_decoder;
MP3DecoderHelix mp3_decoder;
WAVDecoder wav_decoder;
AACDecoderHelix aac_decoder;
MultiDecoder m4a_inner_decoder;
ContainerM4A m4a_decoder;
ExtendedAudioPlayer& audioPlayer = getExtendedAudioPlayer();


GoertzelStream goertzel;
StreamCopy goertzelCopier(goertzel, kit);



const int KEY_PINS[] = {36, 13, 19, 23, 18, 5};
const int NUM_KEYS = 6;



const int FIRMWARE_UPDATE_KEY = 19;

static bool audioKitInitialized = false;
void setup();
void loop();
#line 62 "C:/Users/Jumper/Projects/bowie-phone/src/main.ino"
void setup()
{
    Serial.begin(115200);
    delay(2000);


    Logger.addLogger(Serial);

    Logger.addLogger(RemoteLogger);


    Logger.printf("\n\n=== Bowie Phone Starting ===\n");
    Logger.printf("🔧 Firmware: %s  Build: %s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);

#ifdef RUN_SD_DEBUG_FIRST



    Logger.println("🔧 RUN_SD_DEBUG_FIRST enabled - running SD diagnostics...");
    Logger.println("   Waiting 3 seconds for serial connection...");
    delay(3000);


    AudioBoardStream testKit(AudioKitEs8388V1);
    auto testCfg = testKit.defaultConfig(RXTX_MODE);
    testCfg.setAudioInfo(AUDIO_INFO_DEFAULT());
    testCfg.sd_active = false;
    if (testKit.begin(testCfg)) {
        Logger.println("   AudioKit initialized for testing");
    }

    performSDCardDebug();

    Logger.println();
    Logger.println("═══════════════════════════════════════════");
    Logger.println("SD DEBUG COMPLETE - Device will halt");
    Logger.println("Review results above, then:");
    Logger.println("  1. Remove -DRUN_SD_DEBUG_FIRST flag");
    Logger.println("  2. Update config.h with working pins");
    Logger.println("  3. Rebuild and flash");
    Logger.println("═══════════════════════════════════════════");
    while (true) { delay(1000); }
#endif


    initNotifications();



    pinMode(FIRMWARE_UPDATE_KEY, INPUT_PULLUP);
    delay(50);
    if (digitalRead(FIRMWARE_UPDATE_KEY) == LOW) {
        Logger.println("🔧 KEY3 (GPIO19) held at boot - entering firmware update mode...");
        enterFirmwareUpdateMode();

    }



    AudioToolsLogger.begin(Logger, AUDIOTOOLS_LOG_LEVEL);



    Logger.println("🔧 Initializing AudioKit (RXTX_MODE)...");
    auto cfg = kit.defaultConfig(RXTX_MODE);
    cfg.setAudioInfo(AUDIO_INFO_DEFAULT());

#ifdef AUDIO_INPUT_DEVICE
    cfg.input_device = AUDIO_INPUT_DEVICE;
#else
    cfg.input_device = ADC_INPUT_ALL;
#endif

    cfg.sd_active = false;

    if (!kit.begin(cfg))
    {
        Logger.println("❌ Failed to initialize AudioKit");
        audioKitInitialized = false;
    }
    else
    {
        Logger.println("✅ AudioKit initialized successfully");
        audioKitInitialized = true;



        kit.setInputVolume(AUDIOKIT_INPUT_VOLUME);
        Logger.printf("🔊 Input volume set to %d%%\n", AUDIOKIT_INPUT_VOLUME);
    }



    source = initializeAudioFileManager();




    audioPlayer.setRegistry(&getAudioKeyRegistry());


    audioPlayer.addDecoder(mp3_decoder, "audio/mpeg");


    audioPlayer.addDecoder(wav_decoder, "audio/wav");
    audioPlayer.addDecoder(wav_decoder, "audio/vnd.wave");
    audioPlayer.addDecoder(wav_decoder, "audio/wave");



    m4a_inner_decoder.addDecoder(aac_decoder, "audio/aac");
    m4a_decoder.setDecoder(m4a_inner_decoder);
    audioPlayer.addDecoder(m4a_decoder, "audio/m4a", MimeDetector::checkM4A);
    audioPlayer.begin(kit, source != nullptr);



    initGoertzelDecoder(goertzel, goertzelCopier);



    startGoertzelTask(goertzelCopier);


    Logger.println("🔧 Starting WiFi initialization...");


    initWiFi([]() {




        telnet.onConnect([](String ip) {
            Logger.printf("📡 Telnet client connected from: %s\n", ip.c_str());
            Logger.printf("🔧 Firmware: %s  Build: %s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);
            Logger.addLogger(telnet);
        });
        telnet.onConnectionAttempt([](String ip) {
            Logger.printf("📡 Telnet connection attempt from: %s\n", ip.c_str());
        });
        telnet.onReconnect([](String ip) {
            Logger.printf("📡 Telnet client reconnected from: %s\n", ip.c_str());
        });
        telnet.onDisconnect([](String ip) {
            Logger.printf("📡 Telnet client disconnected from: %s\n", ip.c_str());
            Logger.removeLogger(telnet);
        });

        if (telnet.begin(23)) {
            Logger.println("✅ Telnet server started on port 23");
        } else {
            Logger.println("❌ Failed to start telnet server");
        }



        Logger.println("🌐 Downloading audio catalog...");
        if (downloadAudio()) {
            Logger.println("✅ Audio catalog downloaded successfully");
        } else {
            Logger.println("⚠️ Audio catalog download failed - will retry later");
        }
    });


    setTailscaleSkipCallback([]() -> bool {
        return Phone.isOffHook();
    });


    initializeSpecialCommands();


    Phone.begin();
    Phone.setHookCallback([](bool isOffHook) {
        if (isOffHook) {

            Logger.println("📞 Phone picked up (OFF HOOK)");

            if (checkAndExecuteOffHookCapture()) {


                return;
            }
            Logger.println("⚡ Playing Dial Tone");
            audioPlayer.playAudioKey("dialtone");
        } else {

            Logger.println("📞 Phone hung up (ON HOOK)");
            audioPlayer.stop();
            resetDTMFSequence();
        }
    });

    Logger.println("✅ Bowie Phone Ready!");
    Logger.println("🔧 Serial Debug Mode ACTIVE - type 'help' for commands");

    if (Phone.isOffHook())
    {
        Logger.println("📞 Phone is off hook at boot - playing dial tone");
        audioPlayer.playAudioKey("dialtone");
    }
}

void loop()
{

    static unsigned long lastMaintenanceCheck = 0;
    Phone.loop();
    if (Phone.isOffHook())
    {

        static bool offHookWarningPlayed = false;
        static unsigned long warningPlayedTime = 0;
        unsigned long now = millis();
        unsigned long lastActivity = max(getLastDigitTime(), audioPlayer.getLastActive());


        bool audioInProgress = audioPlayer.isActive() && !audioPlayer.isAudioKeyPlaying("dialtone");
        if (!offHookWarningPlayed && !audioInProgress && lastActivity > 0 && (now - lastActivity) >= OFF_HOOK_TIMEOUT_MS) {
            Logger.println("⚠️ Off-hook timeout - playing warning tone");
            audioPlayer.playAudioKey("off_hook");
            offHookWarningPlayed = true;
            warningPlayedTime = now;
        }


        if (offHookWarningPlayed) {
            if (getLastDigitTime() > warningPlayedTime) {
                offHookWarningPlayed = false;
            } else if (!audioPlayer.isActive() && (now - warningPlayedTime) > 2000) {
                offHookWarningPlayed = false;
            }
        }


        if (audioPlayer.isActive())
        {
            audioPlayer.copy();
            bool playingDialtone = audioPlayer.isAudioKeyPlaying("dialtone");


            setGoertzelMuted(!playingDialtone);
            if (!playingDialtone) {
                return;
            }
        } else {

            setGoertzelMuted(isSequenceLocked());
        }


        char goertzelKey = getGoertzelKey();
        if (goertzelKey != 0) {
            addDtmfDigit(goertzelKey);
        }


        if (isSequenceReady()) {
            readDTMFSequence(true);
        }

    }

    auto limit = isReadingSequence() ? 10 : 100;
    unsigned long now = millis();


    if (now - lastMaintenanceCheck >= limit) {
        lastMaintenanceCheck = now;


        telnet.loop();


        handleNetworkLoop();

        audioMaintenanceLoop();

        processDebugInput(Serial);
        processDebugInput(telnet);

    }
}