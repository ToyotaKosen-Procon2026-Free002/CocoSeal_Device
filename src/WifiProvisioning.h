#pragma once

#include <Arduino.h>

bool startWifiProvisioning(const char* deviceId, bool wifiConnected);
bool takeProvisionedWifiCredentials(char* ssid,
                                    size_t ssidCapacity,
                                    char* password,
                                    size_t passwordCapacity);
void updateWifiProvisioningStatus(bool wifiConnected, const char* state);
void stopWifiProvisioningAdvertising();
void maintainWifiProvisioning();
