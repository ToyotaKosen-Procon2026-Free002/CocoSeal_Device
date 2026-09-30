#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

#include <stddef.h>

// Must remain layout-compatible with CommunicationPacket in the M5GO parent.
struct CommunicationPacket {
    char device_id[37];
    int type; // 0: encounter/sticker request, 1: SOS
    char stickerId[16];
    bool isGateway;
};

static_assert(sizeof(int) == 4, "Parent protocol requires 32-bit int");
static_assert(offsetof(CommunicationPacket, type) == 40,
              "CommunicationPacket layout mismatch");
static_assert(sizeof(CommunicationPacket) == 64,
              "CommunicationPacket layout mismatch");

enum EspNowStatus {
    ESP_NOW_WAITING,
    ESP_NOW_SENDING,
    ESP_NOW_RECEIVED,
    ESP_NOW_ESTABLISHED,
    ESP_NOW_SEND_FAILED
};

extern volatile EspNowStatus espNowStatus;

// main.cpp用のフラグ関数
extern volatile bool encounterFlag;
extern volatile bool sosReceivedEspNow;
extern char displayStickerId[16];
extern bool getSticker;
extern bool isRareSticker;
extern volatile bool lastEncounterWasParent;

// 関数リスト
void setupEspNow();
void sendDummySticker();
void sendSosNotification();
void setEspNowStatus(EspNowStatus status);