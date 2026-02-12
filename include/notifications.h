#pragma once

#include <Arduino.h>

// ============================================================================
// NOTIFICATION SYSTEM
// ============================================================================
// Abstraction layer for status notifications via LEDs
// Maps notification types to hardware pins and handles different payload types

// Notification types
enum class NotificationType {
    WiFiConnected,      // Boolean payload: true=connected, false=disconnected
    TailscaleConnected, // Boolean payload: true=connected, false=disconnected
    DTMFDetected,       // Integer payload: key pressed (0-9, *, #) - pulses LED
    ReadingSequence,    // Boolean payload: true=reading, false=done
};

// Pulse timing configuration (milliseconds)
struct PulseConfig {
    uint16_t onDuration;   // How long LED stays on during pulse
    uint16_t offDuration;  // How long LED stays off between pulses
    uint16_t endDelay;     // Delay after pulse sequence before restoring state
};

// Default pulse configuration
#ifndef PULSE_ON_DURATION_MS
#define PULSE_ON_DURATION_MS 100
#endif

#ifndef PULSE_OFF_DURATION_MS
#define PULSE_OFF_DURATION_MS 100
#endif

#ifndef PULSE_END_DELAY_MS
#define PULSE_END_DELAY_MS 50
#endif

/**
 * Initialize the notification system
 * Sets up LED pins and initial states
 */
void initNotifications();

/**
 * Send a notification with a boolean payload
 * @param type The notification type
 * @param value true to turn LED on, false to turn off
 */
void notify(NotificationType type, bool value);

/**
 * Send a notification with an integer payload
 * Pulses the LED the specified number of times
 * @param type The notification type
 * @param value Number of pulses (typically DTMF key value)
 */
void notify(NotificationType type, int value);

/**
 * Configure pulse timing
 * @param config Pulse timing configuration
 */
void setPulseConfig(const PulseConfig& config);

/**
 * Get current pulse configuration
 * @return Current pulse timing configuration
 */
PulseConfig getPulseConfig();

/**
 * Check if notifications are enabled
 * @return true if LED notifications are available
 */
bool notificationsEnabled();

