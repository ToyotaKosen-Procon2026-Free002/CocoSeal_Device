#pragma once

#include <Arduino.h>

bool setupWifi(unsigned long timeoutMs = 15000);
void maintainWifiConnection(unsigned long retryIntervalMs = 30000);
bool isWifiConnected();
String getWifiIpAddress();
uint8_t getCurrentRadioChannel();
