#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "DisplayManager.h"
#include "DeviceIdentity.h"
#include "EspNowManager.h"
#include "LocalDatabase.h"
#include "SealInventory.h"
#include "ServerSync.h"
#include "TradeProtocol.h"
#include "WifiManager.h"
#include <esp32_e220900t22s_jp_lib.h>
#include <esp_attr.h>
#include <esp_system.h>

#ifndef SERVER_SYNC_RUNTIME_ENABLED
#define SERVER_SYNC_RUNTIME_ENABLED 1
#endif

SET_LOOP_TASK_STACK_SIZE(16384);

#define RED_LED_PIN 2
#define GREEN_LED_PIN 8

#define BUTTON_1_PIN 1
#define BUTTON_2_PIN 9

#define BUZZER_PIN 3

#define BATTERY_PIN 0
#define BATTERY_100_VOLT_HALF 4.2 / 2
#define BATTERY_0_VOLT_HALF 3.2 / 2

#define LCD_SCK_PIN 4
#define LCD_SDA_PIN 5

#define LORA_M0_PIN 7
#define LORA_M1_PIN 6
#define LORA_AUX_PIN 10

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
SemaphoreHandle_t lcdMutex = NULL;

CLoRa lora;
struct LoRaConfigItem_t config;
struct RecvFrameE220900T22SJP_t data;

volatile bool sosReceivedLoRa = false;
bool isAlarmActive = false;
int sosCount = 0;

namespace {
constexpr uint32_t BOOT_DIAGNOSTIC_MAGIC = 0x424F4F54;

enum BootStage : uint32_t {
  BOOT_STAGE_SERIAL = 1,
  BOOT_STAGE_MUTEX,
  BOOT_STAGE_I2C,
  BOOT_STAGE_OLED,
  BOOT_STAGE_DISPLAY_SETUP,
  BOOT_STAGE_GPIO,
  BOOT_STAGE_DEVICE_IDENTITY,
  BOOT_STAGE_LOCAL_DATABASE,
  BOOT_STAGE_SEAL_INVENTORY,
  BOOT_STAGE_TEST_SEALS,
  BOOT_STAGE_TRADE_PROTOCOL,
  BOOT_STAGE_LORA_CONFIG,
  BOOT_STAGE_LORA_INIT,
  BOOT_STAGE_LORA_TASK,
  BOOT_STAGE_WIFI,
  BOOT_STAGE_ESPNOW,
  BOOT_STAGE_COMPLETE
};

RTC_DATA_ATTR uint32_t previousBootDiagnosticMagic = 0;
RTC_DATA_ATTR uint32_t previousBootDiagnosticStage = 0;
RTC_DATA_ATTR bool previousBootDiagnosticCompleted = false;

const char* bootStageName(uint32_t stage) {
  switch (stage) {
    case BOOT_STAGE_SERIAL: return "serial";
    case BOOT_STAGE_MUTEX: return "mutex";
    case BOOT_STAGE_I2C: return "I2C";
    case BOOT_STAGE_OLED: return "OLED initialization";
    case BOOT_STAGE_DISPLAY_SETUP: return "display setup";
    case BOOT_STAGE_GPIO: return "GPIO setup";
    case BOOT_STAGE_DEVICE_IDENTITY: return "device identity";
    case BOOT_STAGE_LOCAL_DATABASE: return "local event database";
    case BOOT_STAGE_SEAL_INVENTORY: return "seal inventory";
    case BOOT_STAGE_TEST_SEALS: return "test seal inventory";
    case BOOT_STAGE_TRADE_PROTOCOL: return "trade protocol";
    case BOOT_STAGE_LORA_CONFIG: return "LoRa configuration";
    case BOOT_STAGE_LORA_INIT: return "LoRa initialization";
    case BOOT_STAGE_LORA_TASK: return "LoRa receive task";
    case BOOT_STAGE_WIFI: return "Wi-Fi";
    case BOOT_STAGE_ESPNOW: return "ESP-NOW";
    case BOOT_STAGE_COMPLETE: return "setup complete";
    default: return "unknown";
  }
}

void beginBootStep(BootStage stage) {
  previousBootDiagnosticMagic = BOOT_DIAGNOSTIC_MAGIC;
  previousBootDiagnosticStage = static_cast<uint32_t>(stage);
  previousBootDiagnosticCompleted = false;
  Serial.printf("BOOT STEP BEGIN: %s\n", bootStageName(stage));
  Serial.flush();
}

void completeBootStep(BootStage stage) {
  previousBootDiagnosticMagic = BOOT_DIAGNOSTIC_MAGIC;
  previousBootDiagnosticStage = static_cast<uint32_t>(stage);
  previousBootDiagnosticCompleted = true;
  Serial.printf("BOOT STEP DONE: %s\n", bootStageName(stage));
  Serial.flush();
}
}

void LoRaRecvTask(void *pvParameters) {
  while (1) {
    if (lora.receiveFrame(&data) == 0) {
      if (data.recv_data_len >= 3 &&
          memcmp(data.recv_data, "SOS", 3) == 0) {
        sosReceivedLoRa = true;
      }
      if (data.recv_data_len < 3 ||
          memcmp(data.recv_data, "SOS", 3) != 0) {
        if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0, 0);

        display.printf("recv data:\n");
        for (int i = 0; i < data.recv_data_len; i++) {
          display.printf("%c", data.recv_data[i]);
        }
        display.printf("\n");
        display.printf("hex dump:\n");
        for (int i = 0; i < data.recv_data_len; i++) {
          display.printf("%02x ", data.recv_data[i]);
        }
        display.printf("\n");
        display.printf("RSSI: %d dBm\n", data.rssi);
        display.printf("\n");

        display.display();
        xSemaphoreGive(lcdMutex);
        }
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void LoRaSendTask() {
  if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
    display.clearDisplay();
    display.setCursor(0, 0);
    String msg = "SOS !!";

    if (lora.SendFrame(config, (uint8_t *)msg.c_str(), strlen(msg.c_str())) == 0) {
      display.printf("send succeeded.\n");
      display.printf("\n");
    } else {
      display.printf("send failed.\n");
      display.printf("\n");
    }

    display.display();
    xSemaphoreGive(lcdMutex);
  }
}

void triggerSos() {
  isAlarmActive = true;
  sendSosNotification();
  LoRaSendTask();
  displaySOSAlert();
}

void resetSosAlarm() {
  isAlarmActive = false;
  sosCount = 0;
  sosReceivedEspNow = false;
  sosReceivedLoRa = false;
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH);
}

void displayBootStatus(const char *message) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(message);
  display.display();
}

const char *getResetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "OTHER";
  }
}

void setup() {
  uint32_t lastBootStage = previousBootDiagnosticStage;
  bool hasPreviousBootDiagnostic =
      previousBootDiagnosticMagic == BOOT_DIAGNOSTIC_MAGIC;
  bool previousBootStageCompleted = previousBootDiagnosticCompleted;
  previousBootDiagnosticMagic = BOOT_DIAGNOSTIC_MAGIC;
  previousBootDiagnosticStage = BOOT_STAGE_SERIAL;
  previousBootDiagnosticCompleted = false;
  Serial.begin(115200);
  Serial.println("BOOT STEP BEGIN: serial");
  Serial.flush();
  esp_reset_reason_t resetReason = esp_reset_reason();
  delay(1000);
  if (hasPreviousBootDiagnostic) {
    Serial.printf("Boot diagnostic: previous reset at '%s' (%s)\n",
                  bootStageName(lastBootStage),
                  previousBootStageCompleted ? "step completed" :
                                                "step was in progress");
  } else {
    Serial.println("Boot diagnostic: no previous application stage recorded");
  }
  completeBootStep(BOOT_STAGE_SERIAL);
  logPreviousServerSyncDiagnostic();
  Serial.printf("Boot: reset reason %d (%s)\n",
                static_cast<int>(resetReason),
                getResetReasonName(resetReason));

  beginBootStep(BOOT_STAGE_MUTEX);
  lcdMutex = xSemaphoreCreateMutex();
  completeBootStep(BOOT_STAGE_MUTEX);

  beginBootStep(BOOT_STAGE_I2C);
  Serial.println("Boot: initializing OLED");
  Wire.begin(LCD_SDA_PIN, LCD_SCK_PIN);
  completeBootStep(BOOT_STAGE_I2C);

  beginBootStep(BOOT_STAGE_OLED);
  uint8_t oledAddress = 0;
  const uint8_t oledAddresses[] = {0x3C, 0x3D};
  for (uint8_t address : oledAddresses) {
    if (display.begin(SSD1306_SWITCHCAPVCC, address)) {
      oledAddress = address;
      break;
    }
  }

  if (oledAddress == 0) {
    Serial.println("Boot error: OLED initialization failed");
    while (true);
  }
  Serial.printf("Boot: OLED initialized at 0x%02X\n", oledAddress);
  completeBootStep(BOOT_STAGE_OLED);

  beginBootStep(BOOT_STAGE_DISPLAY_SETUP);
  displayBootStatus("Starting device...");
  initializeJapaneseDisplay();
  display.clearDisplay();
  display.display();
  displayBootStatus("Reset reason:");
  display.setCursor(0, 12);
  display.println(getResetReasonName(resetReason));
  display.display();
  delay(3000);
  completeBootStep(BOOT_STAGE_DISPLAY_SETUP);

  beginBootStep(BOOT_STAGE_GPIO);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, HIGH);

  pinMode(BUTTON_1_PIN, INPUT);
  pinMode(BUTTON_2_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(BATTERY_PIN, INPUT);
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);
  completeBootStep(BOOT_STAGE_GPIO);

  beginBootStep(BOOT_STAGE_DEVICE_IDENTITY);
  Serial.println("Boot: initializing device identity");
  displayBootStatus("Checking device identity...");
  if (!initializeDeviceIdentity()) {
    Serial.println("Boot error: device identity initialization failed");
    displayBootStatus("BOOT ERROR: ID / key");
    while (true) {
      delay(1000);
    }
  }
  Serial.printf("Child UUID: %s\n", getDeviceId());
  completeBootStep(BOOT_STAGE_DEVICE_IDENTITY);

  beginBootStep(BOOT_STAGE_LOCAL_DATABASE);
  Serial.println("Boot: initializing local event database");
  displayBootStatus("Checking event storage...");
  if (!initializeLocalDatabase()) {
    Serial.println("Boot error: local event database initialization failed");
    displayBootStatus("BOOT ERROR: event DB");
    while (true) {
      delay(1000);
    }
  }
  completeBootStep(BOOT_STAGE_LOCAL_DATABASE);

  beginBootStep(BOOT_STAGE_SEAL_INVENTORY);
  Serial.println("Boot: initializing seal inventory");
  displayBootStatus("Checking seal storage...");
  if (!initializeSealInventory()) {
    Serial.println("Boot error: seal inventory initialization failed");
    displayBootStatus("BOOT ERROR: seal DB");
    while (true) {
      delay(1000);
    }
  }
  completeBootStep(BOOT_STAGE_SEAL_INVENTORY);

  beginBootStep(BOOT_STAGE_TEST_SEALS);
  if (!initializeTestSealInventory()) {
    Serial.println("Boot error: test seal initialization failed");
    displayBootStatus("BOOT ERROR: test seals");
    while (true) {
      delay(1000);
    }
  }
  completeBootStep(BOOT_STAGE_TEST_SEALS);

  beginBootStep(BOOT_STAGE_TRADE_PROTOCOL);
  Serial.println("Boot: initializing trade protocol");
  displayBootStatus("Checking trade state...");
  if (!initializeTradeProtocol()) {
    const char *tradeError = getTradeProtocolInitError();
    Serial.printf("Boot warning: trade protocol disabled (%s)\n", tradeError);
    displayBootStatus("Trade disabled:");
    display.setCursor(0, 12);
    display.println(tradeError);
    display.display();
    delay(2000);
  }
  completeBootStep(BOOT_STAGE_TRADE_PROTOCOL);

  delay(10);

  beginBootStep(BOOT_STAGE_LORA_CONFIG);
  Serial.println("Boot: initializing LoRa");
  displayBootStatus("Initializing LoRa...");
  lora.SetDefaultConfigValue(config);
  completeBootStep(BOOT_STAGE_LORA_CONFIG);

  beginBootStep(BOOT_STAGE_LORA_INIT);
  while (lora.InitLoRaModule(config)) {
    Serial.println("Boot: LoRa init retry");
    delay(100);
  }
  Serial.println("Boot: LoRa initialized");
  completeBootStep(BOOT_STAGE_LORA_INIT);

  beginBootStep(BOOT_STAGE_LORA_TASK);
  lora.SwitchToNormalMode();
  BaseType_t loraTaskCreated =
      xTaskCreateUniversal(LoRaRecvTask, "LoRaRecvTask", 8192, NULL, 1, NULL, 0);
  if (loraTaskCreated != pdPASS) {
    Serial.println("Boot error: failed to create LoRa receive task");
  }
  completeBootStep(BOOT_STAGE_LORA_TASK);

  delay(10);

  beginBootStep(BOOT_STAGE_WIFI);
  Serial.println("Boot: connecting to Wi-Fi");
  displayBootStatus("Connecting Wi-Fi...");
  setupWifi();
  if (isWifiConnected()) {
    Serial.printf("Contest mode active; ESP-NOW will share Wi-Fi channel %u\n",
                  WiFi.channel());
  } else {
    Serial.println("ESP-NOW will use offline fallback channel 1");
  }
  completeBootStep(BOOT_STAGE_WIFI);

  beginBootStep(BOOT_STAGE_ESPNOW);
  Serial.println("Boot: initializing ESP-NOW");
  displayBootStatus("Initializing ESP-NOW...");
  setupEspNow();
  completeBootStep(BOOT_STAGE_ESPNOW);

  beginBootStep(BOOT_STAGE_COMPLETE);
  Serial.println("Boot: setup complete");
#if SERVER_SYNC_RUNTIME_ENABLED
  Serial.println("Server sync runtime: enabled");
  Serial.println(
      "BOOT: own device name will be printed after server profile is loaded");
#else
  Serial.println("Server sync runtime: disabled for diagnosis");
  Serial.println("BOOT: own device name unavailable (server sync disabled)");
#endif
  displayBootStatus("Ready");
  completeBootStep(BOOT_STAGE_COMPLETE);
}
  

float getBatteryPercent() {
  int millivolt = analogReadMilliVolts(BATTERY_PIN);
  float percent = (millivolt - BATTERY_0_VOLT_HALF * 1000) / (BATTERY_100_VOLT_HALF * 1000 - BATTERY_0_VOLT_HALF * 1000);
  return percent * 100.0;
}

int lastBtn1State = HIGH;         // ボタン1の以前の状態
unsigned long lastPressTime = 0;  // 最後にボタン1が押された時間

void loop() {
  unsigned long currentMillis = millis();   // 現在の時刻を取得

  maintainWifiConnection();
  processQueuedLocalEvents();
#if !CONTEST_MODE
  static unsigned long lastGatewayRewardProcess = 0;
  if (currentMillis - lastGatewayRewardProcess >= 5000) {
    lastGatewayRewardProcess = currentMillis;
    uint32_t localDateKey = 0;
    LocalEvent gatewayEncounter = {};
    if (getNextPendingGatewayEncounter(gatewayEncounter)) {
      if (!gatewayEncounter.stickerId[0]) {
        Serial.printf("Skipping gateway encounter without sticker: %s\n",
                      gatewayEncounter.eventId);
        markGatewayRewardProcessed(gatewayEncounter.eventId);
      } else if (getTrustedLocalDateKey(localDateKey) &&
                 awardGatewaySealOncePerDay(gatewayEncounter.partnerDeviceId,
                                            gatewayEncounter.stickerId,
                                            localDateKey)) {
        if (!markGatewayRewardProcessed(gatewayEncounter.eventId)) {
          Serial.println("Warning: gateway reward state was not finalized");
        }
      }
    }
  }
#endif
  processTradeProtocol();
  processEspNowDisplayEvents();
  static bool tradeDebugVisible = false;
  static unsigned long tradeDebugHideAt = 0;
  static bool tradeOutcomePending = false;
  static unsigned long tradeOutcomeDisplayAt = 0;
  static char pendingTradeOutcome[64] = {};
  static bool encounterSequenceActive = false;
  static bool parentEncounterIntro = false;
  static unsigned long encounterScreenUntil = 0;
  char tradeStatus[64];
  bool currentTradeInProgress = false;
  if (takeTradeDebugStatus(tradeStatus, sizeof(tradeStatus),
                           currentTradeInProgress)) {
    if (!currentTradeInProgress) {
      snprintf(pendingTradeOutcome, sizeof(pendingTradeOutcome), "%s",
               tradeStatus);
      tradeOutcomePending = true;
      tradeOutcomeDisplayAt = currentMillis + 1200;
    }
  }
  bool resetPressed = digitalRead(BUTTON_2_PIN) == LOW;
  bool wasAlarmActive = isAlarmActive;

  if (!resetPressed && (sosReceivedEspNow || sosReceivedLoRa)) {
    sosReceivedEspNow = false;
    sosReceivedLoRa = false;
    isAlarmActive = true;
    encounterSequenceActive = false;
    tradeDebugVisible = false;
    displaySOSReceived();
  }
  if (resetPressed) {
    resetSosAlarm();
    if (wasAlarmActive && xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.clearDisplay();
      display.display();
      xSemaphoreGive(lcdMutex);
    }
  }

  static unsigned long displayClearTime = 0;
  static bool needDisplayClear = false;

  // すれ違い結果の画面表示
  if (encounterFlag && !isAlarmActive) {
    encounterFlag = false;
    encounterSequenceActive = true;
    parentEncounterIntro = lastEncounterWasParent;
    encounterScreenUntil = currentMillis + 3000;
    tradeDebugVisible = false;
    needDisplayClear = false;
    displayEncounter(lastEncounterWasParent ? ENCOUNTER_SOURCE_PARENT
                                            : ENCOUNTER_SOURCE_CHILD,
                     displayPeerName);
  }

  if (encounterSequenceActive &&
      static_cast<int32_t>(currentMillis - encounterScreenUntil) >= 0 &&
      !isAlarmActive) {
    if (parentEncounterIntro) {
      parentEncounterIntro = false;
      displayParentEncounterReward();
      encounterScreenUntil = currentMillis + 3000;
    } else {
      encounterSequenceActive = false;
      if (tradeOutcomePending) {
        tradeOutcomeDisplayAt = currentMillis;
      } else if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.display();
        xSemaphoreGive(lcdMutex);
      }
    }
  }

  if (tradeOutcomePending &&
      static_cast<int32_t>(currentMillis - tradeOutcomeDisplayAt) >= 0 &&
      !encounterSequenceActive && !isAlarmActive) {
    tradeOutcomePending = false;
    tradeDebugVisible = true;
    tradeDebugHideAt = currentMillis + 3000;
    displayTradeDebugStatus(pendingTradeOutcome);
    displayClearTime = tradeDebugHideAt;
    needDisplayClear = true;
  }

#if SERVER_SYNC_RUNTIME_ENABLED
  if (!isSynchronizedEncounterDisplayPending() &&
      !needDisplayClear && !tradeOutcomePending &&
      !encounterSequenceActive) {
    processServerSync();
  }
#endif

  if (tradeDebugVisible &&
      static_cast<int32_t>(currentMillis - tradeDebugHideAt) >= 0 &&
      !isAlarmActive && !encounterSequenceActive && !encounterFlag) {
    tradeDebugVisible = false;
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.clearDisplay();
      display.display();
      xSemaphoreGive(lcdMutex);
    }
  }

  // ★ 修正点: 5秒ごとに子機から全13チャネルへすれ違い信号を自発送信する
  static unsigned long lastEncounterSendTime = 0;
  if (currentMillis - lastEncounterSendTime >= 5000) {
    lastEncounterSendTime = currentMillis;
    Serial.println("[ESP-NOW] Sending encounter announcement...");
    sendEncounterAnnouncement();
  }

  int currentBtn1State = digitalRead(BUTTON_1_PIN);

  if (!resetPressed && lastBtn1State == HIGH && currentBtn1State == LOW) {

    // 前回のボタン押し下げから3秒(3000ミリ秒)以上たっていたらカウントを0に戻す
    if (currentMillis - lastPressTime > 3000) {
      sosCount = 0;
    }

    sosCount++;
    lastPressTime = currentMillis;

    // 1回目の押下から警報を開始する
    isAlarmActive = true;

    // 画面表示
    displaySOSPressCount(sosCount);

    if (sosCount >= 3) {
      triggerSos();
      sosCount = 0;  // カウントをリセット
    }
  }
  lastBtn1State = currentBtn1State;

  if (resetPressed) {
    if (!wasAlarmActive) {
      displayBattery(getBatteryPercent());

      // 5秒後に画面をクリアするためのタイマーをセット
      displayClearTime = currentMillis + 5000;
      needDisplayClear = true;
    }
  }

  if (needDisplayClear && currentMillis >= displayClearTime) {
    needDisplayClear = false;

    if (!isAlarmActive) {
      if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.display();
        xSemaphoreGive(lcdMutex);
      }
    }
  }

  int outBuzzer = LOW;
  int outRedLed = HIGH;
  int outGreenLed = HIGH;

  // アラーム状態ならブザーと赤LEDをONに上書き
  if (isAlarmActive && !resetPressed) {
    outBuzzer = HIGH;
    outRedLed = LOW;
  }

  // ボタン2が押されていれば緑LEDをONに上書き
  if (resetPressed) {
    outGreenLed = LOW;
  }

  digitalWrite(BUZZER_PIN, outBuzzer);
  digitalWrite(RED_LED_PIN, outRedLed);
  digitalWrite(GREEN_LED_PIN, outGreenLed);

  delay(isSynchronizedEncounterDisplayPending() || tradeOutcomePending ||
                encounterSequenceActive
            ? 20
            : 100);
}