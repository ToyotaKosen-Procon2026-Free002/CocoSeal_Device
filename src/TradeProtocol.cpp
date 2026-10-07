#include "TradeProtocol.h"

#include "DeviceIdentity.h"
#include "EspNowManager.h"
#include "LocalDatabase.h"
#include "SealInventory.h"

#include <Preferences.h>
#include <ctype.h>
#include <esp_now.h>
#include <esp_system.h>
#include <stddef.h>

namespace {
constexpr uint32_t PACKET_MAGIC = 0x43535452;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t PREFERENCES_VERSION = 1;
constexpr char PREFERENCES_NAMESPACE[] = "trade";
constexpr char PENDING_KEY[] = "pending";
constexpr uint8_t EVENT_QUEUE_LENGTH = 8;
constexpr uint32_t RETRY_INTERVAL_MS = 1500;
constexpr uint32_t PRE_COMMIT_TIMEOUT_MS = 60000;

enum class TradeMessageType : uint8_t {
    OFFER = 1,
    ACCEPT = 2,
    COMMIT = 3,
    ACK = 4,
    REJECT = 5,
    ENCOUNTER_DISPLAY = 6,
    TRADE_RESULT = 7
};

enum class TradeResult : uint16_t {
    SUCCESS = 0,
    FAILURE = 1
};

enum class TradeRole : uint8_t {
    INITIATOR = 1,
    RESPONDER = 2
};

enum class TradePhase : uint8_t {
    WAITING_FOR_ACCEPT = 1,
    WAITING_FOR_COMMIT = 2,
    WAITING_FOR_ACK = 3
};

#pragma pack(push, 1)
struct TradePacket {
    uint32_t magic;
    uint8_t version;
    TradeMessageType messageType;
    uint16_t reserved;
    char transactionId[37];
    char senderDeviceId[37];
    char targetDeviceId[37];
    char initiatorSealId[37];
    char responderSealId[37];
    uint32_t checksum;
};

struct PendingTrade {
    uint8_t version;
    TradeRole role;
    TradePhase phase;
    uint8_t reserved;
    uint8_t peerMac[6];
    uint32_t startedUptime;
    char transactionId[37];
    char peerDeviceId[37];
    char localSealId[37];
    char peerSealId[37];
    uint32_t checksum;
};

struct QueuedPacket {
    uint8_t peerMac[6];
    TradePacket packet;
};

struct QueuedEncounter {
    uint8_t peerMac[6];
    char peerDeviceId[37];
};
#pragma pack(pop)

static_assert(sizeof(TradePacket) <= ESP_NOW_MAX_DATA_LEN,
              "Trade packet exceeds ESP-NOW payload limit");

QueueHandle_t packetQueue = nullptr;
QueueHandle_t encounterQueue = nullptr;
PendingTrade pendingTrade = {};
bool hasPendingTrade = false;
bool protocolReady = false;
uint32_t lastSendTime = 0;
const char* initializationError = "";
char tradeDebugStatus[64] = {};
bool tradeDebugStatusChanged = false;
bool tradeInProgress = false;

void setTradeDebugStatus(const char* status, bool inProgress) {
    snprintf(tradeDebugStatus, sizeof(tradeDebugStatus), "%s", status);
    tradeInProgress = inProgress;
    tradeDebugStatusChanged = true;
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

uint32_t calculateCrc(const uint8_t* bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

uint32_t packetChecksum(const TradePacket& packet) {
    return calculateCrc(reinterpret_cast<const uint8_t*>(&packet),
                        offsetof(TradePacket, checksum));
}

uint32_t pendingChecksum(const PendingTrade& pending) {
    return calculateCrc(reinterpret_cast<const uint8_t*>(&pending),
                        offsetof(PendingTrade, checksum));
}

bool validUuid(const char* value) {
    if (!value || strlen(value) != 36) {
        return false;
    }
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') {
                return false;
            }
        } else if (!isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

bool validIdentifier(const char* value) {
    return value && value[0] && strnlen(value, 37) < 37;
}

bool validPacket(const TradePacket& packet) {
    if (packet.magic != PACKET_MAGIC ||
        packet.version != PROTOCOL_VERSION ||
        packet.messageType < TradeMessageType::OFFER ||
        packet.messageType > TradeMessageType::TRADE_RESULT ||
        !validUuid(packet.transactionId) ||
        !validUuid(packet.senderDeviceId) ||
        !validUuid(packet.targetDeviceId) ||
        packet.checksum != packetChecksum(packet)) {
        return false;
    }
    if (packet.messageType == TradeMessageType::OFFER) {
        return validIdentifier(packet.initiatorSealId) &&
               packet.responderSealId[0] == '\0';
    }
    if (packet.messageType == TradeMessageType::ENCOUNTER_DISPLAY) {
        return packet.initiatorSealId[0] == '\0' &&
               packet.responderSealId[0] == '\0';
    }
    if (packet.messageType == TradeMessageType::TRADE_RESULT) {
        return packet.reserved <=
                   static_cast<uint16_t>(TradeResult::FAILURE) &&
               packet.initiatorSealId[0] == '\0' &&
               packet.responderSealId[0] == '\0';
    }
    return validIdentifier(packet.initiatorSealId) &&
           validIdentifier(packet.responderSealId);
}

bool validPending(const PendingTrade& pending) {
    return pending.version == PREFERENCES_VERSION &&
           pending.role >= TradeRole::INITIATOR &&
           pending.role <= TradeRole::RESPONDER &&
           pending.phase >= TradePhase::WAITING_FOR_ACCEPT &&
           pending.phase <= TradePhase::WAITING_FOR_ACK &&
           validUuid(pending.transactionId) &&
           validUuid(pending.peerDeviceId) &&
           validIdentifier(pending.localSealId) &&
           (pending.peerSealId[0] == '\0' ||
            validIdentifier(pending.peerSealId)) &&
           pending.checksum == pendingChecksum(pending);
}

bool sameTransaction(const TradePacket& packet, const uint8_t* peerMac) {
    return hasPendingTrade &&
           strcmp(packet.transactionId, pendingTrade.transactionId) == 0 &&
           strcmp(packet.senderDeviceId, pendingTrade.peerDeviceId) == 0 &&
           memcmp(peerMac, pendingTrade.peerMac,
                  sizeof(pendingTrade.peerMac)) == 0;
}

bool loadPendingTrade() {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        initializationError = "NVS OPEN";
        Serial.println("Trade protocol error: failed to open pending storage");
        return false;
    }
    size_t length = preferences.isKey(PENDING_KEY)
                        ? preferences.getBytesLength(PENDING_KEY)
                        : 0;
    if (length == 0) {
        preferences.end();
        hasPendingTrade = false;
        return true;
    }
    bool loaded = length == sizeof(pendingTrade) &&
                  preferences.getBytes(
                      PENDING_KEY, &pendingTrade, sizeof(pendingTrade)) ==
                      sizeof(pendingTrade);
    preferences.end();
    if (!loaded || !validPending(pendingTrade)) {
        initializationError = "NVS DATA";
        Serial.println("Trade protocol error: pending transaction is corrupt");
        return false;
    }
    hasPendingTrade = true;
    return true;
}

bool savePendingTrade() {
    pendingTrade.version = PREFERENCES_VERSION;
    pendingTrade.checksum = pendingChecksum(pendingTrade);
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Trade protocol error: failed to open pending storage");
        return false;
    }
    bool saved = preferences.putBytes(
                     PENDING_KEY, &pendingTrade, sizeof(pendingTrade)) ==
                 sizeof(pendingTrade);
    preferences.end();
    if (!saved) {
        Serial.println("Trade protocol error: failed to save transaction");
        return false;
    }
    hasPendingTrade = true;
    return true;
}

bool clearPendingTrade() {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Trade protocol error: failed to clear transaction");
        return false;
    }
    bool removed = !preferences.isKey(PENDING_KEY) ||
                   preferences.remove(PENDING_KEY);
    preferences.end();
    if (!removed) {
        Serial.println("Trade protocol error: failed to remove transaction");
        return false;
    }
    memset(&pendingTrade, 0, sizeof(pendingTrade));
    hasPendingTrade = false;
    return true;
}

bool ensurePeer(const uint8_t* peerMac) {
    if (esp_now_is_peer_exist(peerMac)) {
        return true;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peerMac, sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.encrypt = false;
    esp_err_t result = esp_now_add_peer(&peer);
    if (result != ESP_OK) {
        Serial.printf("Trade protocol error: failed to add peer: %d\n", result);
        return false;
    }
    return true;
}

TradePacket makePacket(TradeMessageType type,
                       const char* transactionId,
                       const char* peerDeviceId,
                       const char* initiatorSealId,
                       const char* responderSealId) {
    TradePacket packet = {};
    packet.magic = PACKET_MAGIC;
    packet.version = PROTOCOL_VERSION;
    packet.messageType = type;
    snprintf(packet.transactionId, sizeof(packet.transactionId), "%s",
             transactionId);
    snprintf(packet.senderDeviceId, sizeof(packet.senderDeviceId), "%s",
             getDeviceId());
    snprintf(packet.targetDeviceId, sizeof(packet.targetDeviceId), "%s",
             peerDeviceId);
    if (initiatorSealId) {
        snprintf(packet.initiatorSealId, sizeof(packet.initiatorSealId), "%s",
                 initiatorSealId);
    }
    if (responderSealId) {
        snprintf(packet.responderSealId, sizeof(packet.responderSealId), "%s",
                 responderSealId);
    }
    packet.checksum = packetChecksum(packet);
    return packet;
}

bool sendPacketToPeer(const TradePacket& packet, const uint8_t* peerMac) {
    lastSendTime = millis();
    if (!ensurePeer(peerMac)) {
        return false;
    }
    esp_err_t result = esp_now_send(
        peerMac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    if (result != ESP_OK) {
        Serial.printf("Trade protocol send failed: %d\n", result);
        return false;
    }
    return true;
}

void sendEncounterDisplay(const QueuedEncounter& encounter) {
    char encounterId[37];
    generateUuid(encounterId, sizeof(encounterId));
    TradePacket packet = makePacket(
        TradeMessageType::ENCOUNTER_DISPLAY, encounterId,
        encounter.peerDeviceId, nullptr, nullptr);
    if (sendPacketToPeer(packet, encounter.peerMac)) {
        scheduleSynchronizedEncounterDisplay(encounter.peerDeviceId);
    }
}

void sendTradeOutcome(const uint8_t* peerMac,
                      const char* peerDeviceId,
                      const char* transactionId,
                      TradeResult result) {
    if (!peerMac || !validUuid(peerDeviceId)) {
        return;
    }
    char generatedTransactionId[37];
    if (!validUuid(transactionId)) {
        generateUuid(generatedTransactionId, sizeof(generatedTransactionId));
        transactionId = generatedTransactionId;
    }
    TradePacket packet = makePacket(
        TradeMessageType::TRADE_RESULT, transactionId, peerDeviceId,
        nullptr, nullptr);
    packet.reserved = static_cast<uint16_t>(result);
    packet.checksum = packetChecksum(packet);
    sendPacketToPeer(packet, peerMac);
}

void reportTradeOutcome(const uint8_t* peerMac,
                        const char* peerDeviceId,
                        const char* transactionId,
                        bool succeeded) {
    sendTradeOutcome(peerMac, peerDeviceId, transactionId,
                     succeeded ? TradeResult::SUCCESS : TradeResult::FAILURE);
    setTradeDebugStatus(succeeded ? "COMPLETE: TRADE" : "TRADE_FAILED",
                        false);
}

bool sendPendingMessage() {
    if (!hasPendingTrade) {
        return false;
    }
    TradeMessageType type;
    const char* initiatorSealId;
    const char* responderSealId = nullptr;
    if (pendingTrade.role == TradeRole::INITIATOR &&
        pendingTrade.phase == TradePhase::WAITING_FOR_ACCEPT) {
        type = TradeMessageType::OFFER;
        initiatorSealId = pendingTrade.localSealId;
    } else if (pendingTrade.role == TradeRole::INITIATOR &&
               pendingTrade.phase == TradePhase::WAITING_FOR_ACK) {
        type = TradeMessageType::COMMIT;
        initiatorSealId = pendingTrade.localSealId;
        responderSealId = pendingTrade.peerSealId;
    } else if (pendingTrade.role == TradeRole::RESPONDER &&
               pendingTrade.phase == TradePhase::WAITING_FOR_COMMIT) {
        type = TradeMessageType::ACCEPT;
        initiatorSealId = pendingTrade.peerSealId;
        responderSealId = pendingTrade.localSealId;
    } else {
        return false;
    }
    TradePacket packet = makePacket(
        type, pendingTrade.transactionId, pendingTrade.peerDeviceId,
        initiatorSealId, responderSealId);
    return sendPacketToPeer(packet, pendingTrade.peerMac);
}

void sendAck(const TradePacket& received, const uint8_t* peerMac) {
    TradePacket ack = makePacket(
        TradeMessageType::ACK, received.transactionId,
        received.senderDeviceId, received.initiatorSealId,
        received.responderSealId);
    sendPacketToPeer(ack, peerMac);
}

void sendReject(const TradePacket& received, const uint8_t* peerMac) {
    TradePacket reject = makePacket(
        TradeMessageType::REJECT, received.transactionId,
        received.senderDeviceId, received.initiatorSealId,
        received.responderSealId);
    sendPacketToPeer(reject, peerMac);
}

bool beginTrade(const QueuedEncounter& encounter) {
    sendEncounterDisplay(encounter);

    char localSealId[37] = {};
    if (!getFirstTradeableSeal(nullptr, localSealId, sizeof(localSealId))) {
        Serial.printf("Trade not started with %s: no tradeable seal in pool\n",
                      encounter.peerDeviceId);
        reportTradeOutcome(encounter.peerMac, encounter.peerDeviceId,
                           nullptr, false);
        return false;
    }

    PendingTrade candidate = {};
    candidate.role = TradeRole::INITIATOR;
    candidate.phase = TradePhase::WAITING_FOR_ACCEPT;
    candidate.startedUptime = millis();
    memcpy(candidate.peerMac, encounter.peerMac, sizeof(candidate.peerMac));
    snprintf(candidate.peerDeviceId, sizeof(candidate.peerDeviceId), "%s",
             encounter.peerDeviceId);
    snprintf(candidate.localSealId, sizeof(candidate.localSealId), "%s",
             localSealId);
    uint8_t uuidBytes[16];
    esp_fill_random(uuidBytes, sizeof(uuidBytes));
    uuidBytes[6] = (uuidBytes[6] & 0x0F) | 0x40;
    uuidBytes[8] = (uuidBytes[8] & 0x3F) | 0x80;
    snprintf(candidate.transactionId, sizeof(candidate.transactionId),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             uuidBytes[0], uuidBytes[1], uuidBytes[2], uuidBytes[3],
             uuidBytes[4], uuidBytes[5], uuidBytes[6], uuidBytes[7],
             uuidBytes[8], uuidBytes[9], uuidBytes[10], uuidBytes[11],
             uuidBytes[12], uuidBytes[13], uuidBytes[14], uuidBytes[15]);
    pendingTrade = candidate;
    if (!savePendingTrade()) {
        reportTradeOutcome(encounter.peerMac, encounter.peerDeviceId,
                           candidate.transactionId, false);
        return false;
    }
    Serial.printf("Trade started with %s, offering %s\n",
                  pendingTrade.peerDeviceId, pendingTrade.localSealId);
    char status[64];
    snprintf(status, sizeof(status), "OFFER: %.36s",
             pendingTrade.localSealId);
    setTradeDebugStatus(status, true);
    if (!sendPendingMessage()) {
        setTradeDebugStatus("OFFER SEND FAILED", true);
        return false;
    }
    setTradeDebugStatus("OFFER SENT; WAIT ACCEPT", true);
    return true;
}

void handleOffer(const QueuedPacket& queued) {
    const TradePacket& packet = queued.packet;
    if (strcmp(packet.senderDeviceId, getDeviceId()) >= 0) {
        return;
    }
    if (hasPendingTrade) {
        if (pendingTrade.role == TradeRole::RESPONDER &&
            pendingTrade.phase == TradePhase::WAITING_FOR_COMMIT &&
            sameTransaction(packet, queued.peerMac) &&
            strcmp(packet.initiatorSealId, pendingTrade.peerSealId) == 0) {
            sendPendingMessage();
        }
        return;
    }

    if (wasTradeApplied(packet.transactionId)) {
        return;
    }
    char localSealId[37] = {};
    if (!getFirstTradeableSeal(packet.initiatorSealId,
                               localSealId, sizeof(localSealId))) {
        Serial.printf("Trade declined: no eligible seal for %s\n",
                      packet.senderDeviceId);
        reportTradeOutcome(queued.peerMac, packet.senderDeviceId,
                           packet.transactionId, false);
        return;
    }

    pendingTrade = {};
    pendingTrade.role = TradeRole::RESPONDER;
    pendingTrade.phase = TradePhase::WAITING_FOR_COMMIT;
    pendingTrade.startedUptime = millis();
    memcpy(pendingTrade.peerMac, queued.peerMac, sizeof(pendingTrade.peerMac));
    snprintf(pendingTrade.transactionId, sizeof(pendingTrade.transactionId),
             "%s", packet.transactionId);
    snprintf(pendingTrade.peerDeviceId, sizeof(pendingTrade.peerDeviceId),
             "%s", packet.senderDeviceId);
    snprintf(pendingTrade.peerSealId, sizeof(pendingTrade.peerSealId),
             "%s", packet.initiatorSealId);
    snprintf(pendingTrade.localSealId, sizeof(pendingTrade.localSealId),
             "%s", localSealId);
    if (!savePendingTrade()) {
        reportTradeOutcome(queued.peerMac, packet.senderDeviceId,
                           packet.transactionId, false);
        clearPendingTrade();
        return;
    }
    setTradeDebugStatus("OFFER RECEIVED; PREPARING", true);
    Serial.printf("Trade offer accepted: offering %s\n",
                  pendingTrade.localSealId);
    if (sendPendingMessage()) {
        char status[64];
        snprintf(status, sizeof(status), "ACCEPT SENT: %.36s",
                 pendingTrade.localSealId);
        setTradeDebugStatus(status, true);
    } else {
        setTradeDebugStatus("ACCEPT SEND FAILED", true);
    }
}

void handleAccept(const QueuedPacket& queued) {
    if (!hasPendingTrade ||
        pendingTrade.role != TradeRole::INITIATOR ||
        pendingTrade.phase != TradePhase::WAITING_FOR_ACCEPT ||
        !sameTransaction(queued.packet, queued.peerMac) ||
        strcmp(queued.packet.initiatorSealId,
               pendingTrade.localSealId) != 0 ||
        strcmp(queued.packet.responderSealId,
               pendingTrade.localSealId) == 0) {
        return;
    }

    snprintf(pendingTrade.peerSealId, sizeof(pendingTrade.peerSealId), "%s",
             queued.packet.responderSealId);
    pendingTrade.phase = TradePhase::WAITING_FOR_ACK;
    if (savePendingTrade()) {
        Serial.println("Trade accepted by peer; committing");
        char status[64];
        snprintf(status, sizeof(status), "COMMIT SENT: %.36s",
                 pendingTrade.peerSealId);
        setTradeDebugStatus(status, true);
        if (!sendPendingMessage()) {
            setTradeDebugStatus("COMMIT SEND FAILED", true);
        }
    } else {
        reportTradeOutcome(queued.peerMac, queued.packet.senderDeviceId,
                           queued.packet.transactionId, false);
        clearPendingTrade();
    }
}

void handleCommit(const QueuedPacket& queued) {
    const TradePacket& packet = queued.packet;
    if (wasTradeApplied(packet.transactionId)) {
        sendAck(packet, queued.peerMac);
        return;
    }
    const char* rejectionReason = nullptr;
    if (!hasPendingTrade) {
        rejectionReason = "no pending offer";
    } else if (pendingTrade.role != TradeRole::RESPONDER) {
        rejectionReason = "not responder";
    } else if (pendingTrade.phase != TradePhase::WAITING_FOR_COMMIT) {
        rejectionReason = "unexpected phase";
    } else if (!sameTransaction(packet, queued.peerMac)) {
        rejectionReason = "transaction/peer mismatch";
    } else if (strcmp(packet.initiatorSealId, pendingTrade.peerSealId) != 0) {
        rejectionReason = "initiator seal mismatch";
    } else if (strcmp(packet.responderSealId, pendingTrade.localSealId) != 0) {
        rejectionReason = "responder seal mismatch";
    }
    if (rejectionReason) {
        Serial.printf("Trade commit rejected: %s (txn %s, peer %s)\n",
                      rejectionReason, packet.transactionId,
                      packet.senderDeviceId);
        reportTradeOutcome(queued.peerMac, packet.senderDeviceId,
                           packet.transactionId, false);
        sendReject(packet, queued.peerMac);
        return;
    }

    if (!applyTradeOnce(packet.transactionId,
                        pendingTrade.localSealId,
                        pendingTrade.peerSealId)) {
        reportTradeOutcome(queued.peerMac, packet.senderDeviceId,
                           packet.transactionId, false);
        sendReject(packet, queued.peerMac);
        clearPendingTrade();
        return;
    }
    if (!queueTradeCompleteEvent(packet.senderDeviceId,
                                 getPeerDeviceName(packet.senderDeviceId),
                                 pendingTrade.localSealId,
                                 pendingTrade.peerSealId)) {
        Serial.println("Trade warning: completed trade was not queued for sync");
    }
    setTradeDebugStatus("COMPLETE: TRADE", false);
    clearPendingTrade();
    Serial.printf("Trade completed with %s\n", packet.senderDeviceId);
    sendAck(packet, queued.peerMac);
}

void handleAck(const QueuedPacket& queued) {
    const TradePacket& packet = queued.packet;
    if (!hasPendingTrade ||
        pendingTrade.role != TradeRole::INITIATOR ||
        pendingTrade.phase != TradePhase::WAITING_FOR_ACK ||
        !sameTransaction(packet, queued.peerMac) ||
        strcmp(packet.initiatorSealId, pendingTrade.localSealId) != 0 ||
        strcmp(packet.responderSealId, pendingTrade.peerSealId) != 0) {
        return;
    }

    if (!applyTradeOnce(packet.transactionId,
                        pendingTrade.localSealId,
                        pendingTrade.peerSealId)) {
        reportTradeOutcome(queued.peerMac, packet.senderDeviceId,
                           packet.transactionId, false);
        clearPendingTrade();
        return;
    }
    if (!queueTradeCompleteEvent(packet.senderDeviceId,
                                 getPeerDeviceName(packet.senderDeviceId),
                                 pendingTrade.localSealId,
                                 pendingTrade.peerSealId)) {
        Serial.println("Trade warning: completed trade was not queued for sync");
    }
    reportTradeOutcome(queued.peerMac, packet.senderDeviceId,
                       packet.transactionId, true);
    clearPendingTrade();
    Serial.printf("Trade completed with %s\n", packet.senderDeviceId);
}

void handleReject(const QueuedPacket& queued) {
    const TradePacket& packet = queued.packet;
    if (!hasPendingTrade ||
        pendingTrade.role != TradeRole::INITIATOR ||
        pendingTrade.phase != TradePhase::WAITING_FOR_ACK ||
        strcmp(packet.initiatorSealId, pendingTrade.localSealId) != 0 ||
        strcmp(packet.responderSealId, pendingTrade.peerSealId) != 0 ||
        !sameTransaction(packet, queued.peerMac)) {
        return;
    }
    Serial.printf("Trade rejected by %s\n", packet.senderDeviceId);
    reportTradeOutcome(queued.peerMac, packet.senderDeviceId,
                       packet.transactionId, false);
    clearPendingTrade();
}

void handleEncounterDisplay(const QueuedPacket& queued) {
    scheduleSynchronizedEncounterDisplay(queued.packet.senderDeviceId);
}

void handleTradeResult(const QueuedPacket& queued) {
    const TradePacket& packet = queued.packet;
    if (hasPendingTrade &&
        (!sameTransaction(packet, queued.peerMac) ||
         strcmp(packet.targetDeviceId, getDeviceId()) != 0)) {
        return;
    }

    bool succeeded = packet.reserved ==
                     static_cast<uint16_t>(TradeResult::SUCCESS);
    setTradeDebugStatus(succeeded ? "COMPLETE: TRADE" : "TRADE_FAILED",
                        false);
    if (hasPendingTrade) {
        clearPendingTrade();
    }
}

void handlePacket(const QueuedPacket& queued) {
    const TradePacket& packet = queued.packet;
    if (!validPacket(packet) ||
        strcmp(packet.targetDeviceId, getDeviceId()) != 0 ||
        strcmp(packet.senderDeviceId, getDeviceId()) == 0) {
        return;
    }

    switch (packet.messageType) {
        case TradeMessageType::OFFER:
            handleOffer(queued);
            break;
        case TradeMessageType::ACCEPT:
            handleAccept(queued);
            break;
        case TradeMessageType::COMMIT:
            handleCommit(queued);
            break;
        case TradeMessageType::ACK:
            handleAck(queued);
            break;
        case TradeMessageType::REJECT:
            handleReject(queued);
            break;
        case TradeMessageType::ENCOUNTER_DISPLAY:
            handleEncounterDisplay(queued);
            break;
        case TradeMessageType::TRADE_RESULT:
            handleTradeResult(queued);
            break;
    }
}
}

bool initializeTradeProtocol() {
    if (protocolReady) {
        return true;
    }
    packetQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(QueuedPacket));
    if (!packetQueue) {
        initializationError = "PACKET QUEUE";
        Serial.println("Trade protocol error: failed to create packet queue");
        return false;
    }
    encounterQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(QueuedEncounter));
    if (!encounterQueue) {
        initializationError = "PEER QUEUE";
        Serial.println("Trade protocol error: failed to create peer queue");
        vQueueDelete(packetQueue);
        packetQueue = nullptr;
        return false;
    }
    if (!loadPendingTrade()) {
        return false;
    }
    protocolReady = true;
    initializationError = "";
    if (hasPendingTrade) {
        Serial.printf("Resuming pending trade with %s\n",
                      pendingTrade.peerDeviceId);
        if (pendingTrade.role == TradeRole::INITIATOR &&
            pendingTrade.phase == TradePhase::WAITING_FOR_ACCEPT) {
            setTradeDebugStatus("RESUMED; WAIT ACCEPT", true);
        } else if (pendingTrade.role == TradeRole::INITIATOR) {
            setTradeDebugStatus("RESUMED; WAIT ACK", true);
        } else {
            setTradeDebugStatus("RESUMED; WAIT COMMIT", true);
        }
    }
    return true;
}

const char* getTradeProtocolInitError() {
    return initializationError;
}

bool takeTradeDebugStatus(char* status,
                          size_t capacity,
                          bool& inProgress) {
    if (!status || capacity == 0 || !tradeDebugStatusChanged) {
        return false;
    }
    snprintf(status, capacity, "%s", tradeDebugStatus);
    inProgress = tradeInProgress;
    tradeDebugStatusChanged = false;
    return true;
}

void notifyTradePeerEncounter(const uint8_t* peerMac,
                              const char* peerDeviceId) {
    if (!protocolReady || !peerMac || !validUuid(peerDeviceId) ||
        strcmp(getDeviceId(), peerDeviceId) >= 0) {
        return;
    }
    QueuedEncounter encounter = {};
    memcpy(encounter.peerMac, peerMac, sizeof(encounter.peerMac));
    snprintf(encounter.peerDeviceId, sizeof(encounter.peerDeviceId), "%s",
             peerDeviceId);
    if (xQueueSend(encounterQueue, &encounter, 0) != pdTRUE) {
        Serial.println("Trade protocol warning: encounter queue is full");
    }
}

bool receiveTradeProtocolPacket(const uint8_t* peerMac,
                                const uint8_t* data,
                                size_t dataLength) {
    if (!peerMac || !data || dataLength != sizeof(TradePacket)) {
        return false;
    }
    TradePacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    if (packet.magic != PACKET_MAGIC) {
        return false;
    }
    if (!protocolReady) {
        Serial.println("Trade protocol packet ignored: protocol not ready");
        return true;
    }

    if (validPacket(packet) &&
        packet.messageType == TradeMessageType::ENCOUNTER_DISPLAY &&
        strcmp(packet.targetDeviceId, getDeviceId()) == 0 &&
        strcmp(packet.senderDeviceId, getDeviceId()) != 0) {
        scheduleSynchronizedEncounterDisplay(packet.senderDeviceId);
        return true;
    }

    QueuedPacket queued = {};
    memcpy(queued.peerMac, peerMac, sizeof(queued.peerMac));
    queued.packet = packet;
    if (xQueueSend(packetQueue, &queued, 0) != pdTRUE) {
        Serial.println("Trade protocol warning: packet queue is full");
    }
    return true;
}

void processTradeProtocol() {
    if (!protocolReady) {
        return;
    }

    QueuedPacket queuedPacket = {};
    while (xQueueReceive(packetQueue, &queuedPacket, 0) == pdTRUE) {
        handlePacket(queuedPacket);
    }

    QueuedEncounter encounter = {};
    while (xQueueReceive(encounterQueue, &encounter, 0) == pdTRUE) {
        if (!hasPendingTrade) {
            beginTrade(encounter);
        }
    }

    if (hasPendingTrade &&
        (pendingTrade.startedUptime > millis() ||
         millis() - pendingTrade.startedUptime >= PRE_COMMIT_TIMEOUT_MS)) {
        Serial.println("Trade protocol: transaction timed out");
        reportTradeOutcome(pendingTrade.peerMac, pendingTrade.peerDeviceId,
                           pendingTrade.transactionId, false);
        clearPendingTrade();
        return;
    }
    if (hasPendingTrade &&
        static_cast<uint32_t>(millis() - lastSendTime) >= RETRY_INTERVAL_MS) {
        sendPendingMessage();
    }
}
