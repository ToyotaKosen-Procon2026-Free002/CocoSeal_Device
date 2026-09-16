#include "EspNowManager.h"

#define COOL_DOWN_TIME 30000 // クールタイム30秒

// すれ違い履歴の構造体
typedef struct {
    uint8_t macAddr[6];
    unsigned long lastTradeTime;
} EncounterHistory;
EncounterHistory recent_history[10];

volatile bool encounterFlag = false;
volatile bool sosReceivedEspNow = false;
char displayStickerId[16] = "";
bool getSticker = false;
bool isRareSticker = false;
bool isParentDevice = false;
volatile EspNowStatus espNowStatus = ESP_NOW_WAITING;

struct_message myData;
struct_message peerData;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;
uint32_t nextTransactionId = 1;
uint32_t pendingTransactionId = 0;
uint8_t pendingPeerMac[6] = {};

bool isInCooldown(const uint8_t *mac_addr, unsigned long currentMillis) {
    for (int i = 0; i < 10; i++) {
        if (memcmp(recent_history[i].macAddr, mac_addr, 6) == 0) {
            return currentMillis - recent_history[i].lastTradeTime < COOL_DOWN_TIME;
        }
    }
    return false;
}

void completeExchange(const struct_message& receivedData,
                      const uint8_t *mac_addr,
                      unsigned long currentMillis) {
    if (isInCooldown(mac_addr, currentMillis)) {
        return;
    }

    memcpy(recent_history[0].macAddr, mac_addr, 6);
    recent_history[0].lastTradeTime = currentMillis;

    if (receivedData.has_sticker) {
        getSticker = true;
        strcpy(displayStickerId, receivedData.sticker_id);
        isRareSticker = receivedData.is_parent;
    } else {
        getSticker = false;
        isRareSticker = false;
    }

    setEspNowStatus(ESP_NOW_ESTABLISHED);
    encounterFlag = true;
}

bool isPendingPeer(const uint8_t *mac_addr, uint32_t transactionId) {
    bool isBroadcastPeer = memcmp(pendingPeerMac, broadcastAddress, 6) == 0;
    return pendingTransactionId == transactionId &&
           (isBroadcastPeer || memcmp(pendingPeerMac, mac_addr, 6) == 0);
}

bool sendMessage(const struct_message& message, const uint8_t *mac_addr) {
    return esp_now_send(mac_addr, (uint8_t *)&message, sizeof(message)) == ESP_OK;
}

struct_message makeMessage(EspNowMessageType messageType, uint32_t transactionId) {
    struct_message message = {};
    strcpy(message.device_id, "ESP-0001");
    message.is_parent = isParentDevice;
    message.has_sticker = true;
    strcpy(message.sticker_id, "st_005");
    message.transaction_id = transactionId;
    message.message_type = messageType;
    return message;
}

// データを受信したときの処理
void onEspNowRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
    if (data_len != sizeof(struct_message)) {
        return; // データサイズが異なる場合は無視
    }

    memcpy(&peerData, data, sizeof(peerData));
    setEspNowStatus(ESP_NOW_RECEIVED);

    // 受信した相手をESP-NOWの送信先として登録する
    if (!esp_now_is_peer_exist(mac_addr)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac_addr, 6);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            return;
        }
    }

    unsigned long currentMillis = millis();
    if (peerData.message_type == ESP_NOW_SOS) {
        sosReceivedEspNow = true;
        return;
    }
    if (isInCooldown(mac_addr, currentMillis)) {
        return;
    }

    switch (peerData.message_type) {
        case ESP_NOW_OFFER: {
            struct_message accept = makeMessage(ESP_NOW_ACCEPT, peerData.transaction_id);
            sendMessage(accept, mac_addr);
            break;
        }
        case ESP_NOW_ACCEPT: {
            if (isPendingPeer(mac_addr, peerData.transaction_id)) {
                memcpy(pendingPeerMac, mac_addr, 6);
                struct_message commit = makeMessage(ESP_NOW_COMMIT, peerData.transaction_id);
                sendMessage(commit, mac_addr);
            }
            break;
        }
        case ESP_NOW_COMMIT: {
            completeExchange(peerData, mac_addr, currentMillis);
            struct_message commitAck = makeMessage(ESP_NOW_COMMIT_ACK, peerData.transaction_id);
            sendMessage(commitAck, mac_addr);
            break;
        }
        case ESP_NOW_COMMIT_ACK:
            if (isPendingPeer(mac_addr, peerData.transaction_id)) {
                completeExchange(peerData, mac_addr, currentMillis);
            }
            break;
        case ESP_NOW_SOS:
            break;
    }
}

void onEspNowSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    setEspNowStatus(status == ESP_NOW_SEND_SUCCESS
                        ? ESP_NOW_SENDING
                        : ESP_NOW_SEND_FAILED);
}

void setEspNowStatus(EspNowStatus status) {
    espNowStatus = status;
}

// 起動時の初期設定
void setupEspNow() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    if (esp_now_init() != ESP_OK) return;

    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_register_send_cb(onEspNowSent);
}

// 定期的に周りに呼びかける関数
void sendDummySticker() {
    setEspNowStatus(ESP_NOW_SENDING);
    pendingTransactionId = nextTransactionId++;
    myData = makeMessage(ESP_NOW_OFFER, pendingTransactionId);
    memcpy(pendingPeerMac, broadcastAddress, 6);
    esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
}

void sendSosNotification() {
    struct_message sosMessage = makeMessage(ESP_NOW_SOS, nextTransactionId++);
    esp_now_send(broadcastAddress, (uint8_t *)&sosMessage, sizeof(sosMessage));
}