#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "DisplayManager.h"
#include "DeviceIdentity.h"
#include "EspNowManager.h"
#include "LocalDatabase.h"
#include "SealInventory.h"
#include "WifiManager.h"
#include <esp32_e220900t22s_jp_lib.h>
#include <NimBLEDevice.h>

#define SERVICE_UUID "42fbd1f2-b02c-1ba6-87f8-7d9ca4f3a343"

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

NimBLEAdvertising *pAdvertising;
NimBLEScan *pScan;
volatile bool bleFlag = false;
volatile bool sosReceivedLoRa = false;
int lastRSSI;
std::string lastBLEMac;
bool isAlarmActive = false;
int sosCount = 0;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    if (device->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
      int rssi = device->getRSSI();
      std::string addr = device->getAddress().toString();

      lastRSSI = rssi;
      lastBLEMac = addr;
      bleFlag = true;
    }
  }
};

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
  if (!saveSosSentEvent()) {
    Serial.println("Warning: SOS event was not persisted before transmission");
  }
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

void setup() {
  Serial.begin(115200);

  if (!initializeDeviceIdentity()) {
    Serial.println("Boot error: device identity initialization failed");
    while (true) {
      delay(1000);
    }
  }

  if (!initializeLocalDatabase()) {
    Serial.println("Boot error: local event database initialization failed");
    while (true) {
      delay(1000);
    }
  }

  if (!initializeSealInventory()) {
    Serial.println("Boot error: seal inventory initialization failed");
    while (true) {
      delay(1000);
    }
  }

  lcdMutex = xSemaphoreCreateMutex();

  Serial.println("Boot: initializing OLED");
  Wire.begin(LCD_SDA_PIN, LCD_SCK_PIN);

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
  initializeJapaneseDisplay();
  display.clearDisplay();
  display.display();

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

  delay(10);

  Serial.println("Boot: initializing LoRa");
  lora.SetDefaultConfigValue(config);
  while (lora.InitLoRaModule(config)) {
    Serial.println("Boot: LoRa init retry");
    delay(100);
  }
  Serial.println("Boot: LoRa initialized");

  lora.SwitchToNormalMode();

  xTaskCreateUniversal(LoRaRecvTask, "LoRaRecvTask", 8192, NULL, 1, NULL, 0);

  delay(10);

  Serial.println("Boot: initializing ESP-NOW");
  setupEspNow();
  Serial.println("Boot: connecting to Wi-Fi");
  setupWifi();
  if (isWifiConnected() && WiFi.channel() != 1) {
    Serial.printf(
        "Warning: router channel %u differs from parent ESP-NOW channel 1\n",
        WiFi.channel());
  }
  Serial.println("Boot: initializing BLE");

  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  NimBLEDevice::init("ESP_NODE");
  
  pAdvertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setName("ESP_NODE");
  advData.addServiceUUID(SERVICE_UUID);
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(30);
  pScan->setScanCallbacks(new ScanCallbacks(), true);
  pScan->start(0, false, true);
  Serial.println("Boot: setup complete");
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

  bool resetPressed = digitalRead(BUTTON_2_PIN) == LOW;
  bool wasAlarmActive = isAlarmActive;

  if (!resetPressed && (sosReceivedEspNow || sosReceivedLoRa)) {
    sosReceivedEspNow = false;
    sosReceivedLoRa = false;
    isAlarmActive = true;
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
  if (encounterFlag) {
    encounterFlag = false;

    // SOS発動中でなければ表示する
    if (!isAlarmActive) {
      displayEncounter(lastEncounterWasParent ? ENCOUNTER_SOURCE_PARENT
                                              : ENCOUNTER_SOURCE_CHILD,
                       displayStickerId);

      // 5秒後に画面をクリアするためのタイマーをセット
      displayClearTime = currentMillis + 5000;
      needDisplayClear = true;
    }
  }

  // 定期的に自分のデータを周囲に送信
  static unsigned long lastSendTime = 0;
  if (currentMillis - lastSendTime >= 5000) { // 5秒ごとに送信
    lastSendTime = currentMillis;
    sendDummySticker();
  }

  if (bleFlag) {
    bleFlag = false;
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

  delay(100);
}