#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "EspNowManager.h"

extern Adafruit_SSD1306 display;
extern SemaphoreHandle_t lcdMutex;

enum EncounterSource {
    ENCOUNTER_SOURCE_CHILD,
    ENCOUNTER_SOURCE_PARENT
};

// 画面表示用の関数リスト
void initializeJapaneseDisplay();
void displaySOSPressCount(int count);
void displaySOSAlert();
void displaySOSReceived();
void displayBattery(float percent);
void displayEncounter(EncounterSource source, const char* peerName);
void displayParentEncounterReward();
void displayTradeDebugStatus(const char* status);