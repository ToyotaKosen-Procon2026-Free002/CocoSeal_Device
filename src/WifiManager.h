#pragma once

#include <Arduino.h>

bool setupWifi(unsigned long timeoutMs = 15000);
void maintainWifiConnection(unsigned long retryIntervalMs = 30000);
bool isWifiConnected();
String getWifiIpAddress();
bool getTrustedUnixTime(uint32_t& timestamp);
bool getSosSigningUnixTime(uint32_t& timestamp, bool& isTrusted);
bool getTrustedLocalDateKey(uint32_t& dateKey);
