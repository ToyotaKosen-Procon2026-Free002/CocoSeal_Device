#pragma once

#include <Arduino.h>

bool initializeDeviceIdentity();
const char* getDeviceId();
const uint8_t* getDevicePublicKey(size_t& length);
bool signDeviceMessage(const uint8_t* message,
                      size_t messageLength,
                      uint8_t* signature,
                      size_t signatureCapacity,
                      size_t& signatureLength);
