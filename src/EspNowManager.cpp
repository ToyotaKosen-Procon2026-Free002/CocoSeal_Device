#include "EspNowManager.h"
#include "DeviceIdentity.h"
#include "LocalDatabase.h"
#include "TradeProtocol.h"
#include "WifiManager.h"

#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/ecdsa.h>
#include <time.h>

#define COOL_DOWN_TIME 30000
#define ENCOUNTER_HISTORY_SIZE 10
#define MESSAGE_TYPE_ENCOUNTER 0
#define MESSAGE_TYPE_SOS 1
#define MESSAGE_TYPE_NAME_ANNOUNCEMENT 2
#define PEER_NAME_CACHE_SIZE 10
#define SYNCHRONIZED_DISPLAY_DELAY_MS 300
#define PARENT_NAME_DISPLAY_DELAY_MS 300

namespace {
struct EncounterHistory {
    uint8_t macAddr[6];
    unsigned long lastTradeTime;
    bool occupied;
};

struct PeerName {
    char deviceId[37];
    char name[20];
};

EncounterHistory recentHistory[ENCOUNTER_HISTORY_SIZE] = {};
PeerName peerNames[PEER_NAME_CACHE_SIZE] = {};
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
char localDeviceName[20] = "";
bool localDeviceNameAvailable = false;
char scheduledPeerDeviceId[37] = "";
uint32_t synchronizedDisplayAt = 0;
bool synchronizedDisplayPending = false;
char scheduledParentDeviceId[37] = "";
uint32_t parentNameDisplayAt = 0;
bool parentNameDisplayPending = false;

size_t utf8PrefixLength(const char* text, size_t capacity) {
    if (!text || capacity == 0) {
        return 0;
    }
    size_t length = strnlen(text, capacity);
    size_t limit = length < capacity ? length : capacity - 1;
    size_t validLength = 0;
    while (validLength < limit) {
        uint8_t lead = static_cast<uint8_t>(text[validLength]);
        size_t codePointLength = 0;
        if (lead <= 0x7F) {
            codePointLength = 1;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            codePointLength = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            codePointLength = 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            codePointLength = 4;
        } else {
            break;
        }
        if (codePointLength > limit - validLength) {
            break;
        }

        bool validCodePoint = true;
        for (size_t i = 1; i < codePointLength; ++i) {
            if ((static_cast<uint8_t>(text[validLength + i]) & 0xC0) !=
                0x80) {
                validCodePoint = false;
                break;
            }
        }
        if (!validCodePoint) {
            break;
        }

        uint8_t second = static_cast<uint8_t>(text[validLength + 1]);
        if ((lead == 0xE0 && second < 0xA0) ||
            (lead == 0xED && second > 0x9F) ||
            (lead == 0xF0 && second < 0x90) ||
            (lead == 0xF4 && second > 0x8F)) {
            break;
        }
        validLength += codePointLength;
    }
    return validLength;
}

void copyUtf8(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) {
        return;
    }
    destination[0] = '\0';
    if (!source || !source[0]) {
        return;
    }
    size_t length = utf8PrefixLength(source, capacity);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

void rememberPeerName(const char* deviceId, const char* name) {
    if (!deviceId || !deviceId[0] || !name || !name[0]) {
        return;
    }

    PeerName* slot = nullptr;
    for (PeerName& peer : peerNames) {
        if (strcmp(peer.deviceId, deviceId) == 0) {
            slot = &peer;
            break;
        }
        if (!slot && !peer.deviceId[0]) {
            slot = &peer;
        }
    }
    if (!slot) {
        slot = &peerNames[0];
    }
    snprintf(slot->deviceId, sizeof(slot->deviceId), "%s", deviceId);
    copyUtf8(slot->name, sizeof(slot->name), name);
}

const char* findPeerName(const char* deviceId) {
    for (const PeerName& peer : peerNames) {
        if (peer.deviceId[0] && strcmp(peer.deviceId, deviceId) == 0) {
            return peer.name;
        }
    }
    return "";
}

bool isInCooldown(const uint8_t *macAddr, unsigned long now) {
    for (const EncounterHistory& history : recentHistory) {
        if (history.occupied &&
            memcmp(history.macAddr, macAddr, sizeof(history.macAddr)) == 0 &&
            now - history.lastTradeTime < COOL_DOWN_TIME) {
            return true;
        }
    }
    return false;
}

void recordEncounter(const uint8_t *macAddr, unsigned long now) {
    EncounterHistory *slot = nullptr;
    for (EncounterHistory& history : recentHistory) {
        if (history.occupied &&
            memcmp(history.macAddr, macAddr, sizeof(history.macAddr)) == 0) {
            slot = &history;
            break;
        }
        if (!history.occupied) {
            slot = &history;
        }
    }

    if (slot == nullptr) {
        slot = &recentHistory[0];
        for (EncounterHistory& history : recentHistory) {
            if (now - history.lastTradeTime >
                now - slot->lastTradeTime) {
                slot = &history;
            }
        }
    }

    memcpy(slot->macAddr, macAddr, sizeof(slot->macAddr));
    slot->lastTradeTime = now;
    slot->occupied = true;
}

CommunicationPacket makePacket(int type) {
    CommunicationPacket packet = {};
    snprintf(packet.device_id, sizeof(packet.device_id), "%s", getDeviceId());
    packet.type = type;
    packet.isGateway = false;
    return packet;
}

bool sendPacket(const CommunicationPacket& packet) {
    esp_err_t result = esp_now_send(
        broadcastAddress, reinterpret_cast<const uint8_t*>(&packet),
        sizeof(packet));
    if (result != ESP_OK) {
        Serial.printf("ESP-NOW send failed: %d\n", result);
        setEspNowStatus(ESP_NOW_SEND_FAILED);
        return false;
    }
    return true;
}

void generateEventId(char* output, size_t capacity) {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    snprintf(output, capacity,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool sendNameAnnouncement() {
    if (!localDeviceNameAvailable) {
        return false;
    }
    NameAnnouncementPacket packet = {};
    snprintf(packet.device_id, sizeof(packet.device_id), "%s", getDeviceId());
    packet.type = MESSAGE_TYPE_NAME_ANNOUNCEMENT;
    snprintf(packet.name, sizeof(packet.name), "%s", localDeviceName);
    esp_err_t result = esp_now_send(
        broadcastAddress, reinterpret_cast<const uint8_t*>(&packet),
        sizeof(packet));
    if (result != ESP_OK) {
        Serial.printf("ESP-NOW name announcement failed: %d\n", result);
        return false;
    }
    return true;
}

void onEspNowRecv(const uint8_t *macAddr, const uint8_t *data, int dataLen) {
    if (receiveTradeProtocolPacket(
            macAddr, data, static_cast<size_t>(dataLen))) {
        return;
    }
    if (dataLen != sizeof(CommunicationPacket)) {
        Serial.printf("Ignoring unknown ESP-NOW packet: %d bytes\n",
                      dataLen);
        return;
    }

    int messageType = -1;
    memcpy(&messageType, data + offsetof(CommunicationPacket, type),
           sizeof(messageType));
    if (messageType == MESSAGE_TYPE_NAME_ANNOUNCEMENT) {
        NameAnnouncementPacket namePacket = {};
        memcpy(&namePacket, data, sizeof(namePacket));
        namePacket.device_id[sizeof(namePacket.device_id) - 1] = '\0';
        namePacket.name[sizeof(namePacket.name) - 1] = '\0';
        if (namePacket.device_id[0] && namePacket.name[0]) {
            rememberPeerName(namePacket.device_id, namePacket.name);
            Serial.printf("ESP-NOW peer name received: %s\n",
                          namePacket.name);
        }
        return;
    }

    CommunicationPacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    packet.device_id[sizeof(packet.device_id) - 1] = '\0';
    packet.stickerId[sizeof(packet.stickerId) - 1] = '\0';

    if (packet.type == MESSAGE_TYPE_SOS) {
        if (!packet.isGateway) {
            Serial.printf("SOS received from %s\n", packet.device_id);
            if (!queueSosEvent(LOCAL_EVENT_SOS_RECEIVED, packet.device_id,
                               getDeviceId())) {
                Serial.println("Warning: received SOS is not queued locally");
            }
            sosReceivedEspNow = true;
        }
        return;
    }

    if (packet.type != MESSAGE_TYPE_ENCOUNTER) {
        Serial.printf("Ignoring unknown ESP-NOW message type: %d\n",
                      packet.type);
        return;
    }

    unsigned long now = millis();
    if (isInCooldown(macAddr, now)) {
        return;
    }
    recordEncounter(macAddr, now);

    if (!queueEncounterEvent(packet.device_id, findPeerName(packet.device_id),
                             packet.isGateway,
                             packet.stickerId)) {
        Serial.println("Warning: encounter is not queued locally");
    }
    if (!packet.isGateway) {
        notifyTradePeerEncounter(macAddr, packet.device_id);
    }
    getSticker = packet.stickerId[0] != '\0';
    snprintf(displayStickerId, sizeof(displayStickerId), "%s",
             packet.stickerId);
    snprintf(displayPeerDeviceId, sizeof(displayPeerDeviceId), "%s",
             packet.device_id[0] ? packet.device_id : "unknown");
    copyUtf8(displayPeerName, sizeof(displayPeerName),
             findPeerName(packet.device_id));
    lastEncounterWasParent = packet.isGateway;
    // The parent packet does not contain a rarity field.
    isRareSticker = false;
    setEspNowStatus(ESP_NOW_ESTABLISHED);
    if (packet.isGateway) {
        snprintf(scheduledParentDeviceId,
                 sizeof(scheduledParentDeviceId), "%s",
                 displayPeerDeviceId);
        parentNameDisplayAt = millis() + PARENT_NAME_DISPLAY_DELAY_MS;
        parentNameDisplayPending = true;
    }

    Serial.printf("Encounter from %s (%s), sticker: %s\n",
                  packet.device_id,
                  packet.isGateway ? "gateway" : "child",
                  packet.stickerId);
}

void onEspNowSent(const uint8_t *, esp_now_send_status_t status) {
    setEspNowStatus(status == ESP_NOW_SEND_SUCCESS
                        ? ESP_NOW_SENDING
                        : ESP_NOW_SEND_FAILED);
}
}

volatile bool encounterFlag = false;
volatile bool sosReceivedEspNow = false;
char displayStickerId[16] = "";
char displayPeerDeviceId[37] = "";
char displayPeerName[20] = "";
bool getSticker = false;
bool isRareSticker = false;
volatile bool lastEncounterWasParent = false;
volatile EspNowStatus espNowStatus = ESP_NOW_WAITING;

void setEspNowStatus(EspNowStatus status) {
    espNowStatus = status;
}

void setLocalDeviceName(const char* name) {
    if (!name || !name[0]) {
        Serial.println("ESP-NOW warning: ignored empty local device name");
        return;
    }
    copyUtf8(localDeviceName, sizeof(localDeviceName), name);
    localDeviceNameAvailable = true;
    Serial.printf("ESP-NOW local name updated: %s\n", localDeviceName);
}

const char* getPeerDeviceName(const char* deviceId) {
    return findPeerName(deviceId);
}

void scheduleSynchronizedEncounterDisplay(const char* peerDeviceId) {
    if (!peerDeviceId || !peerDeviceId[0]) {
        return;
    }
    snprintf(scheduledPeerDeviceId, sizeof(scheduledPeerDeviceId), "%s",
             peerDeviceId);
    synchronizedDisplayAt = millis() + SYNCHRONIZED_DISPLAY_DELAY_MS;
    synchronizedDisplayPending = true;
}

bool isSynchronizedEncounterDisplayPending() {
    return synchronizedDisplayPending || parentNameDisplayPending;
}

void processEspNowDisplayEvents() {
    if (parentNameDisplayPending &&
        static_cast<int32_t>(millis() - parentNameDisplayAt) >= 0) {
        parentNameDisplayPending = false;
        snprintf(displayPeerDeviceId, sizeof(displayPeerDeviceId), "%s",
                 scheduledParentDeviceId);
        copyUtf8(displayPeerName, sizeof(displayPeerName),
                 findPeerName(scheduledParentDeviceId));
        lastEncounterWasParent = true;
        encounterFlag = true;
    }

    if (!synchronizedDisplayPending ||
        static_cast<int32_t>(millis() - synchronizedDisplayAt) < 0) {
        return;
    }
    synchronizedDisplayPending = false;
    snprintf(displayPeerDeviceId, sizeof(displayPeerDeviceId), "%s",
             scheduledPeerDeviceId);
    copyUtf8(displayPeerName, sizeof(displayPeerName),
             findPeerName(scheduledPeerDeviceId));
    lastEncounterWasParent = false;
    encounterFlag = true;
}

void setupEspNow() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    esp_err_t result = esp_now_init();
    if (result != ESP_OK) {
        Serial.printf("ESP-NOW initialization failed: %d\n", result);
        return;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcastAddress, sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(broadcastAddress)) {
        result = esp_now_add_peer(&peer);
        if (result != ESP_OK) {
            Serial.printf("ESP-NOW broadcast peer setup failed: %d\n", result);
            return;
        }
    }

    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_register_send_cb(onEspNowSent);
    Serial.printf("ESP-NOW ready on radio channel %u; peer channel auto\n",
                  WiFi.channel());
    Serial.printf("ESP-NOW packet size %u bytes\n",
                  static_cast<unsigned>(sizeof(CommunicationPacket)));
}

void sendEncounterAnnouncement() {
    sendNameAnnouncement();
    CommunicationPacket packet = makePacket(MESSAGE_TYPE_ENCOUNTER);
    setEspNowStatus(ESP_NOW_SENDING);
    sendPacket(packet);
}

void sendSosNotification() {
    CommunicationPacket packet = makePacket(MESSAGE_TYPE_SOS);
    Serial.println(
        "Sending Wi-Fi-independent SOS alert over ESP-NOW (64-byte packet)");
    setEspNowStatus(ESP_NOW_SENDING);
    sendPacket(packet);
}
