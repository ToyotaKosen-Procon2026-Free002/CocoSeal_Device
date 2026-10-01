#pragma once

#include <Arduino.h>

struct SealInventoryItem {
    char sealId[37];
    uint16_t ownedCount;
    uint16_t tradeCount;
};

bool initializeSealInventory();
size_t getSealInventoryCount();
bool getSealInventoryItem(size_t index, SealInventoryItem& item);
bool addOwnedSeal(const char* sealId, uint16_t count);
bool setSealTradeCount(const char* sealId, uint16_t count);
bool exchangeOwnedSeals(const char* offeredSealId,
                        const char* receivedSealId);
bool awardGatewaySealOncePerDay(const char* gatewayId,
                                const char* sealId,
                                uint32_t localDateKey);
