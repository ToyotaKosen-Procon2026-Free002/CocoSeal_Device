#include "WifiManager.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <time.h>

#define ESP_NOW_FALLBACK_CHANNEL 1

#ifndef WIFI_
#define WIFI_ ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

namespace {
unsigned long lastConnectionAttempt = 0;
bool networkTimeSyncConfigured = false;

void setEspNowFallbackChannel() {
    esp_err_t result =
        esp_wifi_set_channel(ESP_NOW_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("Failed to set ESP-NOW fallback channel: %d\n", result);
    }
}
}

bool setupWifi(unsigned long timeoutMs) {
    if (strlen(WIFI_) == 0) {
        Serial.println("Wi-Fi credentials are not configured");
        return false;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(WIFI_, WIFI_PASSWORD);
    lastConnectionAttempt = millis();

    Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_);
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < timeoutMs) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        configTzTime("JST-9", "pool.ntp.org", "time.google.com");
        networkTimeSyncConfigured = true;
        Serial.printf("Wi-Fi connected, IP: %s, channel: %u\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.channel());
        return true;
    }

    Serial.printf("Wi-Fi connection failed, status: %d\n", WiFi.status());
    WiFi.disconnect(false, false);
    delay(100);
    setEspNowFallbackChannel();
    return false;
}

void maintainWifiConnection(unsigned long retryIntervalMs) {
    if (WiFi.status() == WL_CONNECTED) {
        if (!networkTimeSyncConfigured) {
            configTzTime("JST-9", "pool.ntp.org", "time.google.com");
            networkTimeSyncConfigured = true;
            Serial.println("Network time synchronization started (JST)");
        }
        return;
    }

    unsigned long currentMillis = millis();
    if (currentMillis - lastConnectionAttempt < retryIntervalMs) {
        return;
    }
    lastConnectionAttempt = currentMillis;

    if (strlen(WIFI_) == 0) {
        return;
    }

    Serial.println("Wi-Fi disconnected; retrying connection");
    WiFi.begin(WIFI_, WIFI_PASSWORD);
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

bool getTrustedLocalDateKey(uint32_t& dateKey) {
    dateKey = 0;
    if (!networkTimeSyncConfigured ||
        sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        return false;
    }

    time_t now = time(nullptr);
    struct tm localTime = {};
    if (now < 1735689600 || localtime_r(&now, &localTime) == nullptr ||
        localTime.tm_year + 1900 < 2025) {
        return false;
    }

    dateKey = static_cast<uint32_t>((localTime.tm_year + 1900) * 10000 +
                                    (localTime.tm_mon + 1) * 100 +
                                    localTime.tm_mday);
    return true;
}
