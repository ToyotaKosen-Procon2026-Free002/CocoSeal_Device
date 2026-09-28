#include "WifiManager.h"

#include <WiFi.h>
#include <esp_wifi.h>

#define ESP_NOW_FALLBACK_CHANNEL 1

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

namespace {
unsigned long lastConnectionAttempt = 0;

void setEspNowFallbackChannel() {
    esp_err_t result =
        esp_wifi_set_channel(ESP_NOW_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("Failed to set ESP-NOW fallback channel: %d\n", result);
    }
}
}

bool setupWifi(unsigned long timeoutMs) {
    if (strlen(WIFI_SSID) == 0) {
        Serial.println("Wi-Fi credentials are not configured");
        return false;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastConnectionAttempt = millis();

    Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < timeoutMs) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Wi-Fi connected, IP: %s, channel: %u\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.channel());
        return true;
    }

    Serial.printf("Wi-Fi connection failed, status: %d\n", WiFi.status());
    setEspNowFallbackChannel();
    return false;
}

void maintainWifiConnection(unsigned long retryIntervalMs) {
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    unsigned long currentMillis = millis();
    if (currentMillis - lastConnectionAttempt < retryIntervalMs) {
        return;
    }
    lastConnectionAttempt = currentMillis;

    if (strlen(WIFI_SSID) == 0) {
        return;
    }

    Serial.println("Wi-Fi disconnected; retrying connection");
    setEspNowFallbackChannel();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
