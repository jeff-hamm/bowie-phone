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

// Buffer size for audio copy operations (affects DTMF sampling)
#ifndef AUDIO_COPY_BUFFER_SIZE
#define AUDIO_COPY_BUFFER_SIZE 4096
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