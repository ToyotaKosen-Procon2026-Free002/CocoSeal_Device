#include "SealInventory.h"

#include <Preferences.h>
#include <stddef.h>

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "seals";
constexpr char INVENTORY_KEY[] = "state";
constexpr uint8_t SCHEMA_VERSION = 2;
constexpr size_t MAX_SEAL_TYPES = 32;
constexpr size_t MAX_DAILY_GATEWAYS = 10;

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

struct InventoryState {
    uint8_t schemaVersion;
    uint8_t itemCount;
    uint8_t reserved[2];
    SealInventoryItem items[MAX_SEAL_TYPES];
    GatewayAward gatewayAwards[MAX_DAILY_GATEWAYS];
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
    return true;
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
