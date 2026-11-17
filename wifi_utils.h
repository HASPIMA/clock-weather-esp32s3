// wifi_utils.h - Connect/disconnect helpers shared by every task that uses WiFi.
// The WiFi radio is global: callers must hold wifiMutex around connect ... off.
#pragma once

#include <Arduino.h>

// Connects in station mode with the credentials from wifi.txt.
// Returns false if not connected within timeoutMs.
bool wifiConnect(const char* tag, uint32_t timeoutMs);

// Disconnects and turns the radio off to save power.
void wifiOff(const char* tag);
