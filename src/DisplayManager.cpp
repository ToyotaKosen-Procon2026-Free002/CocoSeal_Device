#include "DisplayManager.h"
#include <U8g2_for_Adafruit_GFX.h>

namespace {
U8G2_FOR_ADAFRUIT_GFX u8g2;

void beginJapaneseScreen() {
    display.clearDisplay();
    u8g2.setForegroundColor(SSD1306_WHITE);
    u8g2.setBackgroundColor(SSD1306_BLACK);
    u8g2.setFont(u8g2_font_unifont_t_japanese1);
    u8g2.setFontMode(1);
    u8g2.setFontDirection(0);
}

void drawJapaneseLines(const char* const* lines, size_t lineCount) {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    beginJapaneseScreen();
    int lineStep = lineCount > 1 ? 48 / static_cast<int>(lineCount - 1) : 0;
    for (size_t i = 0; i < lineCount; ++i) {
        u8g2.setCursor(0, 10 + static_cast<int>(i) * lineStep);
        u8g2.print(lines[i]);
    }
    display.display();
    xSemaphoreGive(lcdMutex);
}
}

void initializeJapaneseDisplay() {
    u8g2.begin(display);
}

void displaySOSPressCount(int count) {
    char countLine[8];
    snprintf(countLine, sizeof(countLine), "%02d/03", count);
    const char* lines[] = {
        "SOS",
        countLine,
        "バッテリーボタンで",
        "かいじょ"
    };
    drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
}

void displaySOSAlert() {
    const char* lines[] = {
        "SOSアラート！",
        "ちかくの大人に",
        "いってね！",
        "バッテリーボタンで",
        "かいじょ"
    };
    drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
}

void displaySOSReceived() {
    const char* lines[] = {
        "ちかくでSOSが",
        "なったよ",
        "あんぜんなところに",
        "いってね"
    };
    drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
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
void displayEncounter(EncounterSource source, const char* stickerId) {
    if (source == ENCOUNTER_SOURCE_PARENT) {
        const char* lines[] = {
            "testをとおったよ",
            "オリジナルシール",
            "ゲット！"
        };
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
        return;
    }

    char stickerLine[24];
    snprintf(stickerLine, sizeof(stickerLine), "シール[%s]",
             stickerId && stickerId[0] ? stickerId : "st001");
    const char* lines[] = {
        "testとすれちがい",
        stickerLine,
        "ゲット！"
    };
    drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
}
