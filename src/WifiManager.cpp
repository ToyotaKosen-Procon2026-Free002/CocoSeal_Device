#include "WifiManager.h"

#include <WiFi.h>
#include <esp_wifi.h>

#define ESP_NOW_FALLBACK_CHANNEL 1

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

volatile WifiStatus wifiStatus = WIFI_STATUS_NONE;

bool setupWifi(unsigned long timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < timeoutMs) {
        delay(250);
    }

    bool connected = WiFi.status() == WL_CONNECTED;
    wifiStatus = connected ? WIFI_STATUS_SUCCESS : WIFI_STATUS_FAILED;
    if (!connected) {
        keepEspNowChannel();
    }
    return connected;
}

void maintainWifiConnection(unsigned long retryIntervalMs) {
    static unsigned long lastCheckTime = 0;

    unsigned long currentMillis = millis();
    if (currentMillis - lastCheckTime < retryIntervalMs) {
        return;
    }
    lastCheckTime = currentMillis;

    if (isWifiConnected()) {
        wifiStatus = WIFI_STATUS_SUCCESS;
        return;
    }

    wifiStatus = WIFI_STATUS_FAILED;
    keepEspNowChannel();
}

void keepEspNowChannel() {
    esp_wifi_set_channel(ESP_NOW_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

bool isWifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String getWifiIpAddress() {
    if (!isWifiConnected()) {
        return String();
    }
    return WiFi.localIP().toString();
}

WifiStatus getWifiStatus() {
    return wifiStatus;
}

void clearWifiStatus() {
    wifiStatus = WIFI_STATUS_NONE;
}
