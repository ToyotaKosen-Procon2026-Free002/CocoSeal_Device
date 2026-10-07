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
void displayEncounter(EncounterSource source, const char* peerName) {
    const char* name = peerName && peerName[0] ? peerName : "ID";
    if (source == ENCOUNTER_SOURCE_PARENT) {
        const char* lines[] = {"こうばん", "をとおったよ"};
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
    } else {
        const char* lines[] = {name, "とすれちがい"};
        drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
    }
}

void displayParentEncounterReward() {
    const char* lines[] = {"オリジナルシール", "ゲット！"};
    drawJapaneseLines(lines, sizeof(lines) / sizeof(lines[0]));
}

void displayTradeDebugStatus(const char* status) {
    const char* message = status && status[0] ? status : "UNKNOWN";
    bool tradeSucceeded = strncmp(message, "COMPLETE:", 9) == 0;
    bool tradeFailed = strcmp(message, "TRADE_FAILED") == 0;
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (tradeSucceeded || tradeFailed) {
        beginJapaneseScreen();
        if (tradeSucceeded) {
            u8g2.setCursor(0, 32);
            u8g2.print("シールこうかん！");
        } else {
            u8g2.setCursor(0, 20);
            u8g2.print("こうかんに");
            u8g2.setCursor(0, 44);
            u8g2.print("しっぱいしました");
        }
        display.display();
        xSemaphoreGive(lcdMutex);
        return;
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("TRADE PROCESS");
    for (size_t line = 0; line < 4; ++line) {
        char textLine[22];
        size_t offset = line * (sizeof(textLine) - 1);
        if (offset >= strlen(message)) {
            break;
        }
        snprintf(textLine, sizeof(textLine), "%.21s", message + offset);
        display.setCursor(0, 16 + static_cast<int>(line) * 12);
        display.print(textLine);
    }
    display.display();
    xSemaphoreGive(lcdMutex);
}
