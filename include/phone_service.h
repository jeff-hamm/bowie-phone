#ifndef PHONE_SERVICE_H
#define PHONE_SERVICE_H

#include <Arduino.h>
#include <functional>

// Define callback types
typedef std::function<void(bool)> HookStateCallback;
class PhoneService {
public:
    PhoneService();
    
    // Initialize the service
    void begin();
    
    // Main processing loop - call this frequently
    void loop();
    
    // Ringing control
    void startRinging();
    void stopRinging();
    bool isRinging() const { return _isRinging; }
    
    // Hook state
    bool isOffHook() const { return _isOffHook; }
    void setHookCallback(HookStateCallback callback);

private:
    // Pin definitions (will be loaded from config.h in cpp)
    int _pinFR;
    int _pinRM;
    int _pinSHK;
    
    // State variables
    bool _isRinging;
    bool _isOffHook;
    bool _lastShkReading;
    
    // Timing variables
    unsigned long _lastRingToggleTime;
    bool _ringState;
    
    unsigned long _lastDebounceTime;
    unsigned long _debounceDelay;
    
    // Callbacks
    HookStateCallback _hookCallback;
    
    // Internal helpers
    void updateRingSignal();
    void checkHookState();
};

extern PhoneService Phone;

#endif // PHONE_SERVICE_H
