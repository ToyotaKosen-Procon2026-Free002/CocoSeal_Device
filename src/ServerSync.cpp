#include "ServerSync.h"

#include "BatteryManager.h"
#include "DeviceIdentity.h"
#include "EspNowManager.h"
#include "LocalDatabase.h"
#include "SealInventory.h"
#include "WifiManager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/ecdsa.h>
#include <time.h>

#ifndef SERVER_BASE_URL
#define SERVER_BASE_URL "https://coco-seal.mydns.jp"
#endif

#ifndef SERVER_API_HEX_FIELDS
#define SERVER_API_HEX_FIELDS 0
#endif

#ifndef CONTEST_MODE
#define CONTEST_MODE 0
#endif

namespace {
constexpr uint32_t SYNC_INTERVAL_MS = 5000;
constexpr uint32_t TRADE_POOL_REFRESH_INTERVAL_MS = 30000;
constexpr uint32_t HTTP_TIMEOUT_MS = 3000;
constexpr uint32_t SYNC_DIAGNOSTIC_MAGIC = 0x53594E43;

enum SyncDiagnosticStage : uint32_t {
    SYNC_STAGE_IDLE = 0,
    SYNC_STAGE_CHECK_WIFI,
    SYNC_STAGE_CHECK_TIME,
    SYNC_STAGE_FIND_EVENT,
    SYNC_STAGE_ACTIVATE_KEY,
    SYNC_STAGE_ACTIVATE_JSON,
    SYNC_STAGE_DEVICE_PROFILE_GET,
    SYNC_STAGE_DEVICE_PROFILE_REQUEST,
    SYNC_STAGE_DEVICE_PROFILE_PARSE,
    SYNC_STAGE_TRADE_POOL_GET,
    SYNC_STAGE_TRADE_POOL_REQUEST,
    SYNC_STAGE_TRADE_POOL_PARSE,
    SYNC_STAGE_HTTPS_BEGIN,
    SYNC_STAGE_HTTP_SIGNATURE,
    SYNC_STAGE_HTTP_POST,
    SYNC_STAGE_HTTP_RESPONSE,
    SYNC_STAGE_EVENT_TIMESTAMP,
    SYNC_STAGE_EVENT_SIGNATURE,
    SYNC_STAGE_EVENT_JSON,
    SYNC_STAGE_REMOVE_EVENT
};

RTC_DATA_ATTR uint32_t syncDiagnosticMagic = 0;
RTC_DATA_ATTR uint32_t syncDiagnosticStage = SYNC_STAGE_IDLE;

constexpr char SERVER_ROOT_CA[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

uint32_t lastSyncAttempt = 0;
uint32_t lastDeviceProfileRefreshAttempt = 0;
uint32_t lastTradePoolRefreshAttempt = 0;
bool deviceActivated = false;
bool syncScheduleInitialized = false;
bool deviceProfileRefreshAttempted = false;
bool tradePoolRefreshAttempted = false;
bool batteryUpdatePending = false;
bool previousWifiConnected = false;

const char* syncStageName(uint32_t stage) {
    switch (stage) {
        case SYNC_STAGE_CHECK_WIFI: return "checking Wi-Fi";
        case SYNC_STAGE_CHECK_TIME: return "checking trusted time";
        case SYNC_STAGE_FIND_EVENT: return "finding pending event";
        case SYNC_STAGE_ACTIVATE_KEY: return "reading activation public key";
        case SYNC_STAGE_ACTIVATE_JSON: return "building activation JSON";
        case SYNC_STAGE_DEVICE_PROFILE_GET: return "fetching device profile";
        case SYNC_STAGE_DEVICE_PROFILE_REQUEST: return "sending device-profile GET";
        case SYNC_STAGE_DEVICE_PROFILE_PARSE: return "parsing device profile";
        case SYNC_STAGE_TRADE_POOL_GET: return "fetching server trade pool";
        case SYNC_STAGE_TRADE_POOL_REQUEST: return "sending trade-pool GET";
        case SYNC_STAGE_TRADE_POOL_PARSE: return "parsing server trade pool";
        case SYNC_STAGE_HTTPS_BEGIN: return "initializing HTTPS";
        case SYNC_STAGE_HTTP_SIGNATURE: return "signing HTTP request";
        case SYNC_STAGE_HTTP_POST: return "sending HTTP POST";
        case SYNC_STAGE_HTTP_RESPONSE: return "reading HTTP response";
        case SYNC_STAGE_EVENT_TIMESTAMP: return "preparing event timestamp";
        case SYNC_STAGE_EVENT_SIGNATURE: return "signing event";
        case SYNC_STAGE_EVENT_JSON: return "building event JSON";
        case SYNC_STAGE_REMOVE_EVENT: return "removing synced event";
        default: return "idle/unknown";
    }
}

void setSyncDiagnosticStage(SyncDiagnosticStage stage, bool log = true) {
    syncDiagnosticMagic = SYNC_DIAGNOSTIC_MAGIC;
    syncDiagnosticStage = static_cast<uint32_t>(stage);
    if (!log) {
        return;
    }
    Serial.printf(
        "Server sync diagnostic: %s; free heap %u, largest block %u, loop stack watermark %u\n",
        syncStageName(syncDiagnosticStage),
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    Serial.flush();
}

String toLowerString(const char* value) {
    String result(value ? value : "");
    result.toLowerCase();
    return result;
}

String toHex(const uint8_t* bytes, size_t length) {
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    String result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        result += HEX_DIGITS[bytes[i] >> 4];
        result += HEX_DIGITS[bytes[i] & 0x0F];
    }
    return result;
}

String timestampToIso8601(uint32_t timestamp) {
    time_t value = static_cast<time_t>(timestamp);
    struct tm utc = {};
    if (gmtime_r(&value, &utc) == nullptr) {
        return String();
    }

    char formatted[21];
    if (strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ",
                 &utc) == 0) {
        return String();
    }
    return String(formatted);
}

bool parseSuccessResponse(const String& responseBody) {
    JsonDocument response;
    DeserializationError error = deserializeJson(response, responseBody);
    if (error) {
        Serial.printf("Server sync error: invalid JSON response (%s)\n",
                      error.c_str());
        return false;
    }
    return response["success"].is<bool>() &&
           response["success"].as<bool>();
}

int postJson(const char* path,
             const String& requestBody,
             bool authenticated,
             String& responseBody) {
    setSyncDiagnosticStage(SYNC_STAGE_HTTPS_BEGIN);
    String serverHost(SERVER_BASE_URL);
    int schemeEnd = serverHost.indexOf("://");
    if (schemeEnd >= 0) {
        serverHost.remove(0, schemeEnd + 3);
    }
    int pathStart = serverHost.indexOf('/');
    if (pathStart >= 0) {
        serverHost.remove(pathStart);
    }
    int portStart = serverHost.indexOf(':');
    if (portStart >= 0) {
        serverHost.remove(portStart);
    }

    IPAddress serverAddress;
    if (!WiFi.hostByName(serverHost.c_str(), serverAddress)) {
        Serial.printf("Server sync transport: DNS lookup failed for %s\n",
                      serverHost.c_str());
    } else {
        Serial.printf("Server sync transport: %s resolved to %s\n",
                      serverHost.c_str(), serverAddress.toString().c_str());
        WiFiClient tcpProbe;
        bool tcpConnected =
            tcpProbe.connect(serverAddress, 443, HTTP_TIMEOUT_MS);
        Serial.printf("Server sync transport: TCP port 443 %s\n",
                      tcpConnected ? "reachable" : "unreachable");
        tcpProbe.stop();
    }

    WiFiClientSecure client;
    client.setCACert(SERVER_ROOT_CA);

    HTTPClient http;
    String url = String(SERVER_BASE_URL) + path;
    if (!http.begin(client, url)) {
        Serial.println("Server sync error: failed to initialize HTTPS");
        return -1;
    }
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    if (authenticated) {
        http.addHeader("X-Device-Id", getDeviceId());

        setSyncDiagnosticStage(SYNC_STAGE_HTTP_SIGNATURE);
        String signedMessage = String(getDeviceId()) + requestBody;
        uint8_t signature[MBEDTLS_ECDSA_MAX_LEN] = {};
        size_t signatureLength = 0;
        if (!signDeviceMessage(
                reinterpret_cast<const uint8_t*>(signedMessage.c_str()),
                signedMessage.length(), signature, sizeof(signature),
                signatureLength)) {
            http.end();
            return -1;
        }
        http.addHeader("X-Device-Signature",
                       toHex(signature, signatureLength));
    }

    setSyncDiagnosticStage(SYNC_STAGE_HTTP_POST);
    int status = http.POST(requestBody);
    setSyncDiagnosticStage(SYNC_STAGE_HTTP_RESPONSE);
    if (status > 0) {
        responseBody = http.getString();
        Serial.printf("Server sync: %s returned HTTP %d (%u response bytes)\n",
                      path, status,
                      static_cast<unsigned>(responseBody.length()));
    } else {
        Serial.printf("Server sync error: HTTPS request failed (%s)\n",
                      http.errorToString(status).c_str());
        char tlsError[128] = {};
        int tlsErrorCode = client.lastError(tlsError, sizeof(tlsError));
        Serial.printf("Server sync transport: TLS error %d (%s)\n",
                      tlsErrorCode,
                      tlsErrorCode == 0 ? "none" : tlsError);
    }
    http.end();
    return status;
}

bool activateDevice() {
    setSyncDiagnosticStage(SYNC_STAGE_ACTIVATE_KEY);
    size_t publicKeyLength = 0;
    const uint8_t* publicKey = getDevicePublicKey(publicKeyLength);
    if (!publicKey || publicKeyLength == 0) {
        Serial.println("Server sync error: device public key is unavailable");
        return false;
    }

    setSyncDiagnosticStage(SYNC_STAGE_ACTIVATE_JSON);
    JsonDocument request;
    request["device_id"] = getDeviceId();
    request["public_key"] = toHex(publicKey, publicKeyLength);

    String requestBody;
    serializeJson(request, requestBody);
    String responseBody;
    int status = postJson("/devices/activate", requestBody, false,
                          responseBody);
    if (status != HTTP_CODE_OK || !parseSuccessResponse(responseBody)) {
        Serial.printf("Server sync: device activation failed (HTTP %d)\n",
                      status);
        if (responseBody.length() > 0) {
            Serial.println(responseBody);
        }
        return false;
    }

    deviceActivated = true;
    Serial.println("Server sync: device activation complete");
    return true;
}

bool fetchDeviceProfile() {
    setSyncDiagnosticStage(SYNC_STAGE_DEVICE_PROFILE_GET);
    WiFiClientSecure client;
    client.setCACert(SERVER_ROOT_CA);

    HTTPClient http;
    String url = String(SERVER_BASE_URL) + "/devices/device";
    if (!http.begin(client, url)) {
        Serial.println("Server sync error: failed to initialize device-profile HTTPS");
        return false;
    }
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("X-Device-Id", getDeviceId());

    uint8_t signature[MBEDTLS_ECDSA_MAX_LEN] = {};
    size_t signatureLength = 0;
    const char* deviceId = getDeviceId();
    if (!signDeviceMessage(
            reinterpret_cast<const uint8_t*>(deviceId), strlen(deviceId),
            signature, sizeof(signature), signatureLength)) {
        http.end();
        Serial.println("Server sync error: failed to sign device-profile request");
        return false;
    }
    http.addHeader("X-Device-Signature", toHex(signature, signatureLength));

    setSyncDiagnosticStage(SYNC_STAGE_DEVICE_PROFILE_REQUEST);
    int status = http.GET();
    String responseBody;
    setSyncDiagnosticStage(SYNC_STAGE_HTTP_RESPONSE);
    if (status > 0) {
        responseBody = http.getString();
        Serial.printf("Server sync: GET /devices/device returned HTTP %d "
                      "(%u response bytes)\n",
                      status,
                      static_cast<unsigned>(responseBody.length()));
    } else {
        Serial.printf("Server sync error: device-profile request failed (%s)\n",
                      http.errorToString(status).c_str());
        http.end();
        return false;
    }
    http.end();

    if (status != HTTP_CODE_OK) {
        if (!responseBody.isEmpty()) {
            Serial.println(responseBody);
        }
        return false;
    }

    setSyncDiagnosticStage(SYNC_STAGE_DEVICE_PROFILE_PARSE);
    JsonDocument response;
    DeserializationError error = deserializeJson(response, responseBody);
    const char* name = response["name"].as<const char*>();
    if (error || !response.is<JsonObject>() || !name || !name[0]) {
        Serial.printf("Server sync error: invalid device-profile response (%s)\n",
                      error ? error.c_str() : "missing name");
        return false;
    }

    setLocalDeviceName(name);
    Serial.printf("BOOT: own device name: %s\n", name);
    return true;
}

bool fetchServerTradePool() {
    setSyncDiagnosticStage(SYNC_STAGE_TRADE_POOL_GET);
    WiFiClientSecure client;
    client.setCACert(SERVER_ROOT_CA);

    HTTPClient http;
    String url = String(SERVER_BASE_URL) + "/devices/trading_seals";
    if (!http.begin(client, url)) {
        Serial.println("Server sync error: failed to initialize trade-pool HTTPS");
        return false;
    }
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("X-Device-Id", getDeviceId());

    uint8_t signature[MBEDTLS_ECDSA_MAX_LEN] = {};
    size_t signatureLength = 0;
    const char* deviceId = getDeviceId();
    if (!signDeviceMessage(
            reinterpret_cast<const uint8_t*>(deviceId), strlen(deviceId),
            signature, sizeof(signature), signatureLength)) {
        http.end();
        Serial.println("Server sync error: failed to sign trade-pool request");
        return false;
    }
    http.addHeader("X-Device-Signature", toHex(signature, signatureLength));

    setSyncDiagnosticStage(SYNC_STAGE_TRADE_POOL_REQUEST);
    int status = http.GET();
    String responseBody;
    setSyncDiagnosticStage(SYNC_STAGE_HTTP_RESPONSE);
    if (status > 0) {
        responseBody = http.getString();
        Serial.printf("Server sync: GET /devices/trading_seals returned HTTP %d "
                      "(%u response bytes)\n",
                      status,
                      static_cast<unsigned>(responseBody.length()));
    } else {
        Serial.printf("Server sync error: trade-pool HTTPS request failed (%s)\n",
                      http.errorToString(status).c_str());
        char tlsError[128] = {};
        int tlsErrorCode = client.lastError(tlsError, sizeof(tlsError));
        Serial.printf("Server sync transport: trade-pool TLS error %d (%s)\n",
                      tlsErrorCode,
                      tlsErrorCode == 0 ? "none" : tlsError);
        http.end();
        return false;
    }
    http.end();

    if (status != HTTP_CODE_OK) {
        if (!responseBody.isEmpty()) {
            Serial.println(responseBody);
        }
        return false;
    }

    setSyncDiagnosticStage(SYNC_STAGE_TRADE_POOL_PARSE);
    JsonDocument response;
    DeserializationError error = deserializeJson(response, responseBody);
    if (error || !response.is<JsonArray>()) {
        Serial.printf("Server sync error: invalid trade-pool response (%s)\n",
                      error ? error.c_str() : "expected a JSON array");
        return false;
    }

    ServerTradePoolEntry entries[32] = {};
    size_t entryCount = 0;
    for (JsonVariantConst value : response.as<JsonArrayConst>()) {
        const char* sealId = value["seal_id"].as<const char*>();
        if (!sealId || !sealId[0] || strnlen(sealId, 37) >= 37) {
            Serial.println("Server sync error: trade-pool item has invalid seal_id");
            return false;
        }

        size_t index = 0;
        while (index < entryCount &&
               strcmp(entries[index].sealId, sealId) != 0) {
            ++index;
        }
        if (index == entryCount) {
            if (entryCount >= sizeof(entries) / sizeof(entries[0])) {
                Serial.println(
                    "Server sync error: trade pool has too many seal types");
                return false;
            }
            snprintf(entries[index].sealId, sizeof(entries[index].sealId),
                     "%s", sealId);
            entries[index].count = 0;
            ++entryCount;
        }
        if (entries[index].count == UINT16_MAX) {
            Serial.println(
                "Server sync error: trade-pool seal count overflow");
            return false;
        }
        ++entries[index].count;
    }

    if (!reconcileServerTradePool(entries, entryCount)) {
        Serial.println("Server sync error: failed to apply server trade pool");
        return false;
    }
    for (size_t i = 0; i < entryCount; ++i) {
        Serial.printf("Server sync: trade-pool seal_id=%s count=%u\n",
                      entries[i].sealId,
                      static_cast<unsigned>(entries[i].count));
    }
    Serial.printf("Server sync: loaded %u tradeable seal types from server\n",
                  static_cast<unsigned>(entryCount));
    return true;
}

bool isSyncableEvent(const LocalEvent& event) {
    return event.type == LOCAL_EVENT_ENCOUNTER ||
           event.type == LOCAL_EVENT_TRADE_COMPLETE;
}

bool isGatewayRewardReady(const LocalEvent& event) {
#if CONTEST_MODE
    (void)event;
    return true;
#else
    return !event.partnerIsGateway || !event.stickerId[0] ||
           event.gatewayRewardProcessed;
#endif
}

bool syncEvent(LocalEvent& event, uint32_t currentTimestampUnix) {
    setSyncDiagnosticStage(SYNC_STAGE_EVENT_TIMESTAMP);
    if (strcmp(event.originDeviceId, getDeviceId()) != 0) {
        Serial.printf("Server sync: ignoring event from unexpected device %s\n",
                      event.eventId);
        return false;
    }
    if (event.timestampUnix == 0) {
        Serial.printf("Server sync error: event %s has no timestamp\n",
                      event.eventId);
        return false;
    }

    String eventId = toLowerString(event.eventId);
    String myId = toLowerString(event.originDeviceId);
    String partnerId = toLowerString(event.partnerDeviceId);
    String sendSealId;
    String receiveSealId;

    if (event.type == LOCAL_EVENT_TRADE_COMPLETE) {
        sendSealId = toLowerString(event.sentStickerId);
        receiveSealId = toLowerString(event.receivedStickerId);
    }
#if !CONTEST_MODE
    else if (event.partnerIsGateway && event.stickerId[0] != '\0') {
        receiveSealId = toLowerString(event.stickerId);
    }
#endif

    String eventTimestamp = timestampToIso8601(event.timestampUnix);
    String requestTimestamp = timestampToIso8601(currentTimestampUnix);
    if (eventTimestamp.isEmpty() || requestTimestamp.isEmpty()) {
        Serial.println("Server sync error: failed to format event timestamp");
        return false;
    }

    String canonicalMessage = eventId + "|" + myId + "|" + partnerId + "|" +
        (event.partnerIsGateway ? "1" : "0") + "|" + sendSealId + "|" +
        receiveSealId + "|" + String(event.timestampUnix);
    setSyncDiagnosticStage(SYNC_STAGE_EVENT_SIGNATURE);
    uint8_t eventSignature[MBEDTLS_ECDSA_MAX_LEN] = {};
    size_t eventSignatureLength = 0;
    if (!signDeviceMessage(
            reinterpret_cast<const uint8_t*>(canonicalMessage.c_str()),
            canonicalMessage.length(), eventSignature,
            sizeof(eventSignature), eventSignatureLength)) {
        return false;
    }

    setSyncDiagnosticStage(SYNC_STAGE_EVENT_JSON);
    JsonDocument communication;
    communication["event_id"] = eventId;
    communication["my_id"] = myId;
    communication["partner_id"] = partnerId;
    communication["partner_name"] = event.partnerName;
    communication["partner_is_gateway"] = event.partnerIsGateway != 0;
    if (sendSealId.isEmpty()) {
        communication["send_seal_id"] = nullptr;
    } else {
        communication["send_seal_id"] = sendSealId;
    }
    if (receiveSealId.isEmpty()) {
        communication["receive_seal_id"] = nullptr;
    } else {
        communication["receive_seal_id"] = receiveSealId;
    }
    communication["timestamp"] = eventTimestamp;
    communication["signature"] = toHex(eventSignature,
                                        eventSignatureLength);

    JsonDocument request;
    request["device_id"] = getDeviceId();
    request["request_id"] = eventId;
    const float batteryPercent = getBatteryPercent();
    request["battery"] = batteryPercent;
    request["timestamp"] = requestTimestamp;
    request["nearby_communications"].add(communication);

    String requestBody;
    serializeJson(request, requestBody);
    String responseBody;
    int status = postJson("/devices/status", requestBody, true, responseBody);
    if (status != HTTP_CODE_OK || !parseSuccessResponse(responseBody)) {
        Serial.printf("Server sync: event %s failed (HTTP %d)\n",
                      event.eventId, status);
        if (responseBody.length() > 0) {
            Serial.println(responseBody);
        }
        return false;
    }

    setSyncDiagnosticStage(SYNC_STAGE_REMOVE_EVENT);
    if (!markLocalEventSynced(event.eventId)) {
        Serial.printf("Server sync error: accepted event %s could not be removed\n",
                      event.eventId);
        return false;
    }
    Serial.printf("Server sync: uploaded event %s\n", event.eventId);
    return true;
}

bool syncBatteryStatus(uint32_t currentTimestampUnix) {
    String timestamp = timestampToIso8601(currentTimestampUnix);
    if (timestamp.isEmpty()) {
        Serial.println("Server sync error: failed to format battery timestamp");
        return false;
    }

    JsonDocument request;
    request["device_id"] = getDeviceId();
    request["request_id"] = String(getDeviceId()) + "-" +
                            String(currentTimestampUnix);
    const float batteryPercent = getBatteryPercent();
    request["battery"] = batteryPercent;
    request["timestamp"] = timestamp;
    request["nearby_communications"].to<JsonArray>();

    String requestBody;
    serializeJson(request, requestBody);
    String responseBody;
    int status = postJson("/devices/status", requestBody, true, responseBody);
    if (status != HTTP_CODE_OK || !parseSuccessResponse(responseBody)) {
        Serial.printf("Server sync: battery update failed (HTTP %d)\n",
                      status);
        if (!responseBody.isEmpty()) {
            Serial.println(responseBody);
        }
        return false;
    }

    Serial.printf("Server sync: battery updated to %.1f%%\n",
                  batteryPercent);
    return true;
}
}

void logPreviousServerSyncDiagnostic() {
    if (syncDiagnosticMagic != SYNC_DIAGNOSTIC_MAGIC) {
        Serial.println("Server sync diagnostic: no previous sync phase recorded");
        return;
    }

    Serial.printf("Server sync diagnostic: previous reset occurred during '%s' (stage %u)\n",
                  syncStageName(syncDiagnosticStage),
                  static_cast<unsigned>(syncDiagnosticStage));
    syncDiagnosticMagic = 0;
    syncDiagnosticStage = SYNC_STAGE_IDLE;
    Serial.flush();
}

void processServerSync() {
#if !SERVER_API_HEX_FIELDS
    static bool warningLogged = false;
    if (!warningLogged) {
        Serial.println(
            "Server sync paused: enable SERVER_API_HEX_FIELDS after backend update");
        warningLogged = true;
    }
    return;
#endif

    bool wifiConnected = isWifiConnected();
    if (wifiConnected && !previousWifiConnected) {
        batteryUpdatePending = true;
        Serial.println("Server sync: Wi-Fi connected; battery update queued");
    }
    previousWifiConnected = wifiConnected;

    uint32_t nowMillis = millis();
    if (!syncScheduleInitialized) {
        lastSyncAttempt = nowMillis;
        syncScheduleInitialized = true;
        Serial.println("Server sync: waiting before first attempt");
        return;
    }
    if (nowMillis - lastSyncAttempt < SYNC_INTERVAL_MS) {
        return;
    }
    lastSyncAttempt = nowMillis;

    setSyncDiagnosticStage(SYNC_STAGE_CHECK_WIFI, false);
    if (!wifiConnected) {
        syncDiagnosticMagic = 0;
        syncDiagnosticStage = SYNC_STAGE_IDLE;
        return;
    }

    if (!deviceActivated && !activateDevice()) {
        syncDiagnosticMagic = 0;
        syncDiagnosticStage = SYNC_STAGE_IDLE;
        return;
    }

    uint32_t currentTimestampUnix = 0;
    setSyncDiagnosticStage(SYNC_STAGE_CHECK_TIME, false);
    if (!getTrustedUnixTime(currentTimestampUnix)) {
        syncDiagnosticMagic = 0;
        syncDiagnosticStage = SYNC_STAGE_IDLE;
        return;
    }
    if (batteryUpdatePending &&
        syncBatteryStatus(currentTimestampUnix)) {
        batteryUpdatePending = false;
    }

    if (!deviceProfileRefreshAttempted ||
        nowMillis - lastDeviceProfileRefreshAttempt >=
            TRADE_POOL_REFRESH_INTERVAL_MS) {
        deviceProfileRefreshAttempted = true;
        lastDeviceProfileRefreshAttempt = nowMillis;
        fetchDeviceProfile();
    }
    if (!tradePoolRefreshAttempted ||
        nowMillis - lastTradePoolRefreshAttempt >=
            TRADE_POOL_REFRESH_INTERVAL_MS) {
        tradePoolRefreshAttempted = true;
        lastTradePoolRefreshAttempt = nowMillis;
        if (!fetchServerTradePool()) {
            syncDiagnosticMagic = 0;
            syncDiagnosticStage = SYNC_STAGE_IDLE;
            return;
        }
    }

    setSyncDiagnosticStage(SYNC_STAGE_FIND_EVENT);
    LocalEvent pendingEvent = {};
    bool hasSyncableEvent = false;
    bool hasUnresolvableTimestamp = false;
    size_t eventCount = getPendingLocalEventCount();
    for (size_t i = 0; i < eventCount; ++i) {
        LocalEvent candidate = {};
        if (getPendingLocalEvent(i, candidate) &&
            isSyncableEvent(candidate) &&
            isGatewayRewardReady(candidate) &&
            strcmp(candidate.originDeviceId, getDeviceId()) == 0) {
            if (!prepareLocalEventTimestamp(
                    candidate, currentTimestampUnix, nowMillis)) {
                hasUnresolvableTimestamp = true;
                continue;
            }
            pendingEvent = candidate;
            hasSyncableEvent = true;
            break;
        }
    }
    if (!hasSyncableEvent) {
        if (hasUnresolvableTimestamp) {
            Serial.println(
                "Server sync: old events lack a trustworthy timestamp");
        }
        syncDiagnosticMagic = 0;
        syncDiagnosticStage = SYNC_STAGE_IDLE;
        return;
    }

    if (hasSyncableEvent) {
        if (syncEvent(pendingEvent, currentTimestampUnix)) {
            batteryUpdatePending = false;
        }
    }
    syncDiagnosticMagic = 0;
    syncDiagnosticStage = SYNC_STAGE_IDLE;
}
