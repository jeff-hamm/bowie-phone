#ifndef PHONE_SERVICE_H
#define PHONE_SERVICE_H

#include <Arduino.h>
#include <functional>
#include "config.h"

// Define callback types
typedef std::function<void(bool)> HookStateCallback;
class PhoneService {
public:
    PhoneService();
    
    // Initialize the service
    void begin();
    
    // Main processing loop - call this frequently
    void loop();
    
    // Ringing control (no-op when CAN_RING is not defined)
    void startRinging();
    void stopRinging();
    bool isRinging() const;
    
    // Hook state
    bool isOffHook() const { return _isOffHook; }
    void setHookCallback(HookStateCallback callback);
    
    // Debug/simulation methods
    void setOffHook(bool offHook, bool fromDebug = true);  // Simulate hook state change

private:
#ifdef CAN_RING
    // Pin definitions for ringing (will be loaded from config.h in cpp)
    int _pinFR;
    int _pinRM;
    bool _isRinging;
    unsigned long _lastRingToggleTime;
    bool _ringState;
    void updateRingSignal();
#endif
    int _pinSHK;
    
    // State variables
    bool _isOffHook;
    bool _lastShkReading;
    bool _debugOverride;  // When true, ignore physical hook pin
    
    unsigned long _lastDebounceTime;
    unsigned long _debounceDelay;
    
    // Callbacks
    HookStateCallback _hookCallback;
    
    // Internal helpers
    void checkHookState();
};

extern PhoneService Phone;

#endif // PHONE_SERVICE_H
