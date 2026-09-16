#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "EspNowManager.h"

extern Adafruit_SSD1306 display;
extern SemaphoreHandle_t lcdMutex;

// 画面表示用の関数リスト
void displaySOS(int count);
void displaySending();
void displayBattery(float percent);
void displayEncounter(bool gotSticker, bool isRare, const char* stickerId);
void displayEspNowStatus(EspNowStatus status);