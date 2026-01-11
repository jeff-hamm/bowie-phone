#pragma once

// https: // raw.githubusercontent.com/GadgetReboot/KS0835F_Phone_SLIC/refs/heads/main/Sketch/KS0835F_SLIC_Demo.ino

// ============================================================================
// DTMF SEQUENCE CONFIGURATION
// ============================================================================

// Maximum digits in a DTMF sequence
#ifndef MAX_SEQUENCE_LENGTH
#define MAX_SEQUENCE_LENGTH 20
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