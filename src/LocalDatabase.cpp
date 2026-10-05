#include "LocalDatabase.h"

#include <Preferences.h>

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "events";
constexpr size_t MAX_PENDING_EVENTS = 32;

bool databaseReady = false;

int slotKey(size_t index, char* key, size_t capacity) {
    return snprintf(key, capacity, "e%02u",
                    static_cast<unsigned>(index));
}
}

bool initializeLocalDatabase() {
    if (databaseReady) {
        return true;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Local DB error: failed to open NVS for cleanup");
        return false;
    }

    size_t removedCount = 0;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        char key[4];
        slotKey(i, key, sizeof(key));
        if (!preferences.isKey(key)) {
            continue;
        }
        if (!preferences.remove(key)) {
            Serial.printf("Local DB error: failed to remove event slot %s\n",
                          key);
            preferences.end();
            return false;
        }
        ++removedCount;
    }
    if (preferences.isKey("boot") && !preferences.remove("boot")) {
        Serial.println("Local DB error: failed to remove event boot ID");
        preferences.end();
        return false;
    }
    preferences.end();

    databaseReady = true;
    Serial.printf(
        "Local event storage disabled; removed %u pending records\n",
        static_cast<unsigned>(removedCount));
    return true;
}

bool queueEncounterEvent(const char* partnerDeviceId,
                         bool,
                         const char*) {
    return databaseReady && partnerDeviceId != nullptr;
}

bool queueSosEvent(LocalEventType type,
                   const char* originDeviceId,
                   const char*) {
    return databaseReady &&
           (type == LOCAL_EVENT_SOS_SENT ||
            type == LOCAL_EVENT_SOS_RECEIVED) &&
           originDeviceId != nullptr;
}

bool saveSosSentEvent() {
    return databaseReady;
}

bool queueTradeCompleteEvent(const char* peerDeviceId,
                             const char* sentStickerId,
                             const char* receivedStickerId) {
    return databaseReady && peerDeviceId && sentStickerId &&
           sentStickerId[0] && receivedStickerId && receivedStickerId[0];
}

void processQueuedLocalEvents() {}

size_t getPendingLocalEventCount() {
    return 0;
}

bool getPendingLocalEvent(size_t, LocalEvent&) {
    return false;
}

bool markLocalEventSynced(const char*) {
    return false;
}

bool getNextPendingGatewayEncounter(LocalEvent&) {
    return false;
}

bool markGatewayRewardProcessed(const char*) {
    return false;
}
