#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "DisplayManager.h"
#include "EspNowManager.h"
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
#define VCC 3.3
#define BATTERY_100_VOLT_HALF 4.2 / 2
#define BATTERY_0_VOLT_HALF 3.2 / 2
#define ANALOG_RESOLUTION 4096

#define LCD_SCK_PIN 5
#define LCD_SDA_PIN 4

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
int lastRSSI;
std::string lastBLEMac;

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

void print_reset_reason() {
  esp_reset_reason_t reason = esp_reset_reason();
  display.print("Reset Reason: ");

  switch (reason) {
    case ESP_RST_POWERON: display.println("POWERON"); break;
    case ESP_RST_BROWNOUT: display.println("BROWNOUT"); break;
    case ESP_RST_SW: display.println("SW"); break;
    case ESP_RST_PANIC: display.println("PANIC"); break;
    case ESP_RST_INT_WDT: display.println("INT_WDT"); break;
    case ESP_RST_TASK_WDT: display.println("TASK_WDT"); break;
    case ESP_RST_DEEPSLEEP: display.println("DEEPSLEEP"); break;
    case ESP_RST_EXT: display.println("EXT"); break;
    default: display.println("UNKNOWN"); break;
  }
  display.display();
}


void setup() {
  lcdMutex = xSemaphoreCreateMutex();

  Wire.begin(LCD_SDA_PIN, LCD_SCK_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (true);
  }

  if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Hello World");

    display.display();

    print_reset_reason();

    xSemaphoreGive(lcdMutex);
  }

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

  lora.SetDefaultConfigValue(config);
  while (lora.InitLoRaModule(config)) {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.println("LoRa init retry");
      display.display();
      xSemaphoreGive(lcdMutex);
    }
    delay(100);
  }
  if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
    display.println("LoRa init success");
    display.display();
    xSemaphoreGive(lcdMutex);
  }

  lora.SwitchToNormalMode();

  xTaskCreateUniversal(LoRaRecvTask, "LoRaRecvTask", 8192, NULL, 1, NULL, 0);

  delay(10);

  setupEspNow();
  setupWifi();

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
}
  

float getBatteryPercent() {
  int millivolt = analogReadMilliVolts(BATTERY_PIN);
  float percent = (millivolt - BATTERY_0_VOLT_HALF * 1000) / (BATTERY_100_VOLT_HALF * 1000 - BATTERY_0_VOLT_HALF * 1000);
  return percent * 100.0;
}

bool sended = false;
bool isAlarmActive = false;
int lastBtn1State = HIGH;         // ボタン1の以前の状態
int sosCount = 0;                 // 連続で押された回数
unsigned long lastPressTime = 0;  // 最後にボタン1が押された時間

void loop() {
  unsigned long currentMillis = millis();   // 現在の時刻を取得

  maintainWifiConnection();

  static unsigned long displayClearTime = 0;
  static bool needDisplayClear = false;
  static unsigned long wifiStatusDisplayUntil = 0;

  // すれ違い結果の画面表示
  if (encounterFlag) {
    encounterFlag = false;

    // SOS発動中でなければ表示する
    if (!isAlarmActive) {
      displayEncounter(getSticker, isRareSticker, displayStickerId);
      clearWifiStatus();
      wifiStatusDisplayUntil = 0;

      // 5秒後に画面をクリアするためのタイマーをセット
      displayClearTime = currentMillis + 5000;
      needDisplayClear = true;
    }
  } else if (!isAlarmActive && !needDisplayClear &&
             getWifiStatus() != WIFI_STATUS_NONE) {
    displayWifiStatus(getWifiStatus());
    clearWifiStatus();
    wifiStatusDisplayUntil = currentMillis + 1000;
  } else if (!isAlarmActive && !needDisplayClear &&
             wifiStatusDisplayUntil != 0 &&
             currentMillis >= wifiStatusDisplayUntil) {
    wifiStatusDisplayUntil = 0;
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.clearDisplay();
      display.display();
      xSemaphoreGive(lcdMutex);
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
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      /*
      display.setCursor(0,0);
      display.clearDisplay();
      display.printf("BLE Mac:\n %s\n", lastBLEMac.c_str());
      display.printf("RSSI:\n %d dBm\n", lastRSSI);
      display.display();
      */
      xSemaphoreGive(lcdMutex);
    }
  }

  int currentBtn1State = digitalRead(BUTTON_1_PIN);

  if (lastBtn1State == HIGH && currentBtn1State == LOW) {

    // 前回のボタン押し下げから3秒(3000ミリ秒)以上たっていたらカウントを0に戻す
    if (currentMillis - lastPressTime > 3000) {
      sosCount = 0;
    }

    sosCount++;
    lastPressTime = currentMillis;

    isAlarmActive = true;

    // 画面表示
    displaySOS(sosCount);

    if (sosCount >= 3) {
      // 3回連続で押された場合、LoRa送信タスクを実行
      LoRaSendTask();
      sosCount = 0;  // カウントをリセット
    }
  }
  lastBtn1State = currentBtn1State;

  if (digitalRead(BUTTON_2_PIN) == LOW) {
    if (!isAlarmActive) {
      displayBattery(getBatteryPercent());

      // 5秒後に画面をクリアするためのタイマーをセット
      displayClearTime = currentMillis + 5000;
      needDisplayClear = true;
    }

    // 通信関係は調整中
    // if (!sended) sendEspNow("ESP NOW !!");
    // sended = true;
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
  if (isAlarmActive) {
    outBuzzer = HIGH;
    outRedLed = LOW;
  }

  // ボタン2が押されていれば緑LEDをONに上書き
  if (digitalRead(BUTTON_2_PIN) == LOW) {
    outGreenLed = LOW;
  }

  digitalWrite(BUZZER_PIN, outBuzzer);
  digitalWrite(RED_LED_PIN, outRedLed);
  digitalWrite(GREEN_LED_PIN, outGreenLed);

  delay(100);
}