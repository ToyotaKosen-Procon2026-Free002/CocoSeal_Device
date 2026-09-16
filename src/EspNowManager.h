#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// すれ違い通信用のデータ型
typedef struct struct_message {
    char device_id[16];     // 自分のデバイスID
    bool is_parent;         // 親機ならtrue、子機ならfalse
    bool has_sticker;       // シールを持っているか？
    char sticker_id[16];    // 渡すシールのID
} struct_message;

// このデバイスが親機かどうか（親機ではtrueに設定する）
extern bool isParentDevice;

// main.cpp用のフラグ関数
extern bool encounterFlag;
extern char displayStickerId[16];
extern bool getSticker;
extern bool isRareSticker;

// 関数リスト
void setupEspNow();
void sendDummySticker();