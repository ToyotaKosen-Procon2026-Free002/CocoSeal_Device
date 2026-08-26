#include "EspNowManager.h"

#define COOL_DOWN_TIME 30000 // クールタイム30秒

// すれ違い履歴の構造体
typedef struct {
    uint8_t macAddr[6];
    unsigned long lastTradeTime;
} EncounterHistory;
EncounterHistory recent_history[10];

bool encounterFlag = false;
char displayStickerId[16] = "";
bool getSticker = false;

struct_message myData;
struct_message peerData;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;

// データを受信したときの処理
void onEspNowRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
    if (data_len != sizeof(struct_message)) {
        return; // データサイズが異なる場合は無視
    }

    memcpy(&peerData, data, sizeof(peerData));
    unsigned long currentMillis = millis();
    bool canTrade = true;

    // すれ違い履歴を確認
    for (int i = 0; i < 10; i++) {
        if (memcmp(recent_history[i].macAddr, mac_addr, 6) == 0) {
            if (currentMillis - recent_history[i].lastTradeTime < COOL_DOWN_TIME) {
                canTrade = false; // クールタイム中
            }
            break;
        }
    }

    // 交換可能ならフラグを立てる
    if (canTrade) {
        memcpy(recent_history[0].macAddr, mac_addr, 6);
        recent_history[0].lastTradeTime = currentMillis;

        if (peerData.has_sticker) {
            getSticker = true;
            strcpy(displayStickerId, peerData.sticker_id);
        } else {
            getSticker = false;
        }
        encounterFlag = true;
    }
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
}

// 定期的に周りに呼びかける関数
void sendDummySticker() {
    strcpy(myData.device_id, "ESP-0001");
    myData.has_sticker = true;
    strcpy(myData.sticker_id, "st_005");
    esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
}