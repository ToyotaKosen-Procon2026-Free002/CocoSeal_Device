#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

enum EspNowMessageType {
    ESP_NOW_OFFER,
    ESP_NOW_ACCEPT,
    ESP_NOW_COMMIT,
    ESP_NOW_COMMIT_ACK
};

// すれ違い通信用のデータ型
typedef struct struct_message {
    char device_id[16];     // 自分のデバイスID
    bool is_parent;         // 親機ならtrue、子機ならfalse
    bool has_sticker;       // シールを持っているか？
    char sticker_id[16];    // 渡すシールのID
    uint32_t transaction_id;
    EspNowMessageType message_type;
} struct_message;

enum EspNowStatus {
    ESP_NOW_WAITING,
    ESP_NOW_SENDING,
    ESP_NOW_RECEIVED,
    ESP_NOW_ESTABLISHED,
    ESP_NOW_SEND_FAILED
};

// このデバイスが親機かどうか（親機ではtrueに設定する）
extern bool isParentDevice;
extern volatile EspNowStatus espNowStatus;

// main.cpp用のフラグ関数
extern bool encounterFlag;
extern char displayStickerId[16];
extern bool getSticker;
extern bool isRareSticker;

// 関数リスト
void setupEspNow();
void sendDummySticker();
void setEspNowStatus(EspNowStatus status);