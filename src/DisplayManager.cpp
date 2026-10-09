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
    int lineStep = 0;
    int firstLineY = 10;
    if (lineCount == 2) {
        lineStep = 24;
        firstLineY = 20;
    } else if (lineCount == 3) {
        lineStep = 18;
        firstLineY = 16;
    } else if (lineCount == 5) {
        lineStep = 11;
        firstLineY = 11;
    } else if (lineCount > 3) {
        lineStep = 48 / static_cast<int>(lineCount - 1);
    }

    for (size_t i = 0; i < lineCount; ++i) {
        u8g2.setCursor(0, firstLineY + static_cast<int>(i) * lineStep);
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
    int roundedPercent = static_cast<int>(percent + 0.5f);
    roundedPercent = constrain(roundedPercent, 0, 100);

    char percentageLine[12];
    snprintf(percentageLine, sizeof(percentageLine), "%d%%", roundedPercent);

    if (roundedPercent <= 30) {
        const char* lines[] = {
            "バッテリーのこり",
            percentageLine,
            "おうちでじゅうでんしてね"
        };
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
    } else {
        const char* lines[] = {
            "バッテリーのこり",
            percentageLine
        };
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
    }
}

// すれ違い結果の表示
void displayEncounter(EncounterSource source,
                      const char* peerName,
                      const char* peerDeviceId) {
    if (source == ENCOUNTER_SOURCE_PARENT) {
        const char* lines[] = {"こうばん", "をとおったよ"};
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
    } else if (peerName && peerName[0]) {
        const char* lines[] = {peerName, "とすれちがい"};
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
    } else {
        if (!peerDeviceId || !peerDeviceId[0] ||
            strcmp(peerDeviceId, "unknown") == 0) {
            const char* lines[] = {"ID", "とすれちがい"};
            drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
            return;
        }

        char idLine1[13];
        char idLine2[13];
        char idLine3[13];
        snprintf(idLine1, sizeof(idLine1), "%.12s", peerDeviceId);
        snprintf(idLine2, sizeof(idLine2), "%.12s",
                 strlen(peerDeviceId) > 12 ? peerDeviceId + 12 : "");
        snprintf(idLine3, sizeof(idLine3), "%.12s",
                 strlen(peerDeviceId) > 24 ? peerDeviceId + 24 : "");
        const char* lines[] = {
            idLine1,
            idLine2,
            idLine3,
            "とすれちがい",
            "ました"
        };
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
    }
}

void displayParentEncounterReward() {
    const char* lines[] = {"オリジナルシール", "ゲット！"};
    drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
}

void displayTradeSuccess() {
    const char* lines[] = {
        "シールこうかん",
        "せいこう！",
    };
    drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
}
