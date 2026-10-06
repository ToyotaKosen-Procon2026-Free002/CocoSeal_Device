#include "LocalDatabase.h"

#include "DeviceIdentity.h"
#include "WifiManager.h"

#include <Preferences.h>
#include <esp_system.h>
#include <stddef.h>

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "events";
constexpr char BOOT_ID_KEY[] = "boot";
constexpr uint8_t SCHEMA_VERSION = 2;
constexpr size_t MAX_PENDING_EVENTS = 32;
constexpr UBaseType_t EVENT_QUEUE_LENGTH = 8;
constexpr uint32_t STORAGE_RETRY_INTERVAL_MS = 5000;

QueueHandle_t eventQueue = nullptr;
char bootId[37] = {};
uint32_t lastStorageRetry = 0;
bool databaseReady = false;
bool storageRetryPending = false;

void slotKey(size_t index, char* key, size_t capacity) {
    snprintf(key, capacity, "e%02u", static_cast<unsigned>(index));
}

uint32_t calculateChecksum(const LocalEvent& event) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&event);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < offsetof(LocalEvent, checksum); ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

bool isValidEvent(const LocalEvent& event) {
    return event.schemaVersion == SCHEMA_VERSION &&
           event.type >= LOCAL_EVENT_ENCOUNTER &&
           event.type <= LOCAL_EVENT_TRADE_COMPLETE &&
           event.eventId[sizeof(event.eventId) - 1] == '\0' &&
           event.originDeviceId[sizeof(event.originDeviceId) - 1] == '\0' &&
           event.partnerDeviceId[sizeof(event.partnerDeviceId) - 1] == '\0' &&
           event.bootId[sizeof(event.bootId) - 1] == '\0' &&
           event.stickerId[sizeof(event.stickerId) - 1] == '\0' &&
           event.sentStickerId[sizeof(event.sentStickerId) - 1] == '\0' &&
           event.receivedStickerId[sizeof(event.receivedStickerId) - 1] == '\0' &&
           event.checksum == calculateChecksum(event);
}

bool readSlot(Preferences& preferences, size_t index, LocalEvent& event) {
    char key[4];
    slotKey(index, key, sizeof(key));
    if (!preferences.isKey(key) ||
        preferences.getBytesLength(key) != sizeof(event)) {
        return false;
    }
    return preferences.getBytes(key, &event, sizeof(event)) == sizeof(event) &&
           isValidEvent(event);
}

bool eventExists(Preferences& preferences, const char* eventId) {
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent existing = {};
        if (readSlot(preferences, i, existing) &&
            strcmp(existing.eventId, eventId) == 0) {
            return true;
        }
    }
    return false;
}

bool persistEvent(const LocalEvent& event) {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Local DB error: failed to open NVS namespace");
        return false;
    }

    if (eventExists(preferences, event.eventId)) {
        preferences.end();
        return true;
    }

    bool saved = false;
    bool freeSlotFound = false;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        char key[4];
        slotKey(i, key, sizeof(key));
        if (preferences.isKey(key)) {
            continue;
        }
        freeSlotFound = true;
        saved = preferences.putBytes(key, &event, sizeof(event)) ==
                sizeof(event);
        if (!saved) {
            Serial.printf("Local DB error: failed writing event slot %s\n",
                          key);
        }
        break;
    }

    if (!saved && !freeSlotFound) {
        Serial.println("Local DB full: pending event was not saved");
    }
    preferences.end();
    return saved;
}

void generateUuid(char* output, size_t capacity) {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    snprintf(output, capacity,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
}

LocalEvent makeEvent(LocalEventType type,
                     const char* originDeviceId,
                     const char* partnerDeviceId) {
    LocalEvent event = {};
    event.schemaVersion = SCHEMA_VERSION;
    event.type = type;
    event.uptimeMs = millis();
    getTrustedUnixTime(event.timestampUnix);
    snprintf(event.bootId, sizeof(event.bootId), "%s", bootId);
    generateUuid(event.eventId, sizeof(event.eventId));
    snprintf(event.originDeviceId, sizeof(event.originDeviceId), "%s",
             originDeviceId ? originDeviceId : "");
    snprintf(event.partnerDeviceId, sizeof(event.partnerDeviceId), "%s",
             partnerDeviceId ? partnerDeviceId : "");
    return event;
}

bool updateEvent(Preferences& preferences,
                 size_t index,
                 LocalEvent& event) {
    event.checksum = calculateChecksum(event);
    char key[4];
    slotKey(index, key, sizeof(key));
    return preferences.putBytes(key, &event, sizeof(event)) == sizeof(event);
}
}

bool initializeLocalDatabase() {
    if (databaseReady) {
        return true;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Local DB error: failed to initialize NVS");
        return false;
    }

    size_t removedCount = 0;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        char key[4];
        slotKey(i, key, sizeof(key));
        if (!preferences.isKey(key)) {
            continue;
        }
        LocalEvent event = {};
        if (readSlot(preferences, i, event)) {
            continue;
        }
        if (!preferences.remove(key)) {
            Serial.printf("Local DB error: failed to remove invalid slot %s\n",
                          key);
            preferences.end();
            return false;
        }
        ++removedCount;
    }

    generateUuid(bootId, sizeof(bootId));
    if (preferences.putBytes(BOOT_ID_KEY, bootId, sizeof(bootId)) !=
        sizeof(bootId)) {
        Serial.println("Local DB error: failed to save boot identifier");
        preferences.end();
        return false;
    }

    size_t discardedUntrustedCount = 0;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent event = {};
        if (!readSlot(preferences, i, event) ||
            event.timestampUnix != 0 ||
            strcmp(event.bootId, bootId) == 0) {
            continue;
        }

        char key[4];
        slotKey(i, key, sizeof(key));
        if (!preferences.remove(key)) {
            Serial.printf(
                "Local DB error: failed to remove untimestamped old event "
                "from slot %s\n",
                key);
            preferences.end();
            return false;
        }
        ++discardedUntrustedCount;
    }
    preferences.end();

    eventQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(LocalEvent));
    if (eventQueue == nullptr) {
        Serial.println("Local DB error: failed to create event queue");
        return false;
    }

    databaseReady = true;
    storageRetryPending = false;
    Serial.printf("Local DB ready: %u pending events",
                  static_cast<unsigned>(getPendingLocalEventCount()));
    if (removedCount > 0) {
        Serial.printf("; removed %u incompatible/corrupt records",
                      static_cast<unsigned>(removedCount));
    }
    if (discardedUntrustedCount > 0) {
        Serial.printf("; discarded %u old events without trusted timestamps",
                      static_cast<unsigned>(discardedUntrustedCount));
    }
    Serial.println();
    return true;
}

bool queueEncounterEvent(const char* partnerDeviceId,
                         bool partnerIsGateway,
                         const char* stickerId) {
    if (!databaseReady || !partnerDeviceId) {
        Serial.println("Local DB error: cannot queue encounter event");
        return false;
    }
    LocalEvent event = makeEvent(
        LOCAL_EVENT_ENCOUNTER, getDeviceId(), partnerDeviceId);
    event.partnerIsGateway = partnerIsGateway ? 1 : 0;
    snprintf(event.stickerId, sizeof(event.stickerId), "%s",
             stickerId ? stickerId : "");
    event.checksum = calculateChecksum(event);

    if (xQueueSend(eventQueue, &event, 0) != pdTRUE) {
        Serial.println("Local DB error: encounter event queue is full");
        return false;
    }
    return true;
}

bool queueSosEvent(LocalEventType type,
                   const char* originDeviceId,
                   const char* partnerDeviceId) {
    if (!databaseReady ||
        (type != LOCAL_EVENT_SOS_SENT &&
         type != LOCAL_EVENT_SOS_RECEIVED) ||
        !originDeviceId) {
        Serial.println("Local DB error: invalid SOS event");
        return false;
    }
    LocalEvent event = makeEvent(type, originDeviceId, partnerDeviceId);
    event.checksum = calculateChecksum(event);
    if (xQueueSend(eventQueue, &event, 0) != pdTRUE) {
        Serial.println("Local DB error: SOS event queue is full");
        return false;
    }
    return true;
}

bool saveSosSentEvent() {
    if (!databaseReady) {
        Serial.println("Local DB error: cannot save SOS before initialization");
        return false;
    }
    LocalEvent event = makeEvent(LOCAL_EVENT_SOS_SENT, getDeviceId(), "");
    event.checksum = calculateChecksum(event);
    return persistEvent(event);
}

bool queueTradeCompleteEvent(const char* peerDeviceId,
                             const char* sentStickerId,
                             const char* receivedStickerId) {
    if (!databaseReady || !peerDeviceId || !sentStickerId ||
        !sentStickerId[0] || !receivedStickerId || !receivedStickerId[0]) {
        Serial.println("Local DB error: invalid completed trade");
        return false;
    }
    LocalEvent event = makeEvent(
        LOCAL_EVENT_TRADE_COMPLETE, getDeviceId(), peerDeviceId);
    snprintf(event.sentStickerId, sizeof(event.sentStickerId), "%s",
             sentStickerId);
    snprintf(event.receivedStickerId, sizeof(event.receivedStickerId), "%s",
             receivedStickerId);
    event.checksum = calculateChecksum(event);
    if (xQueueSend(eventQueue, &event, 0) != pdTRUE) {
        Serial.println("Local DB error: trade event queue is full");
        return false;
    }
    return true;
}

void processQueuedLocalEvents() {
    if (!databaseReady || uxQueueMessagesWaiting(eventQueue) == 0) {
        return;
    }

    uint32_t now = millis();
    if (storageRetryPending &&
        now - lastStorageRetry < STORAGE_RETRY_INTERVAL_MS) {
        return;
    }

    while (uxQueueMessagesWaiting(eventQueue) > 0) {
        LocalEvent event = {};
        if (xQueuePeek(eventQueue, &event, 0) != pdTRUE) {
            return;
        }
        if (!persistEvent(event)) {
            lastStorageRetry = now;
            storageRetryPending = true;
            return;
        }
        LocalEvent processed = {};
        xQueueReceive(eventQueue, &processed, 0);
        Serial.printf("Local DB saved event %s (type %u)\n",
                      processed.eventId,
                      static_cast<unsigned>(processed.type));
    }
    storageRetryPending = false;
}

size_t getPendingLocalEventCount() {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
        Serial.println("Local DB error: failed to count pending events");
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent event = {};
        if (readSlot(preferences, i, event)) {
            ++count;
        }
    }
    preferences.end();
    return count;
}

bool getPendingLocalEvent(size_t index, LocalEvent& event) {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
        Serial.println("Local DB error: failed to read pending event");
        return false;
    }

    size_t currentIndex = 0;
    bool found = false;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent candidate = {};
        if (!readSlot(preferences, i, candidate)) {
            continue;
        }
        if (currentIndex++ == index) {
            event = candidate;
            found = true;
            break;
        }
    }
    preferences.end();
    return found;
}

bool prepareLocalEventTimestamp(LocalEvent& event,
                                uint32_t currentTimestampUnix,
                                uint32_t currentUptimeMs) {
    if (event.timestampUnix != 0) {
        return true;
    }
    if (strcmp(event.bootId, bootId) != 0) {
        return false;
    }

    uint32_t elapsedSeconds =
        (currentUptimeMs - event.uptimeMs) / 1000;
    if (elapsedSeconds >= currentTimestampUnix) {
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Local DB error: failed to save event timestamp");
        return false;
    }

    bool updated = false;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent candidate = {};
        if (!readSlot(preferences, i, candidate) ||
            strcmp(candidate.eventId, event.eventId) != 0) {
            continue;
        }
        candidate.timestampUnix = currentTimestampUnix - elapsedSeconds;
        updated = updateEvent(preferences, i, candidate);
        if (updated) {
            event = candidate;
        }
        break;
    }
    preferences.end();
    if (!updated) {
        Serial.println("Local DB error: failed to persist event timestamp");
    }
    return updated;
}

bool markLocalEventSynced(const char* eventId) {
    if (!eventId || !eventId[0]) {
        Serial.println("Local DB error: cannot remove event without an ID");
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Local DB error: failed to update event status");
        return false;
    }

    bool removed = false;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent event = {};
        if (!readSlot(preferences, i, event) ||
            strcmp(event.eventId, eventId) != 0) {
            continue;
        }
        char key[4];
        slotKey(i, key, sizeof(key));
        removed = preferences.remove(key);
        break;
    }
    preferences.end();
    return removed;
}

bool getNextPendingGatewayEncounter(LocalEvent& event) {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
        Serial.println("Local DB error: failed to read gateway encounters");
        return false;
    }

    bool found = false;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent candidate = {};
        if (!readSlot(preferences, i, candidate) ||
            candidate.type != LOCAL_EVENT_ENCOUNTER ||
            !candidate.partnerIsGateway ||
            candidate.gatewayRewardProcessed) {
            continue;
        }
        event = candidate;
        found = true;
        break;
    }
    preferences.end();
    return found;
}

bool markGatewayRewardProcessed(const char* eventId) {
    if (!eventId || !eventId[0]) {
        Serial.println("Local DB error: gateway reward event has no ID");
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Local DB error: failed to update gateway encounter");
        return false;
    }

    bool updated = false;
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        LocalEvent event = {};
        if (!readSlot(preferences, i, event) ||
            strcmp(event.eventId, eventId) != 0) {
            continue;
        }
        event.gatewayRewardProcessed = 1;
        updated = updateEvent(preferences, i, event);
        break;
    }
    preferences.end();
    if (!updated) {
        Serial.println("Local DB error: failed to mark gateway reward processed");
    }
    return updated;
}
