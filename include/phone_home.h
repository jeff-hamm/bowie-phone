#ifndef PHONE_HOME_H
#define PHONE_HOME_H

#include <Arduino.h>

// Phone Home - periodic check-in with server for remote management
// Returns true if an OTA update was triggered
bool checkForRemoteUpdates(const char* serverUrl = nullptr);

// Set the phone home interval (default: 1 hour)
void setPhoneHomeInterval(unsigned long intervalMs);

// Get the last phone home status
const char* getPhoneHomeStatus();

#endif
