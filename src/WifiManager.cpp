#include "WifiManager.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <time.h>
#include <atomic>

#define ESP_NOW_FALLBACK_CHANNEL 1

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef CONTEST_MODE
#define CONTEST_MODE 0
#endif

namespace {
unsigned long lastConnectionAttempt = 0;
bool networkTimeSyncConfigured = false;
std::atomic<bool> networkTimeTrusted{false};
uint8_t lastConnectedChannel = 0;
uint32_t lastTimeSyncDiagnostic = 0;

void setEspNowFallbackChannel() {
    esp_err_t result =
        esp_wifi_set_channel(ESP_NOW_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("Failed to set ESP-NOW fallback channel: %d\n", result);
    }
}
}

bool setupWifi(unsigned long timeoutMs) {
#if !CONTEST_MODE
    Serial.println("Contest mode is disabled; keeping ESP-NOW on channel 1");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    setEspNowFallbackChannel();
    return false;
#else
    if (strlen(WIFI_SSID) == 0) {
        Serial.println("Wi-Fi credentials are not configured");
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, false);
        setEspNowFallbackChannel();
        return false;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastConnectionAttempt = millis();

    Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < timeoutMs) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        networkTimeTrusted.store(false);
        configTzTime("JST-9", "pool.ntp.org", "time.google.com");
        networkTimeSyncConfigured = true;
        lastConnectedChannel = WiFi.channel();
        Serial.printf("Wi-Fi connected, IP: %s, channel: %u\n",
                      WiFi.localIP().toString().c_str(),
                      lastConnectedChannel);
        return true;
    }

    Serial.printf("Wi-Fi connection failed, status: %d\n", WiFi.status());
    WiFi.disconnect(false, false);
    delay(100);
    setEspNowFallbackChannel();
    return false;
#endif
}

void maintainWifiConnection(unsigned long retryIntervalMs) {
#if !CONTEST_MODE
    (void)retryIntervalMs;
    return;
#else
    if (WiFi.status() == WL_CONNECTED) {
        uint8_t currentChannel = WiFi.channel();
        if (currentChannel != lastConnectedChannel) {
            lastConnectedChannel = currentChannel;
            Serial.printf(
                "Wi-Fi channel changed to %u; ESP-NOW follows current channel\n",
                currentChannel);
        }
        if (!networkTimeSyncConfigured) {
            networkTimeTrusted.store(false);
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

    if (strlen(WIFI_SSID) == 0) {
        return;
    }

    Serial.println("Wi-Fi disconnected; retrying connection");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
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
    uint32_t timestamp = 0;
    if (!getTrustedUnixTime(timestamp)) {
        return false;
    }

    time_t now = static_cast<time_t>(timestamp);
    struct tm localTime = {};
    if (localtime_r(&now, &localTime) == nullptr) {
        return false;
    }

    dateKey = static_cast<uint32_t>((localTime.tm_year + 1900) * 10000 +
                                    (localTime.tm_mon + 1) * 100 +
                                    localTime.tm_mday);
    return true;
}

bool getTrustedUnixTime(uint32_t& timestamp) {
    timestamp = 0;
    const sntp_sync_status_t syncStatus = sntp_get_sync_status();
    if (syncStatus == SNTP_SYNC_STATUS_COMPLETED) {
        networkTimeTrusted.store(true);
    }
    time_t now = time(nullptr);
    bool timeIsValid = now >= 1735689600 &&
                       static_cast<uint64_t>(now) <= UINT32_MAX;
    if (!networkTimeSyncConfigured ||
        !networkTimeTrusted.load() ||
        !timeIsValid) {
        uint32_t currentMillis = millis();
        if (currentMillis - lastTimeSyncDiagnostic >= 30000) {
            lastTimeSyncDiagnostic = currentMillis;
            const char* syncStatusName =
                syncStatus == SNTP_SYNC_STATUS_COMPLETED ? "completed" :
                syncStatus == SNTP_SYNC_STATUS_IN_PROGRESS ? "in progress" :
                                                              "not started";
            Serial.printf(
                "Time sync pending: configured=%s, SNTP=%s (%d), epoch=%lld, "
                "trusted=%s, Wi-Fi=%s\n",
                networkTimeSyncConfigured ? "yes" : "no",
                syncStatusName,
                static_cast<int>(syncStatus),
                static_cast<long long>(now),
                networkTimeTrusted.load() ? "yes" : "no",
                WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
        }
        return false;
    }

    timestamp = static_cast<uint32_t>(now);
    return true;
}
