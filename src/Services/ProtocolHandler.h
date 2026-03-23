
#ifndef PROTOCOL_HANDLER_H
#define PROTOCOL_HANDLER_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "../Config.h"
#include "../Domain/ProtocolDefs.h"
#include "../Domain/DetectionEvent.h"

class BLEManager;
class HistoryService;
class OTAManager;

struct PendingMessage {
 bool active;
 uint16_t seqNum;
 uint32_t sentTime;
 uint8_t retryCount;
 MessageType msgType;
 size_t dataLen;
 uint8_t data[Protocol::HEADER_SIZE + 
 Protocol::MAX_PAYLOAD_SIZE + 
 Protocol::CRC_SIZE];
};


class ProtocolHandler {
private:
 BLEManager* _bleManager;
 HistoryService* _historyService;
 OTAManager* _otaManager;
 
 uint16_t _txSeqNum;
 uint16_t _expectedRxSeqNum;
 
 PendingMessage _pendingMessages[Protocol::MAX_PENDING_MESSAGES];
 
 uint32_t _txCount;
 uint32_t _rxCount;
 uint32_t _retryCount;
 uint32_t _errorCount;
 
 uint8_t _invalidAckCount;
 uint32_t _lastSecurityEvent;
 
 uint8_t _detectionEventCount;
 uint32_t _detectionWindowStart;

 uint16_t _lastConfigSeqNum;
 bool _configSeqInitialized;
 
 bool _initialized;
 
 uint8_t _txFrameBuffer[Protocol::HEADER_SIZE + Protocol::MAX_PAYLOAD_SIZE + Protocol::CRC_SIZE];
 
 bool _broken;
 
 SemaphoreHandle_t _mutex;
 
 bool buildFrame(MessageType type, 
 const uint8_t* payload, 
 size_t payloadLen,
 uint16_t seqNum,
 uint8_t* outFrame,
 size_t* outLen);
 
 bool parseFrame(const uint8_t* data,
 size_t length,
 MessageHeader* outHeader,
 const uint8_t** outPayload,
 size_t* outPayloadLen);
 
 bool trackPendingLocked(uint16_t seqNum, 
 MessageType type,
 const uint8_t* data, 
 size_t len);
 
 bool clearPendingLocked(uint16_t seqNum);

 int8_t findFreeSlot;
 
 bool trackPending(uint16_t seqNum, 
 MessageType type,
 const uint8_t* data, 
 size_t len);
 
 bool clearPending(uint16_t seqNum);
 
 bool validateAckSecurity(uint16_t seqNum);
 
 void handleSecurityThresholdExceeded;
 
 void dispatchMessage(MessageType type, 
 uint16_t seqNum,
 const uint8_t* payload, 
 size_t payloadLen);
 
 void handleHello(uint16_t seqNum, const uint8_t* payload, size_t len);
 void handleAck(const uint8_t* payload, size_t len);
 void handleNack(const uint8_t* payload, size_t len);
 void handleHeartbeat;
 void handlePing(uint16_t seqNum);
 void handleSyncRequest(uint16_t seqNum, const uint8_t* payload, size_t len);
 void handleTimeSync(uint16_t seqNum, const uint8_t* payload, size_t len);
 void handleCalibrate(uint16_t seqNum);
 void handleSetConfig(uint16_t seqNum, const uint8_t* payload, size_t len);
 void handleGetConfig(uint16_t seqNum);
 void handleSleepCommand(uint16_t seqNum);
 
 void handleSetWifi(uint16_t seqNum, const uint8_t* payload, size_t len);
 
 void handleOtaInfo(uint16_t seqNum);
 void handleOtaStart(uint16_t seqNum, const uint8_t* payload, size_t len);
 void handleOtaData(uint16_t seqNum, const uint8_t* payload, size_t len);
 void handleOtaCommit(uint16_t seqNum);
 void handleOtaAbort(uint16_t seqNum);
 
 void handleEnterOtaMode(uint16_t seqNum);


 void handlePairingMode(uint16_t seqNum);

 void handleClearBonds(uint16_t seqNum);
 
public:
 ProtocolHandler;
 
 ~ProtocolHandler;
 
 bool init(BLEManager* bleManager, HistoryService* historyService);
 
 bool sendMessage(MessageType type,
 const uint8_t* payload = nullptr,
 size_t payloadLen = 0,
 bool requireAck = true);
 
 void sendAck(uint16_t ackedSeqNum);
 
 void sendNack(uint16_t nackedSeqNum, ErrorCode error);
 
 void processReceived(const uint8_t* data, size_t length);
 
 void processRetries;
 
 bool sendDetectionEvent(const DetectionEvent* event, bool bypassGovernor = false);
 
 bool sendBatteryStatus(uint8_t percent, bool isCharging);
 
 bool sendHelloAck(uint16_t inResponseTo);
 
 bool sendPong(uint16_t inResponseTo);
 
 bool sendConfigData;
 
 bool sendWifiStatus(uint8_t state, uint8_t reason, int8_t rssi, uint32_t ip);
 
 uint8_t getPendingCount const;

 bool hasPendingSpace const;

 bool isAwaitingAck const;
 
 void clearAllPending;
 
 void resetForNewConnection;
 
 void getStats(uint32_t* txCount, 
 uint32_t* rxCount, 
 uint32_t* retryCount, 
 uint32_t* errorCount) const;
 
 void resetStats;
 
 void (*onTimeSyncReceived)(uint32_t epochSeconds) = nullptr;
 
 void (*onSleepCommandReceived) = nullptr;
 
 void (*onCalibrateRequested)(uint16_t* outWake, uint16_t* outHit) = nullptr;
 
 void (*onGetBatteryStatus)(uint8_t& percent, bool& isCharging) = nullptr;
 
 void setOtaManager(OTAManager* otaManager) { _otaManager = otaManager; }
 
 
 bool sendSleepNotification;
 
 void (*onActivityDetected) = nullptr;
};

#endif

