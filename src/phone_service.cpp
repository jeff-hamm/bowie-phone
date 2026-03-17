#include "phone_service.h"
#include "config.h"
#include "logging.h"
#include "tone_generators.h"

PhoneService Phone;

PhoneService::PhoneService() 
#ifdef CAN_RING
    : _pinFR(F_R), _pinRM(RM), _pinSHK(SHK),
      _isRinging(false), _isOffHook(false), _lastShkReading(false),
      _debugOverride(false), _debugOverrideExpiry(0),
      _lastRingToggleTime(0), _ringState(false),
      _lastDebounceTime(0), _debounceDelay(50),
      _hookCallback(nullptr) {
#else
    : _pinSHK(SHK),
      _isOffHook(false), _lastShkReading(false),
      _debugOverride(false), _debugOverrideExpiry(0),
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
    // Expire timed debug override and restore physical pin state
    if (_debugOverride && _debugOverrideExpiry != 0 && millis() >= _debugOverrideExpiry) {
        Logger.println("🔧 [DEBUG] Hook override EXPIRED - resuming automatic detection");
        _debugOverride = false;
        _debugOverrideExpiry = 0;
        // Immediately sync to whatever the pin currently reads
        bool pinState = digitalRead(_pinSHK);
        setOffHook(pinState, false);
    }

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
        _debugOverrideExpiry = 0;
        Logger.println("🔧 [DEBUG] Hook override DISABLED - resuming automatic detection");
    }
}

void PhoneService::setOffHook(bool offHook, bool override, unsigned long overrideForMs) {
    // When called from debug, enable override to ignore physical pin
    _debugOverride = override;
    if (override) {
        _debugOverrideExpiry = (overrideForMs > 0) ? (millis() + overrideForMs) : 0;
        Logger.printf("🔧 [DEBUG] Hook override ENABLED - physical pin ignored for %lums\n", overrideForMs);
    } else {
        _debugOverrideExpiry = 0;
    }
    
    if (offHook != _isOffHook) {
        _isOffHook = offHook;
        
        // Log debug override state changes
        if (override) {
            Logger.printf("📞 [DEBUG] Phone set to %s\n", _isOffHook ? "OFF HOOK" : "ON HOOK");
        }
        
#ifdef CAN_RING
        // Stop ringing when phone goes off-hook
        if (_isOffHook && _isRinging) {
            stopRinging();
        }
#endif
        
        // Notify callback if registered (app-level hook state logic happens here)
        if (_hookCallback) {
            _hookCallback(_isOffHook);
        }
    }
}
