#include "SealInventory.h"

#include <Preferences.h>
#include <stddef.h>

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "seals";
constexpr char INVENTORY_KEY[] = "state";
constexpr uint8_t SCHEMA_VERSION = 3;
constexpr size_t MAX_SEAL_TYPES = 32;
constexpr size_t MAX_DAILY_GATEWAYS = 10;
constexpr size_t MAX_COMPLETED_TRADES = 4;

struct LegacyInventoryState {
    uint8_t schemaVersion;
    uint8_t itemCount;
    uint8_t reserved[2];
    SealInventoryItem items[MAX_SEAL_TYPES];
    uint32_t checksum;
};

struct GatewayAward {
    char gatewayId[37];
    uint32_t lastAwardDate;
};

struct LegacyInventoryStateV2 {
    uint8_t schemaVersion;
    uint8_t itemCount;
    uint8_t reserved[2];
    SealInventoryItem items[MAX_SEAL_TYPES];
    GatewayAward gatewayAwards[MAX_DAILY_GATEWAYS];
    uint32_t checksum;
};

struct InventoryState {
    uint8_t schemaVersion;
    uint8_t itemCount;
    uint8_t tradeReceiptCursor;
    uint8_t reserved;
    SealInventoryItem items[MAX_SEAL_TYPES];
    GatewayAward gatewayAwards[MAX_DAILY_GATEWAYS];
    char completedTradeIds[MAX_COMPLETED_TRADES][37];
    uint32_t checksum;
};
static_assert(sizeof(InventoryState) <= 1984,
              "Inventory state exceeds the supported NVS blob size");

uint32_t calculateChecksum(const InventoryState& state) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < offsetof(InventoryState, checksum); ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

uint32_t calculateLegacyChecksum(const LegacyInventoryState& state) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < offsetof(LegacyInventoryState, checksum); ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

uint32_t calculateV2Checksum(const LegacyInventoryStateV2& state) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < offsetof(LegacyInventoryStateV2, checksum); ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

bool isValidSealId(const char* sealId) {
    return sealId && sealId[0] && strnlen(sealId, 37) < 37;
}

bool isValidState(const InventoryState& state) {
    if (state.schemaVersion != SCHEMA_VERSION ||
        state.itemCount > MAX_SEAL_TYPES ||
        state.checksum != calculateChecksum(state)) {
        return false;
    }

    for (size_t i = 0; i < state.itemCount; ++i) {
        const SealInventoryItem& item = state.items[i];
        if (!isValidSealId(item.sealId) ||
            item.ownedCount == 0 ||
            item.tradeCount > item.ownedCount) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(item.sealId, state.items[j].sealId) == 0) {
                return false;
            }
        }
    }
    for (const GatewayAward& award : state.gatewayAwards) {
        if (award.gatewayId[0] &&
            (!isValidSealId(award.gatewayId) || award.lastAwardDate == 0)) {
            return false;
        }
        if (!award.gatewayId[0] && award.lastAwardDate != 0) {
            return false;
        }
    }
    if (state.tradeReceiptCursor >= MAX_COMPLETED_TRADES) {
        return false;
    }
    for (const auto& tradeId : state.completedTradeIds) {
        if (tradeId[0] && !isValidSealId(tradeId)) {
            return false;
        }
    }
    return true;
}

void copyLegacyGatewayAwards(InventoryState& state,
                             const GatewayAward* awards) {
    memcpy(state.gatewayAwards, awards, sizeof(state.gatewayAwards));
}

bool loadState(InventoryState& state) {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
        Serial.println("Seal inventory error: failed to open NVS");
        return false;
    }

    size_t length = preferences.getBytesLength(INVENTORY_KEY);
    if (length == 0) {
        preferences.end();
        memset(&state, 0, sizeof(state));
        state.schemaVersion = SCHEMA_VERSION;
        return true;
    }

    if (length == sizeof(state)) {
        bool loaded =
            preferences.getBytes(INVENTORY_KEY, &state, sizeof(state)) ==
            sizeof(state);
        preferences.end();
        if (!loaded || !isValidState(state)) {
            Serial.println("Seal inventory error: stored data failed validation");
            return false;
        }
        return true;
    }

    LegacyInventoryStateV2 legacyV2 = {};
    bool loadedV2 = length == sizeof(legacyV2) &&
                    preferences.getBytes(
                        INVENTORY_KEY, &legacyV2, sizeof(legacyV2)) ==
                        sizeof(legacyV2);
    if (loadedV2) {
        preferences.end();
        if (legacyV2.schemaVersion != 2 ||
            legacyV2.itemCount > MAX_SEAL_TYPES ||
            legacyV2.checksum != calculateV2Checksum(legacyV2)) {
            Serial.println("Seal inventory error: stored data failed validation");
            return false;
        }
        state = {};
        state.schemaVersion = SCHEMA_VERSION;
        state.itemCount = legacyV2.itemCount;
        memcpy(state.items, legacyV2.items, sizeof(legacyV2.items));
        copyLegacyGatewayAwards(state, legacyV2.gatewayAwards);
        state.checksum = calculateChecksum(state);
        if (!isValidState(state)) {
            Serial.println("Seal inventory error: schema 2 inventory failed validation");
            return false;
        }
        Serial.println("Seal inventory: migrated schema 2 data");
        return true;
    }

    LegacyInventoryState legacy = {};
    bool loaded = length == sizeof(legacy) &&
                  preferences.getBytes(
                      INVENTORY_KEY, &legacy, sizeof(legacy)) == sizeof(legacy);
    preferences.end();
    if (!loaded || legacy.schemaVersion != 1 ||
        legacy.itemCount > MAX_SEAL_TYPES ||
        legacy.checksum != calculateLegacyChecksum(legacy)) {
        Serial.println("Seal inventory error: stored data failed validation");
        return false;
    }

    state = {};
    state.schemaVersion = SCHEMA_VERSION;
    state.itemCount = legacy.itemCount;
    memcpy(state.items, legacy.items, sizeof(legacy.items));
    state.checksum = calculateChecksum(state);
    if (!isValidState(state)) {
        Serial.println("Seal inventory error: legacy inventory failed validation");
        return false;
    }
    Serial.println("Seal inventory: migrated local inventory schema");
    return true;
}

bool saveState(InventoryState& state) {
    state.checksum = calculateChecksum(state);
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Seal inventory error: failed to open NVS for writing");
        return false;
    }

    bool saved = preferences.putBytes(
                     INVENTORY_KEY, &state, sizeof(state)) == sizeof(state);
    preferences.end();
    if (!saved) {
        Serial.println("Seal inventory error: failed to persist state");
    }
    return saved;
}

int findItem(const InventoryState& state, const char* sealId) {
    for (size_t i = 0; i < state.itemCount; ++i) {
        if (strcmp(state.items[i].sealId, sealId) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool addToState(InventoryState& state,
                const char* sealId,
                uint16_t count) {
    int index = findItem(state, sealId);
    if (index >= 0) {
        SealInventoryItem& item = state.items[index];
        if (count > UINT16_MAX - item.ownedCount) {
            Serial.println("Seal inventory error: owned count would overflow");
            return false;
        }
        item.ownedCount += count;
        return true;
    }

    if (state.itemCount >= MAX_SEAL_TYPES) {
        Serial.println("Seal inventory full: cannot add another seal type");
        return false;
    }
    SealInventoryItem& item = state.items[state.itemCount++];
    snprintf(item.sealId, sizeof(item.sealId), "%s", sealId);
    item.ownedCount = count;
    item.tradeCount = 0;
    return true;
}
}

bool initializeSealInventory() {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Seal inventory error: failed to open NVS");
        return false;
    }
    size_t storedLength = preferences.getBytesLength(INVENTORY_KEY);
    bool hasStoredState = storedLength != 0;
    preferences.end();

    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }
    if (!hasStoredState || storedLength != sizeof(state)) {
        return saveState(state);
    }
    return true;
}

bool initializeTestSealInventory() {
#if defined(ENABLE_TEST_SEAL_DATA)
    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }
    if (state.itemCount != 0) {
        Serial.println("Test seals skipped: inventory is not empty");
        return true;
    }

    constexpr char TEST_SEAL_IDS[][37] = {
        "st_test_001", "st_test_002", "st_test_003", "st_test_004"
    };
    for (const char* sealId : TEST_SEAL_IDS) {
        if (!addToState(state, sealId, 1)) {
            Serial.printf("Test seal seeding failed at %s\n", sealId);
            return false;
        }
        state.items[state.itemCount - 1].tradeCount = 1;
    }
    if (!saveState(state)) {
        return false;
    }
    Serial.println("Test inventory seeded with 4 tradeable test seals");
#else
    Serial.println("Test seal seeding disabled");
#endif
    return true;
}

size_t getSealInventoryCount() {
    InventoryState state = {};
    return loadState(state) ? state.itemCount : 0;
}

bool getSealInventoryItem(size_t index, SealInventoryItem& item) {
    InventoryState state = {};
    if (!loadState(state) || index >= state.itemCount) {
        return false;
    }
    item = state.items[index];
    return true;
}

bool reconcileServerTradePool(const ServerTradePoolEntry* entries,
                              size_t entryCount) {
    if (entryCount > MAX_SEAL_TYPES || (entryCount > 0 && !entries)) {
        Serial.println("Seal inventory error: invalid server trade pool");
        return false;
    }

    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }
    for (size_t i = 0; i < state.itemCount; ++i) {
        state.items[i].tradeCount = 0;
    }

    for (size_t i = 0; i < entryCount; ++i) {
        if (!isValidSealId(entries[i].sealId) || entries[i].count == 0) {
            Serial.println("Seal inventory error: invalid server trade pool entry");
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(entries[i].sealId, entries[j].sealId) == 0) {
                Serial.println("Seal inventory error: duplicate server trade pool entry");
                return false;
            }
        }

        int index = findItem(state, entries[i].sealId);
        if (index < 0) {
            if (!addToState(state, entries[i].sealId, entries[i].count)) {
                return false;
            }
            index = static_cast<int>(state.itemCount - 1);
        } else if (state.items[index].ownedCount < entries[i].count) {
            state.items[index].ownedCount = entries[i].count;
        }
        state.items[index].tradeCount = entries[i].count;
    }

    if (!saveState(state)) {
        return false;
    }
    Serial.printf("Seal inventory: reconciled %u server tradeable seal types\n",
                  static_cast<unsigned>(entryCount));
    return true;
}

bool getFirstTradeableSeal(const char* excludeSealId,
                           char* sealId,
                           size_t capacity) {
    if (!sealId || capacity == 0) {
        Serial.println("Seal inventory error: invalid trade seal output buffer");
        return false;
    }
    sealId[0] = '\0';
    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }
    for (size_t i = 0; i < state.itemCount; ++i) {
        const SealInventoryItem& item = state.items[i];
        if (item.tradeCount == 0 ||
            (excludeSealId && excludeSealId[0] &&
             strcmp(item.sealId, excludeSealId) == 0)) {
            continue;
        }
        if (strlen(item.sealId) + 1 > capacity) {
            Serial.println("Seal inventory error: trade seal output is too small");
            return false;
        }
        snprintf(sealId, capacity, "%s", item.sealId);
        return true;
    }
    return false;
}

bool addOwnedSeal(const char* sealId, uint16_t count) {
    if (!isValidSealId(sealId) || count == 0) {
        Serial.println("Seal inventory error: invalid seal ID or count");
        return false;
    }
    InventoryState state = {};
    return loadState(state) && addToState(state, sealId, count) &&
           saveState(state);
}

bool setSealTradeCount(const char* sealId, uint16_t count) {
    if (!isValidSealId(sealId)) {
        Serial.println("Seal inventory error: invalid seal ID");
        return false;
    }
    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }

    int index = findItem(state, sealId);
    if (index < 0 || count > state.items[index].ownedCount) {
        Serial.println("Seal inventory error: trade count exceeds ownership");
        return false;
    }
    state.items[index].tradeCount = count;
    return saveState(state);
}

bool exchangeOwnedSeals(const char* offeredSealId,
                        const char* receivedSealId) {
    if (!isValidSealId(offeredSealId) ||
        !isValidSealId(receivedSealId) ||
        strcmp(offeredSealId, receivedSealId) == 0) {
        Serial.println("Seal inventory error: invalid exchange IDs");
        return false;
    }

    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }

    int offeredIndex = findItem(state, offeredSealId);
    if (offeredIndex < 0 ||
        state.items[offeredIndex].tradeCount == 0) {
        Serial.println("Seal inventory error: offered seal is not available");
        return false;
    }

    InventoryState updated = state;
    SealInventoryItem& offered = updated.items[offeredIndex];
    --offered.ownedCount;
    --offered.tradeCount;
    if (offered.ownedCount == 0) {
        for (size_t i = offeredIndex + 1; i < updated.itemCount; ++i) {
            updated.items[i - 1] = updated.items[i];
        }
        memset(&updated.items[updated.itemCount - 1], 0,
               sizeof(updated.items[0]));
        --updated.itemCount;
    }

    if (!addToState(updated, receivedSealId, 1)) {
        return false;
    }
    return saveState(updated);
}

bool applyTradeOnce(const char* transactionId,
                    const char* offeredSealId,
                    const char* receivedSealId) {
    if (!isValidSealId(transactionId) ||
        !isValidSealId(offeredSealId) ||
        !isValidSealId(receivedSealId) ||
        strcmp(offeredSealId, receivedSealId) == 0) {
        Serial.println("Seal inventory error: invalid trade transaction");
        return false;
    }

    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }
    for (const auto& completedId : state.completedTradeIds) {
        if (strcmp(completedId, transactionId) == 0) {
            return true;
        }
    }

    int offeredIndex = findItem(state, offeredSealId);
    if (offeredIndex < 0 || state.items[offeredIndex].tradeCount == 0) {
        Serial.println("Seal inventory error: offered seal is not in trade pool");
        return false;
    }

    InventoryState updated = state;
    SealInventoryItem& offered = updated.items[offeredIndex];
    --offered.ownedCount;
    --offered.tradeCount;
    if (offered.ownedCount == 0) {
        for (size_t i = offeredIndex + 1; i < updated.itemCount; ++i) {
            updated.items[i - 1] = updated.items[i];
        }
        memset(&updated.items[updated.itemCount - 1], 0,
               sizeof(updated.items[0]));
        --updated.itemCount;
    }
    if (!addToState(updated, receivedSealId, 1)) {
        return false;
    }

    snprintf(updated.completedTradeIds[updated.tradeReceiptCursor],
             sizeof(updated.completedTradeIds[0]), "%s", transactionId);
    updated.tradeReceiptCursor =
        (updated.tradeReceiptCursor + 1) % MAX_COMPLETED_TRADES;
    return saveState(updated);
}

bool wasTradeApplied(const char* transactionId) {
    if (!isValidSealId(transactionId)) {
        return false;
    }
    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }
    for (const auto& completedId : state.completedTradeIds) {
        if (strcmp(completedId, transactionId) == 0) {
            return true;
        }
    }
    return false;
}

bool awardGatewaySealOncePerDay(const char* gatewayId,
                                const char* sealId,
                                uint32_t localDateKey) {
    if (!isValidSealId(gatewayId) || !isValidSealId(sealId) ||
        localDateKey == 0) {
        Serial.println("Seal inventory error: invalid gateway award data");
        return false;
    }

    InventoryState state = {};
    if (!loadState(state)) {
        return false;
    }

    GatewayAward* availableSlot = nullptr;
    GatewayAward* oldestSlot = nullptr;
    for (GatewayAward& award : state.gatewayAwards) {
        if (!award.gatewayId[0]) {
            if (!availableSlot) {
                availableSlot = &award;
            }
            continue;
        }
        if (strcmp(award.gatewayId, gatewayId) == 0) {
            if (award.lastAwardDate == localDateKey) {
                return true;
            }
            availableSlot = &award;
            break;
        }
        if (!oldestSlot || award.lastAwardDate < oldestSlot->lastAwardDate) {
            oldestSlot = &award;
        }
    }

    if (!availableSlot) {
        if (!oldestSlot || oldestSlot->lastAwardDate >= localDateKey) {
            Serial.println("Seal inventory error: daily gateway award table is full");
            return false;
        }
        availableSlot = oldestSlot;
    }

    InventoryState updated = state;
    size_t slotIndex = static_cast<size_t>(
        availableSlot - state.gatewayAwards);
    GatewayAward& updatedAward = updated.gatewayAwards[slotIndex];
    snprintf(updatedAward.gatewayId, sizeof(updatedAward.gatewayId), "%s",
             gatewayId);
    updatedAward.lastAwardDate = localDateKey;
    if (!addToState(updated, sealId, 1)) {
        return false;
    }
    return saveState(updated);
}
