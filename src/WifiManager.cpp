#include "WifiManager.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <time.h>
#include <atomic>
#include <Preferences.h>

#include "WifiProvisioning.h"

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
constexpr size_t MAX_SSID_LENGTH = 32;
constexpr size_t MAX_PASSWORD_LENGTH = 63;
constexpr char WIFI_PREFERENCES_NAMESPACE[] = "wifi_cfg";

unsigned long lastConnectionAttempt = 0;
bool networkTimeSyncConfigured = false;
std::atomic<bool> networkTimeTrusted{false};
uint8_t lastConnectedChannel = 0;
uint32_t lastTimeSyncDiagnostic = 0;
char activeSsid[MAX_SSID_LENGTH + 1] = {};
char activePassword[MAX_PASSWORD_LENGTH + 1] = {};
char candidateSsid[MAX_SSID_LENGTH + 1] = {};
char candidatePassword[MAX_PASSWORD_LENGTH + 1] = {};
bool activeCredentialsAvailable = false;
bool candidateConnectionPending = false;
uint32_t candidateConnectionStartedAt = 0;

void setEspNowFallbackChannel() {
    esp_err_t result =
        esp_wifi_set_channel(ESP_NOW_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("Failed to set ESP-NOW fallback channel: %d\n", result);
    }
}

void initializeWifiStation() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.setAutoReconnect(false);
}

bool copyCredential(char* destination,
                    size_t capacity,
                    const String& value,
                    size_t maxLength) {
    if (value.length() > maxLength || value.length() >= capacity) {
        return false;
    }
    snprintf(destination, capacity, "%s", value.c_str());
    return true;
}

bool loadWifiCredentials() {
    Preferences preferences;
    if (preferences.begin(WIFI_PREFERENCES_NAMESPACE, true)) {
        String ssid = preferences.getString("ssid", "");
        String password = preferences.getString("password", "");
        preferences.end();

        if (!ssid.isEmpty()) {
            if (!copyCredential(activeSsid, sizeof(activeSsid), ssid,
                                MAX_SSID_LENGTH) ||
                !copyCredential(activePassword, sizeof(activePassword),
                                password, MAX_PASSWORD_LENGTH)) {
                Serial.println(
                    "Wi-Fi error: stored credentials have invalid lengths");
                memset(activeSsid, 0, sizeof(activeSsid));
                memset(activePassword, 0, sizeof(activePassword));
                return false;
            }
            activeCredentialsAvailable = true;
            return true;
        }
    } else {
        Serial.println(
            "Wi-Fi notice: no saved credentials; checking build-time configuration");
    }

    const String configuredSsid = WIFI_SSID;
    const String configuredPassword = WIFI_PASSWORD;
    if (configuredSsid.isEmpty()) {
        return false;
    }
    if (!copyCredential(activeSsid, sizeof(activeSsid), configuredSsid,
                        MAX_SSID_LENGTH) ||
        !copyCredential(activePassword, sizeof(activePassword),
                        configuredPassword, MAX_PASSWORD_LENGTH)) {
        Serial.println("Wi-Fi error: configured credentials have invalid lengths");
        memset(activeSsid, 0, sizeof(activeSsid));
        memset(activePassword, 0, sizeof(activePassword));
        return false;
    }
    activeCredentialsAvailable = true;
    return true;
}

bool persistWifiCredentials(const char* ssid, const char* password) {
    Preferences preferences;
    if (!preferences.begin(WIFI_PREFERENCES_NAMESPACE, false)) {
        Serial.println("Wi-Fi error: failed to open credential storage");
        return false;
    }
    const size_t savedSsid = preferences.putString("ssid", ssid);
    const size_t savedPassword = preferences.putString("password", password);
    preferences.end();
    return savedSsid == strlen(ssid) + 1 &&
           savedPassword == strlen(password) + 1;
}

void beginNetworkTimeSync() {
    networkTimeTrusted.store(false);
    configTzTime("JST-9", "pool.ntp.org", "time.google.com");
    networkTimeSyncConfigured = true;
    lastConnectedChannel = WiFi.channel();
}

void startCandidateConnection(const char* ssid, const char* password) {
    snprintf(candidateSsid, sizeof(candidateSsid), "%s", ssid);
    snprintf(candidatePassword, sizeof(candidatePassword), "%s", password);
    candidateConnectionPending = true;
    candidateConnectionStartedAt = millis();
    lastConnectionAttempt = candidateConnectionStartedAt;

    WiFi.disconnect(false, false);
    WiFi.begin(candidateSsid, candidatePassword);
    updateWifiProvisioningStatus(false, "connecting");
    Serial.println("Wi-Fi provisioning: connection attempt started");
}

void finishCandidateConnection() {
    const bool saved = persistWifiCredentials(candidateSsid, candidatePassword);
    snprintf(activeSsid, sizeof(activeSsid), "%s", candidateSsid);
    snprintf(activePassword, sizeof(activePassword), "%s", candidatePassword);
    activeCredentialsAvailable = true;
    memset(candidateSsid, 0, sizeof(candidateSsid));
    memset(candidatePassword, 0, sizeof(candidatePassword));
    candidateConnectionPending = false;
    beginNetworkTimeSync();
    updateWifiProvisioningStatus(
        true, saved ? "connected" : "connected_not_saved");
    if (!saved) {
        Serial.println(
            "Wi-Fi connected, but credentials could not be persisted");
    } else {
        Serial.printf("Wi-Fi connected, IP: %s, channel: %u\n",
                      WiFi.localIP().toString().c_str(),
                      lastConnectedChannel);
    }
    stopWifiProvisioningAdvertising();
}

void failCandidateConnection() {
    memset(candidateSsid, 0, sizeof(candidateSsid));
    memset(candidatePassword, 0, sizeof(candidatePassword));
    candidateConnectionPending = false;
    updateWifiProvisioningStatus(false, "connection_failed");
    WiFi.disconnect(false, false);

    if (activeCredentialsAvailable) {
        Serial.println(
            "Wi-Fi provisioning failed; retrying the previously saved network");
        WiFi.begin(activeSsid, activePassword);
        lastConnectionAttempt = millis();
    } else {
        setEspNowFallbackChannel();
    }
}

void processProvisionedCredentials() {
    char ssid[MAX_SSID_LENGTH + 1] = {};
    char password[MAX_PASSWORD_LENGTH + 1] = {};
    if (!takeProvisionedWifiCredentials(ssid, sizeof(ssid), password,
                                        sizeof(password))) {
        return;
    }

#if !CONTEST_MODE
    Serial.println(
        "Wi-Fi provisioning received credentials, but contest mode is disabled");
    updateWifiProvisioningStatus(false, "wifi_disabled");
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
#else
    startCandidateConnection(ssid, password);
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
#endif
}

void maintainCandidateConnection() {
    if (!candidateConnectionPending) {
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        finishCandidateConnection();
        return;
    }
    if (millis() - candidateConnectionStartedAt >= 15000) {
        Serial.printf("Wi-Fi provisioning failed, status: %d\n",
                      WiFi.status());
        failCandidateConnection();
    }
}
}

bool setupWifi(unsigned long timeoutMs) {
    initializeWifiStation();
#if !CONTEST_MODE
    Serial.println("Contest mode is disabled; keeping ESP-NOW on channel 1");
    WiFi.disconnect(false, false);
    setEspNowFallbackChannel();
    return false;
#else
    if (!loadWifiCredentials()) {
        Serial.println(
            "Wi-Fi credentials are not configured; use BLE provisioning");
        WiFi.disconnect(false, false);
        setEspNowFallbackChannel();
        return false;
    }

    WiFi.begin(activeSsid, activePassword);
    lastConnectionAttempt = millis();

    Serial.println("Connecting to the configured Wi-Fi network");
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < timeoutMs) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        beginNetworkTimeSync();
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
    processProvisionedCredentials();
    maintainWifiProvisioning();
#if !CONTEST_MODE
    (void)retryIntervalMs;
    return;
#else
    maintainCandidateConnection();
    if (candidateConnectionPending) {
        return;
    }

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

    if (!activeCredentialsAvailable) {
        return;
    }

    Serial.println("Wi-Fi disconnected; retrying connection");
    WiFi.begin(activeSsid, activePassword);
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
