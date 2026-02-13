#pragma once

// https: // raw.githubusercontent.com/GadgetReboot/KS0835F_Phone_SLIC/refs/heads/main/Sketch/KS0835F_SLIC_Demo.ino

// ============================================================================
// DTMF SEQUENCE CONFIGURATION
// ============================================================================

// Maximum digits in a DTMF sequence
#ifndef MAX_SEQUENCE_LENGTH
#define MAX_SEQUENCE_LENGTH 20
#endif

// Off-hook timeout - play warning tone after this many milliseconds of inactivity
#ifndef OFF_HOOK_TIMEOUT_MS
#define OFF_HOOK_TIMEOUT_MS 30000  // 30 seconds
#endif

// ============================================================================
// SD CARD CONFIGURATION
// ============================================================================

// SD Card SPI pins for ESP32-A1S AudioKit (only used when SD_USE_MMC=0)
// Working DIP switch config for SPI mode: 2,3,4 UP, 5 DOWN
// For SD_MMC (1-bit) mode: different DIP switch config, set SD_USE_MMC=1
#ifndef SD_CS_PIN
#define SD_CS_PIN   13
#endif
#ifndef SD_CLK_PIN
#define SD_CLK_PIN  14
#endif
#ifndef SD_MOSI_PIN
#define SD_MOSI_PIN 15
#endif
#ifndef SD_MISO_PIN
#define SD_MISO_PIN 2
#endif

// Set to 1 to use SD_MMC interface instead of SPI SD
#ifndef SD_USE_MMC
#define SD_USE_MMC 0
#endif

// Base path for audio files on SD card
#ifndef SD_AUDIO_PATH
#define SD_AUDIO_PATH "/audio"
#endif

// ============================================================================
// AUDIO CONFIGURATION
// ============================================================================

// Audio format settings (must match AudioKit and FFT configuration)
#ifndef AUDIO_CHANNELS
#define AUDIO_CHANNELS 1
#endif

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 44100
#endif

#ifndef AUDIO_BITS_PER_SAMPLE
#define AUDIO_BITS_PER_SAMPLE 16
#endif
#ifndef DEFAULT_AUDIO_VOLUME
#define DEFAULT_AUDIO_VOLUME 1.0f
#endif

// Default file extension for audio files when not specified
#ifndef DEFAULT_EXTENSION
#define DEFAULT_EXTENSION "wav"
#endif

// Input volume/gain for DTMF detection (0-100 percentage)
#ifndef AUDIOKIT_INPUT_VOLUME
#define AUDIOKIT_INPUT_VOLUME 100
#endif

// Helper macro to create AudioInfo with default settings
#define AUDIO_INFO_DEFAULT() ([]{ \
    AudioInfo info; \
    info.sample_rate = AUDIO_SAMPLE_RATE; \
    info.channels = AUDIO_CHANNELS; \
    info.bits_per_sample = AUDIO_BITS_PER_SAMPLE; \
    return info; \
}())

// Buffer size for audio copy operations (affects DTMF sampling)
#ifndef AUDIO_COPY_BUFFER_SIZE
#define AUDIO_COPY_BUFFER_SIZE 4096
#endif

// Interval for checking DTMF input during audio playback (milliseconds)
#ifndef DTMF_CHECK_DURING_PLAYBACK_INTERVAL_MS
#define DTMF_CHECK_DURING_PLAYBACK_INTERVAL_MS 200
#endif

// AudioTools library log level (Error=1, Warning=2, Info=3, Debug=4)
#ifndef AUDIOTOOLS_LOG_LEVEL
#define AUDIOTOOLS_LOG_LEVEL AudioToolsLogLevel::Warning
#endif

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================

// Primary DNS server (default: Google DNS)
#ifndef DNS_PRIMARY_1
#define DNS_PRIMARY_1 8
#endif
#ifndef DNS_PRIMARY_2
#define DNS_PRIMARY_2 8
#endif
#ifndef DNS_PRIMARY_3
#define DNS_PRIMARY_3 8
#endif
#ifndef DNS_PRIMARY_4
#define DNS_PRIMARY_4 8
#endif

// Secondary DNS server (default: Cloudflare DNS)
#ifndef DNS_SECONDARY_1
#define DNS_SECONDARY_1 1
#endif
#ifndef DNS_SECONDARY_2
#define DNS_SECONDARY_2 1
#endif
#ifndef DNS_SECONDARY_3
#define DNS_SECONDARY_3 1
#endif
#ifndef DNS_SECONDARY_4
#define DNS_SECONDARY_4 1
#endif

// Convenience macros for creating IPAddress objects
#define DNS_PRIMARY_IPADDRESS IPAddress(DNS_PRIMARY_1, DNS_PRIMARY_2, DNS_PRIMARY_3, DNS_PRIMARY_4)
#define DNS_SECONDARY_IPADDRESS IPAddress(DNS_SECONDARY_1, DNS_SECONDARY_2, DNS_SECONDARY_3, DNS_SECONDARY_4)

// Default WiFi credentials (used when no saved credentials exist)
#ifndef DEFAULT_SSID
#define DEFAULT_SSID "House Atreides"
#endif
#ifndef DEFAULT_PASSWORD
#define DEFAULT_PASSWORD "desertpower"
#endif

// ============================================================================
// PHONE HARDWARE CONFIGURATION
// ============================================================================

// Enable ringing functionality (requires SLIC hardware with F/R and RM pins)
//#define CAN_RING 1

#ifdef CAN_RING
#define F_R 22
#define RM 18
#endif

#define SHK 21
#define RING_CYCLE_MS 750
//#define ASSUME_HOOK 1
// PINOUT
// Row1:
// Row2: 21, 22, 18, 23
// 5, for sure, works as input. Maybe not for SD?

// 21 SHK works

// ============================================================================
// STATUS LED CONFIGURATION
// ============================================================================
// LED pins on AudioKit boards (active LOW on most boards)
// GREEN_LED_GPIO: 22 - used for WiFi connected status
// RED_LED_GPIO:   19 - used for WireGuard/Tailscale connected status
// Note: GPIO 22 conflicts with CAN_RING (F_R pin) - LEDs disabled when ringing enabled
#ifndef GREEN_LED_GPIO
#define GREEN_LED_GPIO 22
#endif
#ifndef RED_LED_GPIO
#define RED_LED_GPIO 19
#endif
// LEDs are active LOW on AudioKit boards
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif