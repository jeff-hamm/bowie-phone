#include "notifications.h"
#include "config.h"
#include "logging.h"

// ============================================================================
// NOTIFICATION SYSTEM IMPLEMENTATION
// ============================================================================

// LED state tracking
static bool greenLedState = false;
static bool redLedState = false;
static bool initialized = false;

// Pulse configuration
static PulseConfig pulseConfig = {
    PULSE_ON_DURATION_MS,
    PULSE_OFF_DURATION_MS,
    PULSE_END_DELAY_MS
};

// Pin mapping for notification types
static int8_t getNotificationPin(NotificationType type) {
    switch (type) {
        case NotificationType::WiFiConnected:
            return GREEN_LED_GPIO;
        case NotificationType::TailscaleConnected:
            return RED_LED_GPIO;
        case NotificationType::DTMFDetected:
            return GREEN_LED_GPIO;
        case NotificationType::ReadingSequence:
            return RED_LED_GPIO;
        default:
            return -1;
    }
}

// Get the state variable for a pin
static bool* getLedState(int8_t pin) {
    if (pin == GREEN_LED_GPIO) {
        return &greenLedState;
    } else if (pin == RED_LED_GPIO) {
        return &redLedState;
    }
    return nullptr;
}

// Low-level LED control
static void setLedRaw(int8_t pin, bool on) {
    if (pin < 0) return;
    
#ifdef CAN_RING
    // GPIO 22 conflicts with ringing - skip if it's the green LED
    if (pin == GREEN_LED_GPIO && pin == 22) {
        return;
    }
#endif

#if LED_ACTIVE_LOW
    digitalWrite(pin, on ? LOW : HIGH);
#else
    digitalWrite(pin, on ? HIGH : LOW);
#endif
}

// Set LED state and track it
static void setLed(int8_t pin, bool on) {
    if (!initialized || pin < 0) return;
    
    bool* state = getLedState(pin);
    if (state) {
        *state = on;
    }
    setLedRaw(pin, on);
}

// Pulse LED a specified number of times, then restore original state
static void pulseLed(int8_t pin, int pulseCount) {
    if (!initialized || pin < 0 || pulseCount <= 0) return;
    
    // Save current state
    bool* statePtr = getLedState(pin);
    bool savedState = statePtr ? *statePtr : false;
    
    // Turn off before pulsing
    setLedRaw(pin, false);
    delay(pulseConfig.offDuration);
    
    // Pulse the specified number of times
    for (int i = 0; i < pulseCount; i++) {
        setLedRaw(pin, true);
        delay(pulseConfig.onDuration);
        setLedRaw(pin, false);
        if (i < pulseCount - 1) {
            delay(pulseConfig.offDuration);
        }
    }
    
    // End delay before restoring
    delay(pulseConfig.endDelay);
    
    // Restore original state
    setLedRaw(pin, savedState);
}

// ============================================================================
// PUBLIC API
// ============================================================================

void initNotifications() {
#ifdef CAN_RING
    // GPIO 22 conflicts with ringing hardware
    Logger.println("⚠️ Notifications: Green LED disabled (CAN_RING enabled, GPIO 22 conflict)");
    
    // Only initialize red LED
    if (RED_LED_GPIO >= 0 && RED_LED_GPIO != 22) {
        pinMode(RED_LED_GPIO, OUTPUT);
        setLedRaw(RED_LED_GPIO, false);
        Logger.printf("💡 Notifications initialized (Red only: GPIO%d)\n", RED_LED_GPIO);
    }
#else
    // Initialize both LED pins
    if (GREEN_LED_GPIO >= 0) {
        pinMode(GREEN_LED_GPIO, OUTPUT);
        setLedRaw(GREEN_LED_GPIO, false);
    }
    if (RED_LED_GPIO >= 0) {
        pinMode(RED_LED_GPIO, OUTPUT);
        setLedRaw(RED_LED_GPIO, false);
    }
    
    Logger.printf("💡 Notifications initialized (Green: GPIO%d, Red: GPIO%d)\n", 
                  GREEN_LED_GPIO, RED_LED_GPIO);
#endif

    initialized = true;
}

void notify(NotificationType type, bool value) {
    if (!initialized) return;
    
    int8_t pin = getNotificationPin(type);
    if (pin < 0) return;
    
    // Log the notification
    const char* typeName = "";
    switch (type) {
        case NotificationType::WiFiConnected:
            typeName = "WiFi";
            break;
        case NotificationType::TailscaleConnected:
            typeName = "Tailscale";
            break;
        case NotificationType::ReadingSequence:
            typeName = "ReadingSequence";
            break;
        default:
            typeName = "Unknown";
            break;
    }
    
    Logger.printf("💡 Notify: %s %s (GPIO%d)\n", typeName, value ? "ON" : "OFF", pin);
    setLed(pin, value);
}

void notify(NotificationType type, int value) {
    if (!initialized) return;
    
    int8_t pin = getNotificationPin(type);
    if (pin < 0) return;
    
    // For DTMF, value is the key pressed
    // Map DTMF keys to pulse counts:
    // 0 = 10 pulses, 1-9 = that many pulses, * = 11, # = 12
    int pulseCount = value;
    if (value == 0) {
        pulseCount = 10;  // 0 key = 10 pulses (like rotary dial)
    } else if (value == '*') {
        pulseCount = 11;
    } else if (value == '#') {
        pulseCount = 12;
    } else if (value >= '0' && value <= '9') {
        // Character digit - convert to number
        pulseCount = (value == '0') ? 10 : (value - '0');
    }
    
    Logger.printf("💡 Notify: DTMF key %d -> %d pulses (GPIO%d)\n", value, pulseCount, pin);
    pulseLed(pin, pulseCount);
}

void setPulseConfig(const PulseConfig& config) {
    pulseConfig = config;
    Logger.printf("💡 Pulse config: on=%dms, off=%dms, end=%dms\n",
                  pulseConfig.onDuration, pulseConfig.offDuration, pulseConfig.endDelay);
}

PulseConfig getPulseConfig() {
    return pulseConfig;
}

bool notificationsEnabled() {
    return initialized;
}

