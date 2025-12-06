#include "phone_service.h"
#include "config.h"
#include "logging.h"

PhoneService Phone;

PhoneService::PhoneService() 
    : _pinFR(F_R), _pinRM(RM), _pinSHK(SHK),
      _isRinging(false), _isOffHook(false), _lastShkReading(false),
      _lastRingToggleTime(0), _ringState(false),
      _lastDebounceTime(0), _debounceDelay(50),
      _hookCallback(nullptr) {
}

void PhoneService::begin() {
    Logger.println("📞 Initializing Phone Service...");
    
    //Configure pins
    pinMode(_pinFR, OUTPUT);
    pinMode(_pinRM, OUTPUT);
    pinMode(_pinSHK, INPUT);
    
    //Set initial states
    digitalWrite(_pinFR, LOW);
    digitalWrite(_pinRM, LOW);
    
    // Read initial hook state
    _isOffHook = digitalRead(_pinSHK);
    _lastShkReading = _isOffHook;
    
    Logger.printf("📞 Phone Service Ready. Initial State: %s\n", _isOffHook ? "OFF HOOK" : "ON HOOK");
}

void PhoneService::loop() {
    checkHookState();
    
    if (_isRinging) {
        updateRingSignal();
    }
}

void PhoneService::startRinging() {
    if (_isRinging) return;
    
    // Don't ring if phone is already off hook
    if (_isOffHook) {
        Logger.println("⚠️ Cannot start ringing: Phone is off hook");
        return;
    }
    
    Logger.println("🔔 Starting Ring Signal");
    _isRinging = true;
    digitalWrite(_pinRM, HIGH); // Enable ring mode
    _lastRingToggleTime = millis();
    _ringState = false;
}

void PhoneService::stopRinging() {
    if (!_isRinging) return;
    
    Logger.println("🔕 Stopping Ring Signal");
    _isRinging = false;
    digitalWrite(_pinRM, LOW); // Disable ring mode
    digitalWrite(_pinFR, LOW); // Ensure F/R is low
}

void PhoneService::updateRingSignal() {
    // If phone is picked up while ringing, stop ringing immediately
    if (_isOffHook) {
        stopRinging();
        return;
    }

    unsigned long currentTime = millis();
    // 20Hz ring signal = 50ms period (25ms high, 25ms low)
    if (currentTime - _lastRingToggleTime >= 25) {
        _ringState = !_ringState;
        digitalWrite(_pinFR, _ringState ? HIGH : LOW);
        _lastRingToggleTime = currentTime;
    }
}

void PhoneService::checkHookState() {
    bool reading = digitalRead(_pinSHK);
    
    // If the switch changed, reset the debouncing timer
    if (reading != _lastShkReading) {
        _lastDebounceTime = millis();
        _lastShkReading = reading; // Update last reading to track stability
    }
    
    if ((millis() - _lastDebounceTime) > _debounceDelay) {
        // Whatever the reading is at, it's been there for longer than the debounce
        // delay, so take it as the actual current state:
        
        if (reading != _isOffHook) {
            _isOffHook = reading;
            
            if (_isOffHook) {
                Logger.println("📞 Phone picked up (OFF HOOK)");
                // Stop ringing if we were ringing
                if (_isRinging) {
                    stopRinging();
                }
            } else {
                Logger.println("📞 Phone hung up (ON HOOK)");
            }
            
            // Notify callback if registered
            if (_hookCallback) {
                _hookCallback(_isOffHook);
            }
        }
    }
}

void PhoneService::setHookCallback(HookStateCallback callback) {
    _hookCallback = callback;
}
