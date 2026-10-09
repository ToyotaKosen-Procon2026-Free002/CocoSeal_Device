#include "WifiProvisioning.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include <string>
#include <vector>

namespace {
constexpr char SERVICE_UUID[] = "42fbd1f2-b02c-1ba6-87f8-7d9ca4f3a343";
constexpr char CONFIG_CHARACTERISTIC_UUID[] =
    "beb5483e-36e1-4688-b7f5-ea07361b26a8";
constexpr char STATUS_CHARACTERISTIC_UUID[] =
    "d29ae63e-b7d3-4874-a690-3432b85e05a5";
constexpr uint32_t ADVERTISING_WINDOW_MS = 120000;
constexpr size_t MAX_SSID_LENGTH = 32;
constexpr size_t MAX_PASSWORD_LENGTH = 63;

NimBLEServer* server = nullptr;
NimBLECharacteristic* statusCharacteristic = nullptr;
NimBLEAdvertising* advertising = nullptr;
portMUX_TYPE credentialMux = portMUX_INITIALIZER_UNLOCKED;
char deviceIdBuffer[37] = {};
char stagedSsid[MAX_SSID_LENGTH + 1] = {};
char stagedPassword[MAX_PASSWORD_LENGTH + 1] = {};
char pendingSsid[MAX_SSID_LENGTH + 1] = {};
char pendingPassword[MAX_PASSWORD_LENGTH + 1] = {};
char provisioningState[24] = "ready";
bool ssidReceived = false;
bool passwordReceived = false;
bool credentialsPending = false;
bool clientConnected = false;
bool wifiConnected = false;
bool active = false;
bool disconnectPending = false;
bool advertisingRestartPending = false;
bool statusDirty = true;
bool initialized = false;
uint32_t advertisingStartedAt = 0;

void publishStatus() {
    if (!statusCharacteristic || !initialized) {
        return;
    }

    bool shouldPublish = false;
    char currentState[sizeof(provisioningState)] = {};
    char currentDeviceId[sizeof(deviceIdBuffer)] = {};
    portENTER_CRITICAL(&credentialMux);
    shouldPublish = statusDirty;
    statusDirty = false;
    const bool isWifiConnected = wifiConnected;
    snprintf(currentState, sizeof(currentState), "%s", provisioningState);
    snprintf(currentDeviceId, sizeof(currentDeviceId), "%s", deviceIdBuffer);
    portEXIT_CRITICAL(&credentialMux);
    if (!shouldPublish) {
        return;
    }

    JsonDocument document;
    document["device_id"] = currentDeviceId;
    document["device_role"] = "child";
    document["wifi_connected"] = isWifiConnected;
    document["provisioning_state"] = currentState;
    String json;
    serializeJson(document, json);
    statusCharacteristic->setValue(
        reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
    if (clientConnected) {
        statusCharacteristic->notify();
    }
}

class ProvisioningServerCallbacks final : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer*) override {
        portENTER_CRITICAL(&credentialMux);
        clientConnected = true;
        statusDirty = true;
        portEXIT_CRITICAL(&credentialMux);
    }

    void onDisconnect(NimBLEServer*) override {
        portENTER_CRITICAL(&credentialMux);
        clientConnected = false;
        disconnectPending = true;
        advertisingRestartPending = true;
        statusDirty = true;
        portEXIT_CRITICAL(&credentialMux);
    }
};

class ConfigCharacteristicCallbacks final
    : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* characteristic) override {
        const std::string value = characteristic->getValue();
        bool accepted = false;
        bool invalid = false;

        portENTER_CRITICAL(&credentialMux);
        if (value.compare(0, 5, "SSID:") == 0) {
            const size_t length = value.length() - 5;
            if (length == 0 || length > MAX_SSID_LENGTH) {
                invalid = true;
            } else {
                memcpy(stagedSsid, value.data() + 5, length);
                stagedSsid[length] = '\0';
                ssidReceived = true;
                accepted = true;
            }
        } else if (value.compare(0, 5, "PASS:") == 0) {
            const size_t length = value.length() - 5;
            if (length > MAX_PASSWORD_LENGTH) {
                invalid = true;
            } else {
                memcpy(stagedPassword, value.data() + 5, length);
                stagedPassword[length] = '\0';
                passwordReceived = true;
                accepted = true;
            }
        } else {
            invalid = true;
        }

        if (ssidReceived && passwordReceived &&
            !credentialsPending && !invalid) {
            memcpy(pendingSsid, stagedSsid, sizeof(pendingSsid));
            memcpy(pendingPassword, stagedPassword, sizeof(pendingPassword));
            credentialsPending = true;
            ssidReceived = false;
            passwordReceived = false;
            memset(stagedSsid, 0, sizeof(stagedSsid));
            memset(stagedPassword, 0, sizeof(stagedPassword));
            snprintf(provisioningState, sizeof(provisioningState), "%s",
                     "connecting");
            statusDirty = true;
        }
        if (invalid) {
            snprintf(provisioningState, sizeof(provisioningState), "%s",
                     "invalid_credentials");
            statusDirty = true;
        }
        portEXIT_CRITICAL(&credentialMux);

        if (!accepted) {
            Serial.println("Wi-Fi provisioning rejected an invalid BLE command");
        }
    }
};

ProvisioningServerCallbacks serverCallbacks;
ConfigCharacteristicCallbacks configCallbacks;

void deinitializeBluetooth() {
    if (!initialized || clientConnected) {
        return;
    }
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    server = nullptr;
    statusCharacteristic = nullptr;
    advertising = nullptr;
    initialized = false;
}

bool initializeBluetooth() {
    char name[16] = {};
    snprintf(name, sizeof(name), "COCO-%.8s", deviceIdBuffer);
    NimBLEDevice::init(name);
    NimBLEDevice::setMTU(185);

    server = NimBLEDevice::createServer();
    if (!server) {
        Serial.println("BLE provisioning error: failed to create GATT server");
        NimBLEDevice::deinit(true);
        return false;
    }
    server->setCallbacks(&serverCallbacks);

    NimBLEService* service = server->createService(SERVICE_UUID);
    if (!service) {
        Serial.println("BLE provisioning error: failed to create GATT service");
        NimBLEDevice::deinit(true);
        server = nullptr;
        return false;
    }
    NimBLECharacteristic* configCharacteristic =
        service->createCharacteristic(
            CONFIG_CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
                NIMBLE_PROPERTY::WRITE_NR);
    configCharacteristic->setCallbacks(&configCallbacks);
    statusCharacteristic = service->createCharacteristic(
        STATUS_CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    service->start();

    advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);
    advertising->setMaxPreferred(0x12);
    advertising->setMinInterval(1600);
    advertising->setMaxInterval(3200);
    initialized = true;
    return true;
}
}

bool startWifiProvisioning(const char* deviceId, bool wifiConnected) {
    if (!deviceId || strlen(deviceId) != 36) {
        Serial.println("BLE provisioning error: invalid child device ID");
        return false;
    }

    if (initialized && clientConnected) {
        Serial.println("BLE provisioning is already connected to an app");
        return false;
    }
    if (initialized && active) {
        advertisingStartedAt = millis();
        Serial.println("BLE Wi-Fi provisioning window extended");
        return true;
    }

    snprintf(deviceIdBuffer, sizeof(deviceIdBuffer), "%s", deviceId);
    portENTER_CRITICAL(&credentialMux);
    clientConnected = false;
    disconnectPending = false;
    credentialsPending = false;
    ssidReceived = false;
    passwordReceived = false;
    memset(stagedSsid, 0, sizeof(stagedSsid));
    memset(stagedPassword, 0, sizeof(stagedPassword));
    memset(pendingSsid, 0, sizeof(pendingSsid));
    memset(pendingPassword, 0, sizeof(pendingPassword));
    snprintf(provisioningState, sizeof(provisioningState), "%s", "ready");
    statusDirty = true;
    portEXIT_CRITICAL(&credentialMux);

    if (!initialized && !initializeBluetooth()) {
        return false;
    }

    updateWifiProvisioningStatus(wifiConnected, "ready");
    NimBLEDevice::startAdvertising();
    active = true;
    advertisingStartedAt = millis();
    Serial.printf("BLE Wi-Fi provisioning active as COCO-%.8s for 120 seconds\n",
                  deviceIdBuffer);
    return true;
}

bool takeProvisionedWifiCredentials(char* ssid,
                                    size_t ssidCapacity,
                                    char* password,
                                    size_t passwordCapacity) {
    if (!ssid || !password || ssidCapacity == 0 || passwordCapacity == 0) {
        return false;
    }

    portENTER_CRITICAL(&credentialMux);
    if (!credentialsPending) {
        portEXIT_CRITICAL(&credentialMux);
        return false;
    }

    const bool fits =
        strlen(pendingSsid) < ssidCapacity &&
        strlen(pendingPassword) < passwordCapacity;
    if (fits) {
        snprintf(ssid, ssidCapacity, "%s", pendingSsid);
        snprintf(password, passwordCapacity, "%s", pendingPassword);
    }
    memset(pendingSsid, 0, sizeof(pendingSsid));
    memset(pendingPassword, 0, sizeof(pendingPassword));
    credentialsPending = false;
    portEXIT_CRITICAL(&credentialMux);
    if (!fits) {
        Serial.println("BLE provisioning error: credential buffer is too small");
    }
    return fits;
}

void updateWifiProvisioningStatus(bool isWifiConnected, const char* state) {
    portENTER_CRITICAL(&credentialMux);
    wifiConnected = isWifiConnected;
    snprintf(provisioningState, sizeof(provisioningState), "%s",
             state ? state : "unknown");
    statusDirty = true;
    portEXIT_CRITICAL(&credentialMux);
}

void stopWifiProvisioningAdvertising() {
    if (!initialized || !active) {
        return;
    }
    NimBLEDevice::stopAdvertising();
    active = false;
    portENTER_CRITICAL(&credentialMux);
    if (!clientConnected) {
        disconnectPending = true;
    }
    portEXIT_CRITICAL(&credentialMux);
}

void maintainWifiProvisioning() {
    if (!initialized) {
        return;
    }

    if (active &&
        millis() - advertisingStartedAt >= ADVERTISING_WINDOW_MS) {
        Serial.println("BLE Wi-Fi provisioning timed out; disabling radio");
        NimBLEDevice::stopAdvertising();
        active = false;
        if (clientConnected && server) {
            const std::vector<uint16_t> connectedPeers =
                server->getPeerDevices();
            for (uint16_t connectionId : connectedPeers) {
                const int result = server->disconnect(connectionId);
                if (result != 0) {
                    Serial.printf(
                        "BLE provisioning error: failed to close connection "
                        "(%d)\n",
                        result);
                }
            }
        } else {
            disconnectPending = true;
        }
    }

    bool shouldDeinitialize = false;
    bool shouldPublish = false;
    bool shouldRestartAdvertising = false;
    portENTER_CRITICAL(&credentialMux);
    shouldDeinitialize = disconnectPending && !clientConnected && !active;
    shouldRestartAdvertising =
        advertisingRestartPending && !clientConnected && active;
    disconnectPending = false;
    advertisingRestartPending = false;
    shouldPublish = statusDirty;
    portEXIT_CRITICAL(&credentialMux);

    if (shouldRestartAdvertising) {
        NimBLEDevice::startAdvertising();
    }
    if (shouldPublish) {
        publishStatus();
    }
    if (shouldDeinitialize) {
        deinitializeBluetooth();
    }
}
