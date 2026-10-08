#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

#include <stddef.h>

// Must remain layout-compatible with CommunicationPacket in the M5GO parent.
struct CommunicationPacket {
    char device_id[37];
    int type; // 0: encounter/sticker request, 1: SOS, 2: name packet layout
    char stickerId[16];
    bool isGateway;
};

struct NameAnnouncementPacket {
    char device_id[37];
    int type;
    char name[20];
};

static_assert(sizeof(int) == 4, "Parent protocol requires 32-bit int");
static_assert(offsetof(CommunicationPacket, type) == 40,
              "CommunicationPacket layout mismatch");
static_assert(offsetof(CommunicationPacket, stickerId) == 44,
              "Parent sticker packet layout mismatch");
static_assert(offsetof(CommunicationPacket, isGateway) == 60,
              "Parent gateway flag layout mismatch");
static_assert(sizeof(CommunicationPacket) == 64,
              "CommunicationPacket layout mismatch");
static_assert(offsetof(NameAnnouncementPacket, type) == 40,
              "Name announcement protocol layout mismatch");
static_assert(offsetof(NameAnnouncementPacket, name) == 44,
              "Name announcement protocol layout mismatch");
static_assert(sizeof(NameAnnouncementPacket) == 64,
              "Name announcement must remain compatible with 64-byte ESP-NOW frames");

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
extern char displayPeerDeviceId[37];
extern char displayPeerName[20];
extern bool getSticker;
extern bool isRareSticker;
extern volatile bool lastEncounterWasParent;

// 関数リスト
void setupEspNow();
void setLocalDeviceName(const char* name);
const char* getPeerDeviceName(const char* deviceId);
void scheduleSynchronizedEncounterDisplay(const char* peerDeviceId);
bool isSynchronizedEncounterDisplayPending();
void processEspNowDisplayEvents();
void sendEncounterAnnouncement();
void sendSosNotification();
void setEspNowStatus(EspNowStatus status);