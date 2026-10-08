#include "BatteryManager.h"

#include <Arduino.h>

namespace {
constexpr int BATTERY_PIN = 0;
constexpr float BATTERY_100_VOLT_HALF = 4.2f / 2.0f;
constexpr float BATTERY_0_VOLT_HALF = 3.2f / 2.0f;
}

void initializeBatteryMonitor() {
    pinMode(BATTERY_PIN, INPUT);
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);
}

float getBatteryPercent() {
    const int millivolts = analogReadMilliVolts(BATTERY_PIN);
    const float percent =
        (millivolts - BATTERY_0_VOLT_HALF * 1000.0f) /
        (BATTERY_100_VOLT_HALF * 1000.0f -
         BATTERY_0_VOLT_HALF * 1000.0f) * 100.0f;
    return constrain(percent, 0.0f, 100.0f);
}
