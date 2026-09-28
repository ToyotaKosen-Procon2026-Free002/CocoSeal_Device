#pragma once

#include <Arduino.h>

bool setupWifi(unsigned long timeoutMs = 15000);
void maintainWifiConnection(unsigned long retryIntervalMs = 10000);
bool isWifiConnected();
String getWifiIpAddress();
