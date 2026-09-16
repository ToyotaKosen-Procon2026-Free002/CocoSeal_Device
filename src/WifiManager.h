#pragma once

#include <Arduino.h>

enum WifiStatus {
    WIFI_STATUS_NONE,
    WIFI_STATUS_SUCCESS,
    WIFI_STATUS_FAILED
};

bool setupWifi(unsigned long timeoutMs = 15000);
void maintainWifiConnection(unsigned long retryIntervalMs = 10000);
bool isWifiConnected();
void keepEspNowChannel();
String getWifiIpAddress();
WifiStatus getWifiStatus();
void clearWifiStatus();
