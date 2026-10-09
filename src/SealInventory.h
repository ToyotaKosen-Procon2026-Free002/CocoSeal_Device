#pragma once

#include <Arduino.h>

struct SealInventoryItem {
    char sealId[37];
    uint16_t ownedCount;
    uint16_t tradeCount;
};

struct ServerTradePoolEntry {
    char sealId[37];
    uint16_t count;
};

bool initializeSealInventory();
bool initializeTestSealInventory();
size_t getSealInventoryCount();
bool getSealInventoryItem(size_t index, SealInventoryItem& item);
bool reconcileServerTradePool(const ServerTradePoolEntry* entries,
                              size_t entryCount);
bool getFirstTradeableSeal(const char* excludeSealId,
                           char* sealId,
                           size_t capacity);
bool addOwnedSeal(const char* sealId, uint16_t count);
bool setSealTradeCount(const char* sealId, uint16_t count);
bool exchangeOwnedSeals(const char* offeredSealId,
                        const char* receivedSealId);
bool applyTradeOnce(const char* transactionId,
                    const char* offeredSealId,
                    const char* receivedSealId);
bool wasTradeApplied(const char* transactionId);
bool awardGatewaySealOncePerDay(const char* gatewayId,
                                const char* sealId,
                                uint32_t localDateKey);
