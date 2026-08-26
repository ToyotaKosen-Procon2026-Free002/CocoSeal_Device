#include "DisplayManager.h"

// SOSボタンを押したときの表示
void displaySOS(int count) {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(2);
        display.println("SOS");

        display.setTextSize(1);
        display.printf("Count: %d / 3\n", count);
        display.display();
        xSemaphoreGive(lcdMutex);
    }
}

// 送信中の表示
void displaySending() {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(2);
        display.println("Sending...");
        display.display();
        xSemaphoreGive(lcdMutex);
    }
}

// バッテリー残量の表示
void displayBattery(float percent) {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(1);
        display.printf("Battery: %.1f%%\n", percent);
        display.display();
        xSemaphoreGive(lcdMutex);
    }
}