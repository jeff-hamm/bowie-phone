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
    
    // Note: PD is hardwired to +3.3V (SLIC always enabled)
    
    //Configure pins
    pinMode(_pinFR, OUTPUT);
    pinMode(_pinRM, OUTPUT);
    // Use pull-up so the hook signal is not left floating; SHK should pull low when on-hook if wired that way
    pinMode(_pinSHK, INPUT);
    
    //Set initial states
    digitalWrite(_pinFR, LOW);
    digitalWrite(_pinRM, LOW);
    
    // Read initial hook state
    #ifdef ASSUME_HOOK
    _isOffHook = false;
    #else
    _isOffHook = digitalRead(_pinSHK);
    #endif
    _lastShkReading = _isOffHook;
    
    Logger.printf("📞 Phone Service Ready. Initial State: %s\n", _isOffHook ? "OFF HOOK" : "ON HOOK");
}

void PhoneService::loop() {
    checkHookState();
    
    if (_isRinging) {
        updateRingSignal();
    }

    // Periodic debug to verify hook sensing in hardware
    // static unsigned long lastDebug = 0;
    // if (millis() - lastDebug > 1000) {
    //     bool raw = digitalRead(_pinSHK);
    //     Logger.printf("📟 Hook debug: raw=%d, isOffHook=%d, isRinging=%d\n", raw, _isOffHook, _isRinging);
    //     lastDebug = millis();
    // }
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

void PhoneService::checkHookState() {
    
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
