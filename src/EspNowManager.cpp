#include "EspNowManager.h"

#define COOL_DOWN_TIME 30000
#define ENCOUNTER_HISTORY_SIZE 10
#define ESP_NOW_CHANNEL 1
#define MESSAGE_TYPE_ENCOUNTER 0
#define MESSAGE_TYPE_SOS 1

namespace {
struct EncounterHistory {
    uint8_t macAddr[6];
    unsigned long lastTradeTime;
    bool occupied;
};

EncounterHistory recentHistory[ENCOUNTER_HISTORY_SIZE] = {};
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(packet.device_id, sizeof(packet.device_id),
             "ESP-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    packet.type = type;
    snprintf(packet.stickerId, sizeof(packet.stickerId), "%s", "st_005");
    packet.isGateway = false;
    return packet;
}

bool sendPacket(const CommunicationPacket& packet) {
    esp_err_t result = esp_now_send(
        broadcastAddress,
        reinterpret_cast<const uint8_t *>(&packet),
        sizeof(packet));
    if (result != ESP_OK) {
        Serial.printf("ESP-NOW send failed: %d\n", result);
        setEspNowStatus(ESP_NOW_SEND_FAILED);
        return false;
    }
    return true;
}

void onEspNowRecv(const uint8_t *macAddr, const uint8_t *data, int dataLen) {
    if (dataLen != sizeof(CommunicationPacket)) {
        Serial.printf("Ignoring incompatible ESP-NOW packet: %d bytes\n",
                      dataLen);
        return;
    }

    CommunicationPacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    packet.device_id[sizeof(packet.device_id) - 1] = '\0';
    packet.stickerId[sizeof(packet.stickerId) - 1] = '\0';

    if (packet.type == MESSAGE_TYPE_SOS) {
        if (!packet.isGateway) {
            Serial.printf("SOS received from %s\n", packet.device_id);
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

    getSticker = packet.stickerId[0] != '\0';
    snprintf(displayStickerId, sizeof(displayStickerId), "%s",
             packet.stickerId);
    lastEncounterWasParent = packet.isGateway;
    // The parent packet does not contain a rarity field.
    isRareSticker = false;
    setEspNowStatus(ESP_NOW_ESTABLISHED);
    encounterFlag = true;

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
bool getSticker = false;
bool isRareSticker = false;
volatile bool lastEncounterWasParent = false;
volatile EspNowStatus espNowStatus = ESP_NOW_WAITING;

void setEspNowStatus(EspNowStatus status) {
    espNowStatus = status;
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
    peer.channel = ESP_NOW_CHANNEL;
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
    Serial.printf("ESP-NOW ready on channel %d; packet size %u bytes\n",
                  ESP_NOW_CHANNEL,
                  static_cast<unsigned>(sizeof(CommunicationPacket)));
}

void sendDummySticker() {
    CommunicationPacket packet = makePacket(MESSAGE_TYPE_ENCOUNTER);
    setEspNowStatus(ESP_NOW_SENDING);
    sendPacket(packet);
}

void sendSosNotification() {
    CommunicationPacket packet = makePacket(MESSAGE_TYPE_SOS);
    sendPacket(packet);
}
