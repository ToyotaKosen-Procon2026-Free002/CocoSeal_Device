#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <esp32_e220900t22s_jp_lib.h>
#include <esp_now.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <esp_coexist.h>

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

volatile bool espNowRecvFlag = false;
uint8_t lastMac[6];
char lastData[64];
int lastDataLen = 0;
volatile bool espNowSentFlag = false;
uint8_t lastSentMac[6];
esp_now_send_status_t lastStatus;
uint8_t address[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;

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

void onEspNowSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  memcpy(lastSentMac, mac_addr, 6);
  lastStatus = status;
  espNowSentFlag = true;
}

void onEspNowRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
  memcpy(lastMac, mac_addr, 6);
  int len = data_len < 63 ? data_len : 63;
  memcpy(lastData, data, len);
  lastData[len] = '\0';
  lastDataLen = len;
  espNowRecvFlag = true;
}

void sendEspNow(const char *msg) {
  esp_now_send(address, (uint8_t*)msg, strlen(msg));
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

  /*
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  while (esp_now_init() != ESP_OK) {
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.println("WiFi init retry");
      display.display();
      xSemaphoreGive(lcdMutex);
    }
    delay(100);
  }
  if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
    display.println("WiFi init success");
    display.display();
    xSemaphoreGive(lcdMutex);
  }

  memcpy(peerInfo.peer_addr, address, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    display.println("Failed to add peer");
    display.display();
    return;
  }

  esp_now_register_send_cb(onEspNowSent);
  esp_now_register_recv_cb(onEspNowRecv);

  */

  /*
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
  */
}
  

float getBatteryPercent() {
  int millivolt = analogReadMilliVolts(BATTERY_PIN);
  float percent = (millivolt - BATTERY_0_VOLT_HALF * 1000) / (BATTERY_100_VOLT_HALF * 1000 - BATTERY_0_VOLT_HALF * 1000);
  return percent;
}

bool sended = false;

void loop() {
  if (espNowRecvFlag) {
    espNowRecvFlag = false;
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.setCursor(0, 0);
      display.clearDisplay();
      display.printf("Received data from:\n %02X:%02X:%02X:%02X:%02X:%02X\n", lastMac[0], lastMac[1], lastMac[2], lastMac[3], lastMac[4], lastMac[5]);
      display.printf("Data:\n %s\n", lastData);
      display.display();
      xSemaphoreGive(lcdMutex);
    }
  }

  if (espNowSentFlag) {
    espNowSentFlag = false;
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.setCursor(0, 0);
      display.clearDisplay();
      display.printf("Last Packet Send Status:\n %s\n", lastStatus == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
      display.display();
      xSemaphoreGive(lcdMutex);
    }
  }
  
  if (bleFlag) {
    bleFlag = false;
    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.setCursor(0,0);
      display.clearDisplay();
      display.printf("BLE Mac:\n %s\n", lastBLEMac.c_str());
      display.printf("RSSI:\n %d dBm\n", lastRSSI);
      display.display();
      xSemaphoreGive(lcdMutex);
    }
  }

  if (digitalRead(BUTTON_1_PIN) == LOW) {
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, HIGH);

    if (!sended) LoRaSendTask();
    sended = true;
  } else if (digitalRead(BUTTON_2_PIN) == LOW) {
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, LOW);

    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println(getBatteryPercent());
      display.display();
      xSemaphoreGive(lcdMutex);
    }

    if (!sended) sendEspNow("ESP NOW !!");
    sended = true;
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, HIGH);

    sended = false;
  }

  delay(100);
}