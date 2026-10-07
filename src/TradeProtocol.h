#pragma once

#include <Arduino.h>

bool initializeTradeProtocol();
const char* getTradeProtocolInitError();
bool takeTradeDebugStatus(char* status,
                          size_t capacity,
                          bool& tradeInProgress);
void notifyTradePeerEncounter(const uint8_t* peerMac,
                              const char* peerDeviceId);
bool receiveTradeProtocolPacket(const uint8_t* peerMac,
                                const uint8_t* data,
                                size_t dataLength);
void processTradeProtocol();
