#pragma once

#include <Arduino.h>

enum LocalEventType : uint8_t {
    LOCAL_EVENT_ENCOUNTER = 1,
    LOCAL_EVENT_SOS_SENT = 2,
    LOCAL_EVENT_SOS_RECEIVED = 3,
    LOCAL_EVENT_TRADE_COMPLETE = 4
};

struct LocalEvent {
    uint8_t schemaVersion;
    LocalEventType type;
    uint8_t partnerIsGateway;
    uint8_t gatewayRewardProcessed;
    uint32_t uptimeMs;
    char bootId[37];
    char eventId[37];
    char originDeviceId[37];
    char partnerDeviceId[37];
    char stickerId[16];
    char sentStickerId[37];
    char receivedStickerId[37];
    uint32_t checksum;
};

bool initializeLocalDatabase();
bool queueEncounterEvent(const char* partnerDeviceId,
                        bool partnerIsGateway,
                        const char* stickerId);
bool queueSosEvent(LocalEventType type,
                   const char* originDeviceId,
                   const char* partnerDeviceId);
bool saveSosSentEvent();
bool queueTradeCompleteEvent(const char* peerDeviceId,
                             const char* sentStickerId,
                             const char* receivedStickerId);
void processQueuedLocalEvents();
size_t getPendingLocalEventCount();
bool getPendingLocalEvent(size_t index, LocalEvent& event);
bool markLocalEventSynced(const char* eventId);
bool getNextPendingGatewayEncounter(LocalEvent& event);
bool markGatewayRewardProcessed(const char* eventId);
