#include "phone_service.h"
#include "config.h"
#include "logging.h"
#include "tone_generators.h"

#ifdef AUTO_CAPTURE_ON_OFFHOOK
#ifdef DEBUG
extern void performAudioCapture(int durationSec);
#endif
#endif

PhoneService Phone;

// Tone generators (owned by this module)
static DualToneGenerator dialToneGenerator(350.0f, 440.0f, 16000.0f);
static DualToneGenerator ringbackToneGenerator(440.0f, 480.0f, 16000.0f);
static RepeatingToneGenerator<int16_t> ringbackRepeater(ringbackToneGenerator, 2000, 4000);

PhoneService::PhoneService() 
#ifdef CAN_RING
    : _pinFR(F_R), _pinRM(RM), _pinSHK(SHK),
      _isRinging(false), _isOffHook(false), _lastShkReading(false),
      _debugOverride(false),
      _lastRingToggleTime(0), _ringState(false),
      _lastDebounceTime(0), _debounceDelay(50),
      _hookCallback(nullptr) {
#else
    : _pinSHK(SHK),
      _isOffHook(false), _lastShkReading(false),
      _debugOverride(false),
      _lastDebounceTime(0), _debounceDelay(50),
      _hookCallback(nullptr) {
#endif
}

void PhoneService::begin() {
    Logger.println("📞 Initializing Phone Service...");
    
    // Note: PD is hardwired to +3.3V (SLIC always enabled)
    
#ifdef CAN_RING
    //Configure ring pins
    pinMode(_pinFR, OUTPUT);
    pinMode(_pinRM, OUTPUT);
    digitalWrite(_pinFR, LOW);
    digitalWrite(_pinRM, LOW);
#endif
    
    // Use pull-up so the hook signal is not left floating; SHK should pull low when on-hook if wired that way
    pinMode(_pinSHK, INPUT);
    
    // Read initial hook state
    #ifdef ASSUME_HOOK
    _isOffHook = false;
    #else
    _isOffHook = digitalRead(_pinSHK);
    #endif
    _lastShkReading = _isOffHook;
    
#ifdef CAN_RING
    Logger.printf("📞 Phone Service Ready (ringing enabled). Initial State: %s\n", _isOffHook ? "OFF HOOK" : "ON HOOK");
#else
    Logger.printf("📞 Phone Service Ready (ringing disabled). Initial State: %s\n", _isOffHook ? "OFF HOOK" : "ON HOOK");
#endif
}

void PhoneService::loop() {
    checkHookState();
    
#ifdef CAN_RING
    if (_isRinging) {
        updateRingSignal();
    }
#endif

    // Periodic debug to verify hook sensing in hardware
    // static unsigned long lastDebug = 0;
    // if (millis() - lastDebug > 1000) {
    //     bool raw = digitalRead(_pinSHK);
    //     Logger.printf("📟 Hook debug: raw=%d, isOffHook=%d, isRinging=%d\n", raw, _isOffHook, _isRinging);
    //     lastDebug = millis();
    // }
}

#ifdef CAN_RING
void PhoneService::startRinging() {
    if (_isRinging) return;
    
    // Don't ring if phone is already off hook
    if (_isOffHook) {
//        Logger.println("⚠️ Cannot start ringing: Phone is off hook");
        return;
    }
    
    Logger.println("🔔 Starting Ring Signal");
    _isRinging = true;
    digitalWrite(_pinRM, HIGH); // Enable ring mode
    _ringState = false;
    digitalWrite(_pinFR, LOW);
    // Force immediate first toggle on next loop to verify FR movement
    _lastRingToggleTime = millis() - RING_CYCLE_MS;

    // One-time debug snapshot when ring starts
    Logger.printf("📟 Ring start debug: RM=%d FR=%d SHK=%d\n", digitalRead(_pinRM), digitalRead(_pinFR), digitalRead(_pinSHK));
}

void PhoneService::stopRinging() {
    if (!_isRinging) return;
    
    Logger.println("🔕 Stopping Ring Signal");
    _isRinging = false;
    digitalWrite(_pinRM, LOW); // Disable ring mode
    digitalWrite(_pinFR, LOW); // Ensure F/R is low
}

bool PhoneService::isRinging() const {
    return _isRinging;
}

void PhoneService::updateRingSignal() {
    // If phone is picked up while ringing, stop ringing immediately
    if (_isOffHook) {
        stopRinging();
        return;
    }

    unsigned long currentTime = millis();
    // 20Hz ring signal = 50ms period (25ms high, 25ms low)
    if (currentTime - _lastRingToggleTime >= RING_CYCLE_MS)
    {
        _ringState = !_ringState;
        digitalWrite(_pinFR, _ringState ? HIGH : LOW);
        _lastRingToggleTime = currentTime;
    }
}

#else
// No-op implementations when CAN_RING is not defined
void PhoneService::startRinging() {}
void PhoneService::stopRinging() {}
bool PhoneService::isRinging() const { return false; }
#endif

void PhoneService::checkHookState() {
    // Skip physical hook checking when debug override is active
    if (_debugOverride) {
        return;
    }
    
#ifdef ASSUME_HOOK
    bool reading = false;
#else
    bool reading = digitalRead(_pinSHK);
#endif    
    // If the switch changed, reset the debouncing timer
    if (reading != _lastShkReading) {
        _lastDebounceTime = millis();
        _lastShkReading = reading; // Update last reading to track stability
    }
    
    if ((millis() - _lastDebounceTime) > _debounceDelay) {
        // Whatever the reading is at, it's been there for longer than the debounce
        // delay, so take it as the actual current state:
        
        if (reading != _isOffHook) {
            // Use setOffHook to handle state change, ringing stop, and callback
            setOffHook(reading, false); // false = not from debug
        }
    }
}

void PhoneService::setHookCallback(HookStateCallback callback) {
    _hookCallback = callback;
}

void PhoneService::resetDebugOverride() {
    if (_debugOverride) {
        _debugOverride = false;
        Logger.println("🔧 [DEBUG] Hook override DISABLED - resuming automatic detection");
    }
}

void PhoneService::setOffHook(bool offHook, bool override) {
    // When called from debug, enable override to ignore physical pin
    _debugOverride = override;
    if (override) {
        Logger.println("🔧 [DEBUG] Hook override ENABLED - physical pin ignored");
    }
    
    if (offHook != _isOffHook) {
        _isOffHook = offHook;
        
        if (_isOffHook) {
            if (override) {
                Logger.println("📞 [DEBUG] Phone set to OFF HOOK");
            } else {
                Logger.println("📞 Phone picked up (OFF HOOK)");
#ifdef AUTO_CAPTURE_ON_OFFHOOK
#ifdef DEBUG
                Logger.println("🎙️ Auto-triggering audio capture (10 seconds)...");
                performAudioCapture(10);
#endif
#endif
            }
#ifdef CAN_RING
            if (_isRinging) {
                stopRinging();
            }
#endif
        } else {
            if (override) {
                Logger.println("📞 [DEBUG] Phone set to ON HOOK");
            } else {
                Logger.println("📞 Phone hung up (ON HOOK)");
            }
        }
        
        // Notify callback if registered
        if (_hookCallback) {
            _hookCallback(_isOffHook);
        }
    }
}
