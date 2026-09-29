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

void displaySOSAlert() {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(2);
        display.println("SOS ALERT");
        display.setTextSize(1);
        display.println("Press reset");
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

// すれ違い結果の表示
void displayEncounter(bool gotSticker, bool isRare, const char* stickerId) {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(2);

        if (gotSticker) {
            if (isRare) {
                display.println("RARE GET!!");
            } else {
                display.println("Sticker GET!");
            }
            display.setTextSize(1);
            display.printf("ID: %s\n", stickerId);
        }

        display.display();
        xSemaphoreGive(lcdMutex);
    }
}

void displayCommunicationTestStatus(bool wifiConnected,
                                    uint8_t radioChannel,
                                    uint8_t espNowChannel,
                                    uint32_t txSuccessCount,
                                    uint32_t txFailureCount,
                                    uint32_t rxCount,
                                    uint32_t invalidRxCount,
                                    int lastRxType,
                                    bool lastRxIsGateway) {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("COMMUNICATION TEST");
        display.printf("WiFi: %s\n", wifiConnected ? "CONNECTED" : "OFFLINE");
        display.printf("RADIO CH:%u PEER CH:%u\n",
                       radioChannel, espNowChannel);

        if (radioChannel != 0 && radioChannel != espNowChannel) {
            display.println("CHANNEL MISMATCH!");
        } else {
            display.println(radioChannel == 0 ? "CHANNEL UNKNOWN"
                                               : "CHANNEL OK");
        }

        display.printf("TX OK:%lu FAIL:%lu\n",
                       static_cast<unsigned long>(txSuccessCount),
                       static_cast<unsigned long>(txFailureCount));
        display.printf("RX OK:%lu BAD:%lu\n",
                       static_cast<unsigned long>(rxCount),
                       static_cast<unsigned long>(invalidRxCount));

        if (lastRxType == 1) {
            display.println("Last RX: SOS");
        } else if (lastRxType == 0) {
            display.printf("Last RX: %s\n",
                           lastRxIsGateway ? "PARENT" : "CHILD");
        } else {
            display.println("Last RX: NONE");
        }

        display.display();
        xSemaphoreGive(lcdMutex);
    }
}
