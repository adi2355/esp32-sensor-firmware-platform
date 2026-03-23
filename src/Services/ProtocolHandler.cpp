
#include "ProtocolHandler.h"
#include "BLEManager.h"
#include "HistoryService.h"
#include "OTAManager.h"
#include "ConfigService.h"
#include "WifiService.h"
#include "TimeService.h"
#include "../HAL/Sensor_HAL.h"
#include "../HAL/HAL_Base.h"
#include "esp_mac.h"

static const char* TAG = "PROTO";


ProtocolHandler::ProtocolHandler
 : _bleManager(nullptr)
 , _historyService(nullptr)
 , _otaManager(nullptr)
 , _txSeqNum(0)
 , _expectedRxSeqNum(0)
 , _txCount(0)
 , _rxCount(0)
 , _retryCount(0)
 , _errorCount(0)
 , _invalidAckCount(0)
 , _lastSecurityEvent(0)
 , _detectionEventCount(0)
 , _detectionWindowStart(0)
 , _lastConfigSeqNum(0)
 , _configSeqInitialized(false)
 , _initialized(false)
 , _broken(false)
 , _mutex(nullptr)
{
 _mutex = xSemaphoreCreateMutex;
 if (_mutex == nullptr) {
 ESP_LOGE(TAG, "FATAL: Failed to create protocol mutex!");
 _broken = true;
 }
 
 memset(_pendingMessages, 0, sizeof(_pendingMessages));
}

ProtocolHandler::~ProtocolHandler {
 if (_mutex != nullptr) {
 vSemaphoreDelete(_mutex);
 _mutex = nullptr;
 }
}


bool ProtocolHandler::init(BLEManager* bleManager, HistoryService* historyService) {
 if (_broken) {
 ESP_LOGE(TAG, "FATAL: ProtocolHandler is broken (mutex allocation failed). "
 "Cannot initialize. System should reboot.");
 return false;
 }
 
 _bleManager = bleManager;
 _historyService = historyService;
 _initialized = true;
 
 ESP_LOGI(TAG, "ProtocolHandler initialized");
 return true;
}


bool ProtocolHandler::buildFrame(MessageType type, 
 const uint8_t* payload, 
 size_t payloadLen,
 uint16_t seqNum,
 uint8_t* outFrame,
 size_t* outLen) {
 if (payloadLen > Protocol::MAX_PAYLOAD_SIZE) {
 ESP_LOGE(TAG, "Payload too large: %u > %u", payloadLen, Protocol::MAX_PAYLOAD_SIZE);
 return false;
 }
 
 outFrame[0] = Protocol::SOF;
 outFrame[1] = static_cast<uint8_t>(type);
 outFrame[2] = Protocol::VERSION;
 outFrame[3] = (seqNum >> 8) & 0xFF;
 outFrame[4] = seqNum & 0xFF;
 outFrame[5] = (payloadLen >> 8) & 0xFF;
 outFrame[6] = payloadLen & 0xFF;
 
 if (payloadLen > 0 && payload != nullptr) {
 memcpy(&outFrame[Protocol::HEADER_SIZE], payload, payloadLen);
 }
 
 size_t crcLen = Protocol::HEADER_SIZE + payloadLen;
 uint16_t crc = calculateCRC16(outFrame, crcLen);
 outFrame[crcLen] = (crc >> 8) & 0xFF;
 outFrame[crcLen + 1] = crc & 0xFF;
 
 *outLen = crcLen + Protocol::CRC_SIZE;
 
 return true;
}


bool ProtocolHandler::parseFrame(const uint8_t* data,
 size_t length,
 MessageHeader* outHeader,
 const uint8_t** outPayload,
 size_t* outPayloadLen) {
 if (length < Protocol::MIN_FRAME_SIZE) {
 ESP_LOGW(TAG, "Frame too short: %u < %u", length, Protocol::MIN_FRAME_SIZE);
 return false;
 }
 
 if (data[0] != Protocol::SOF) {
 ESP_LOGW(TAG, "Invalid SOF: 0x%02X", data[0]);
 return false;
 }
 
 outHeader->sof = data[0];
 outHeader->msgType = data[1];
 outHeader->version = data[2];
 outHeader->seqNum = (data[3] << 8) | data[4];
 outHeader->length = (data[5] << 8) | data[6];
 
 if (!outHeader->isValid) {
 ESP_LOGW(TAG, "Invalid header");
 return false;
 }
 
 size_t expectedLen = Protocol::HEADER_SIZE + outHeader->length + Protocol::CRC_SIZE;
 if (length != expectedLen) {
 ESP_LOGW(TAG, "Length mismatch: got %u, expected %u", length, expectedLen);
 return false;
 }
 
 uint16_t receivedCrc = (data[length - 2] << 8) | data[length - 1];
 uint16_t calculatedCrc = calculateCRC16(data, length - Protocol::CRC_SIZE);
 
 if (receivedCrc != calculatedCrc) {
 ESP_LOGW(TAG, "CRC mismatch: got 0x%04X, calc 0x%04X", receivedCrc, calculatedCrc);
 return false;
 }
 
 if (outHeader->length > 0) {
 *outPayload = &data[Protocol::HEADER_SIZE];
 } else {
 *outPayload = nullptr;
 }
 *outPayloadLen = outHeader->length;
 
 return true;
}


bool ProtocolHandler::sendMessage(MessageType type,
 const uint8_t* payload,
 size_t payloadLen,
 bool requireAck) {
 if (!_initialized || _bleManager == nullptr) {
 ESP_LOGE(TAG, "Not initialized");
 return false;
 }
 
 if (!_bleManager->isConnected) {
 ESP_LOGW(TAG, "Not connected, cannot send");
 return false;
 }
 
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
 ESP_LOGE(TAG, "sendMessage: Failed to acquire mutex");
 return false;
 }

 size_t frameLen = 0;
 uint16_t seqNum = _txSeqNum++;
 
 bool buildSuccess = buildFrame(type, payload, payloadLen, seqNum, _txFrameBuffer, &frameLen);
 
 if (!buildSuccess) {
 ESP_LOGE(TAG, "Failed to build frame");
 xSemaphoreGive(_mutex);
 return false;
 }
 
 if (requireAck) {
 if (!trackPendingLocked(seqNum, type, _txFrameBuffer, frameLen)) {
 ESP_LOGW(TAG, "Pending queue full (slot count: %u), message NOT sent", 
 getPendingCount);
 xSemaphoreGive(_mutex);
 return false;
 }
 }
 
 bool sent = _bleManager->sendNotification(_txFrameBuffer, frameLen);
 
 if (sent) {
 _txCount++;
 ESP_LOGD(TAG, "TX: Type=0x%02X Seq=%u Len=%u CRC=OK",
 static_cast<uint8_t>(type), seqNum, payloadLen);
 } else {
 ESP_LOGE(TAG, "Failed to send notification");
 clearPendingLocked(seqNum);
 _errorCount++;
 }
 
 xSemaphoreGive(_mutex);
 return sent;
}


void ProtocolHandler::sendAck(uint16_t ackedSeqNum) {
 uint8_t payload[2] = {
 (uint8_t)(ackedSeqNum >> 8),
 (uint8_t)(ackedSeqNum & 0xFF)
 };
 
 sendMessage(MessageType::MSG_ACK, payload, 2, false);
}

void ProtocolHandler::sendNack(uint16_t nackedSeqNum, ErrorCode error) {
 uint8_t payload[3] = {
 (uint8_t)(nackedSeqNum >> 8),
 (uint8_t)(nackedSeqNum & 0xFF),
 static_cast<uint8_t>(error)
 };
 
 sendMessage(MessageType::MSG_NACK, payload, 3, false);
}


void ProtocolHandler::processReceived(const uint8_t* data, size_t length) {
 if (!_initialized) return;
 
 MessageHeader header;
 const uint8_t* payload = nullptr;
 size_t payloadLen = 0;
 
 if (!parseFrame(data, length, &header, &payload, &payloadLen)) {
 _errorCount++;
 
 if (length >= 5) {
 uint16_t seqNum = (data[3] << 8) | data[4];
 sendNack(seqNum, ErrorCode::ERR_CRC_MISMATCH);
 }
 return;
 }
 
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
 _rxCount++;
 xSemaphoreGive(_mutex);
 }
 
 ESP_LOGD(TAG, "RX: Type=0x%02X Seq=%u Len=%u",
 header.msgType, header.seqNum, payloadLen);
 
 dispatchMessage(static_cast<MessageType>(header.msgType), 
 header.seqNum, 
 payload, 
 payloadLen);
}


void ProtocolHandler::dispatchMessage(MessageType type, 
 uint16_t seqNum,
 const uint8_t* payload, 
 size_t payloadLen) {
 bool isActivityMessage = (type != MessageType::MSG_HEARTBEAT &&
 type != MessageType::MSG_ACK &&
 type != MessageType::MSG_NACK);
 
 if (isActivityMessage && onActivityDetected) {
 ESP_LOGD(TAG, "Activity detected (msg=0x%02X)", 
 static_cast<uint8_t>(type));
 onActivityDetected;
 }
 
 switch (type) {
 case MessageType::MSG_HELLO:
 handleHello(seqNum, payload, payloadLen);
 break;
 
 case MessageType::MSG_ACK:
 handleAck(payload, payloadLen);
 break;
 
 case MessageType::MSG_NACK:
 handleNack(payload, payloadLen);
 break;
 
 case MessageType::MSG_HEARTBEAT:
 handleHeartbeat;
 break;
 
 case MessageType::MSG_PING:
 handlePing(seqNum);
 break;
 
 case MessageType::MSG_SYNC_REQUEST:
 handleSyncRequest(seqNum, payload, payloadLen);
 break;
 
 case MessageType::MSG_TIME_SYNC:
 handleTimeSync(seqNum, payload, payloadLen);
 break;
 
 case MessageType::MSG_CALIBRATE:
 handleCalibrate(seqNum);
 break;
 
 case MessageType::MSG_SET_CONFIG:
 handleSetConfig(seqNum, payload, payloadLen);
 break;
 
 case MessageType::MSG_GET_CONFIG:
 handleGetConfig(seqNum);
 break;
 
 case MessageType::MSG_SLEEP:
 handleSleepCommand(seqNum);
 break;
 
 case MessageType::MSG_OTA_INFO:
 handleOtaInfo(seqNum);
 break;
 
 case MessageType::MSG_OTA_START:
 handleOtaStart(seqNum, payload, payloadLen);
 break;
 
 case MessageType::MSG_OTA_DATA:
 handleOtaData(seqNum, payload, payloadLen);
 break;
 
 case MessageType::MSG_OTA_COMMIT:
 handleOtaCommit(seqNum);
 break;
 
 case MessageType::MSG_OTA_ABORT:
 handleOtaAbort(seqNum);
 break;
 
 case MessageType::MSG_ENTER_OTA_MODE:
 handleEnterOtaMode(seqNum);
 break;

 case MessageType::MSG_PAIRING_MODE:
 handlePairingMode(seqNum);
 break;

 case MessageType::MSG_CLEAR_BONDS:
 handleClearBonds(seqNum);
 break;

 case MessageType::MSG_SET_WIFI:
 handleSetWifi(seqNum, payload, payloadLen);
 break;

 default:
 ESP_LOGW(TAG, "Unknown message type: 0x%02X", static_cast<uint8_t>(type));
 sendNack(seqNum, ErrorCode::ERR_UNKNOWN_MSG);
 break;
 }
}


void ProtocolHandler::handleHello(uint16_t seqNum, const uint8_t* payload, size_t len) {
 ESP_LOGI(TAG, "HELLO received from app (payload len=%u)", len);
 
 
 if (len >= 5 && payload != nullptr) {
 uint8_t appProtocolVersion = payload[0];
 
 uint32_t appLastEventId = 
 ((uint32_t)payload[1] << 24) |
 ((uint32_t)payload[2] << 16) |
 ((uint32_t)payload[3] << 8) |
 (uint32_t)payload[4];
 
 ESP_LOGI(TAG, "HELLO parsed: protocolVersion=%u, appLastEventId=%lu", 
 appProtocolVersion, appLastEventId);
 
 if (_historyService != nullptr) {
 uint32_t deviceCurrentId = _historyService->getLatestEventId;
 
 ESP_LOGI(TAG, "Event ID state: Device=%lu, App=%lu", 
 deviceCurrentId, appLastEventId);
 
 if (appLastEventId > deviceCurrentId) {
 ESP_LOGW(TAG, "EVENT ID SYNC: Fast-forwarding from %lu to %lu", 
 deviceCurrentId, appLastEventId);
 
 _historyService->fastForwardEventId(appLastEventId);
 
 ESP_LOGI(TAG, "Event ID sync complete. Next event will be ID %lu", 
 appLastEventId + 1);
 } else if (deviceCurrentId > appLastEventId) {
 ESP_LOGI(TAG, "Device ahead by %lu events. App may request sync.", 
 deviceCurrentId - appLastEventId);
 } else {
 ESP_LOGI(TAG, "Event IDs already synchronized.");
 }
 } else {
 ESP_LOGW(TAG, "HistoryService not available - cannot sync Event ID");
 }
 } else {
 ESP_LOGW(TAG, "HELLO without Event ID payload (legacy app or empty payload)");
 }
 
 sendHelloAck(seqNum);
 sendAck(seqNum);
}

void ProtocolHandler::handleAck(const uint8_t* payload, size_t len) {
 if (len < 2) return;
 
 uint16_t ackedSeq = (payload[0] << 8) | payload[1];
 
 if (!validateAckSecurity(ackedSeq)) {
 ESP_LOGW(TAG, "SECURITY: Invalid ACK for Seq=%u (invalid ACK count: %u)",
 ackedSeq, _invalidAckCount);
 return;
 }
 
 bool found = false;
 
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
 ESP_LOGE(TAG, "handleAck: Failed to acquire mutex");
 return;
 }
 
 for (uint8_t i = 0; i < Protocol::MAX_PENDING_MESSAGES; i++) {
 if (_pendingMessages[i].active && _pendingMessages[i].seqNum == ackedSeq) {
 
 if (_pendingMessages[i].msgType == MessageType::MSG_DETECTION_EVENT) {
 const uint8_t* pData = &_pendingMessages[i].data[Protocol::HEADER_SIZE];
 uint32_t eventId;
 memcpy(&eventId, pData, sizeof(uint32_t));
 
 xSemaphoreGive(_mutex);
 
 if (_historyService) {
 _historyService->markAsSynced(eventId);
 }
 
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
 ESP_LOGE(TAG, "handleAck: Failed to re-acquire mutex");
 return;
 }
 
 if (_pendingMessages[i].active && _pendingMessages[i].seqNum == ackedSeq) {
 _pendingMessages[i].active = false;
 found = true;
 }
 } else {
 _pendingMessages[i].active = false;
 found = true;
 }
 
 break;
 }
 }
 xSemaphoreGive(_mutex);
 
 if (found) {
 ESP_LOGD(TAG, "ACK received for Seq=%u", ackedSeq);
 
 _invalidAckCount = 0;
 }
}

void ProtocolHandler::handleNack(const uint8_t* payload, size_t len) {
 if (len < 3) return;
 
 uint16_t nackedSeq = (payload[0] << 8) | payload[1];
 ErrorCode error = static_cast<ErrorCode>(payload[2]);
 
 ESP_LOGW(TAG, "NACK received for Seq=%u, Error=0x%02X", 
 nackedSeq, static_cast<uint8_t>(error));
 
 clearPending(nackedSeq);
 _errorCount++;
}

void ProtocolHandler::handleHeartbeat {
 ESP_LOGD(TAG, "Heartbeat received");
}

void ProtocolHandler::handlePing(uint16_t seqNum) {
 ESP_LOGD(TAG, "PING received, sending PONG");
 sendPong(seqNum);
 sendAck(seqNum);
}

void ProtocolHandler::handleSyncRequest(uint16_t seqNum, const uint8_t* payload, size_t len) {
 if (len < 4 || _historyService == nullptr) {
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 uint32_t sinceId = ((uint32_t)payload[0] << 24) |
 ((uint32_t)payload[1] << 16) |
 ((uint32_t)payload[2] << 8) |
 payload[3];
 
 ESP_LOGI(TAG, "Sync request: events since ID=%lu", sinceId);
 
 
 uint8_t syncedCount = 0;
 DetectionEvent event;
 bool hasMore = true;
 uint32_t lastSyncedId = sinceId;

 while (syncedCount < 10 && hasMore) {
 uint8_t batchCount = 0;
 if (_historyService->getEventsSince(lastSyncedId, &event, 1, &batchCount) && batchCount > 0) {
 if (!sendDetectionEvent(&event, true)) {
 ESP_LOGW(TAG, "Sync stopped at event %u (BLE queue full)", syncedCount);
 hasMore = false;
 break;
 }
 
 lastSyncedId = event.id;
 syncedCount++;
 
 vTaskDelay(pdMS_TO_TICKS(20));
 } else {
 hasMore = false;
 }
 }
 
 ESP_LOGI(TAG, "Sync batch complete: sent %u events", syncedCount);
 sendAck(seqNum);
}

void ProtocolHandler::handleTimeSync(uint16_t seqNum, const uint8_t* payload, size_t len) {
 if (len < sizeof(TimeSyncPayload)) {
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 const TimeSyncPayload* syncData = reinterpret_cast<const TimeSyncPayload*>(payload);
 
 ESP_LOGI(TAG, "Time sync received: epoch=%lu, tzOffset=%d min", 
 syncData->epochSeconds, syncData->tzOffsetMins);
 
 if (_historyService) {
 _historyService->applyTimeSync(syncData->epochSeconds);
 }
 
 ConfigService* configService = ConfigService::getInstance;
 if (configService) {
 configService->updateLastKnownEpoch(syncData->epochSeconds);
 }
 
 if (onTimeSyncReceived) {
 onTimeSyncReceived(syncData->epochSeconds);
 }
 
 sendAck(seqNum);
}

void ProtocolHandler::handleCalibrate(uint16_t seqNum) {
 ESP_LOGI(TAG, "Calibration requested");
 
 uint16_t wakeThresh = 0;
 uint16_t hitThresh = 0;
 
 if (onCalibrateRequested) {
 onCalibrateRequested(&wakeThresh, &hitThresh);
 
 uint8_t response[4] = {
 (uint8_t)(wakeThresh >> 8),
 (uint8_t)(wakeThresh & 0xFF),
 (uint8_t)(detectionThresh >> 8),
 (uint8_t)(detectionThresh & 0xFF)
 };
 
 sendMessage(MessageType::MSG_CONFIG_DATA, response, 4, false);
 }
 
 sendAck(seqNum);
}

void ProtocolHandler::handleSetConfig(uint16_t seqNum, const uint8_t* payload, size_t len) {
 ESP_LOGI(TAG, "SET_CONFIG received (%u bytes), seqNum=%u", len, seqNum);

 if (_configSeqInitialized) {
 int16_t seqDiff = (int16_t)(seqNum - _lastConfigSeqNum);

 if (seqDiff <= 0) {
 ESP_LOGW(TAG, "Dropping stale config packet (seqNum=%u <= last=%u)",
 seqNum, _lastConfigSeqNum);

 sendAck(seqNum);

 return;
 }
 }

 if (len < sizeof(SetConfigPayload)) {
 ESP_LOGW(TAG, "SET_CONFIG payload too short: %u < %u", len, sizeof(SetConfigPayload));
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }

 const SetConfigPayload* configPayload = reinterpret_cast<const SetConfigPayload*>(payload);

 ESP_LOGI(TAG, "SET_CONFIG: configId=0x%02X, value=%u",
 configPayload->configId, configPayload->value);

 ConfigService* config = ConfigService::getInstance;
 if (config == nullptr) {
 ESP_LOGE(TAG, "ConfigService not available!");
 sendNack(seqNum, ErrorCode::ERR_INTERNAL);
 return;
 }

 Sensor_HAL* sensorHal = Sensor_HAL::getInstance;
 if (sensorHal != nullptr && configPayload->configId == ConfigId::SENSITIVITY) {
 sensorHal->suppressDetections(500);
 }

 switch (configPayload->configId) {
 case ConfigId::SENSITIVITY:
 if (configPayload->value > 100) {
 ESP_LOGW(TAG, "Invalid sensitivity value: %u (max 100)", configPayload->value);
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }

 ESP_LOGI(TAG, "Setting sensitivity to %u", configPayload->value);
 config->setSensitivity(configPayload->value);
 break;

 case ConfigId::LED_BRIGHTNESS:
 ESP_LOGI(TAG, "Setting LED brightness to %u", configPayload->value);
 config->setLedBrightness(configPayload->value);
 break;

 default:
 ESP_LOGW(TAG, "Unknown config ID: 0x%02X", configPayload->configId);
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }

 _lastConfigSeqNum = seqNum;
 _configSeqInitialized = true;

 sendAck(seqNum);

 sendConfigData;
}

void ProtocolHandler::handleGetConfig(uint16_t seqNum) {
 ESP_LOGI(TAG, "GET_CONFIG received");
 
 sendAck(seqNum);
 
 sendConfigData;
}

void ProtocolHandler::handleSleepCommand(uint16_t seqNum) {
 ESP_LOGI(TAG, "Sleep command received");

 sendAck(seqNum);

 if (onSleepCommandReceived) {
 onSleepCommandReceived;
 }
}


void ProtocolHandler::handlePairingMode(uint16_t seqNum) {
 ESP_LOGI(TAG, "MSG_PAIRING_MODE (0x43) received");
 ESP_LOGI(TAG, " Action: Enabling pairing mode");

 if (_bleManager == nullptr) {
 ESP_LOGE(TAG, "BLEManager not available - cannot enable pairing mode");
 sendNack(seqNum, ErrorCode::ERR_INTERNAL);
 return;
 }

 _bleManager->enablePairingMode;

 ESP_LOGI(TAG, "Pairing mode enabled - device accepting new connections");

 sendAck(seqNum);
}

void ProtocolHandler::handleClearBonds(uint16_t seqNum) {
 ESP_LOGI(TAG, "MSG_CLEAR_BONDS (0x44) received");
 ESP_LOGI(TAG, " Protocol: Bilateral Bond Clearing");
 ESP_LOGI(TAG, " Reason: App requested bond storage clear");

 if (_bleManager == nullptr) {
 ESP_LOGE(TAG, "FATAL: BLEManager not available - cannot clear bonds");
 sendNack(seqNum, ErrorCode::ERR_INTERNAL);
 return;
 }

 int bondCountBefore = _bleManager->getBondCount;
 ESP_LOGI(TAG, " Current bonds before clear: %d", bondCountBefore);

 ESP_LOGI(TAG, "Clearing all NimBLE bond storage...");
 _bleManager->deleteAllBonds;

 int bondCountAfter = _bleManager->getBondCount;
 ESP_LOGI(TAG, " Bonds after clear: %d", bondCountAfter);

 if (bondCountAfter == 0) {
 ESP_LOGI(TAG, "SUCCESS: All bonds cleared successfully");
 } else {
 ESP_LOGW(TAG, "WARNING: Some bonds may remain (%d)", bondCountAfter);
 }

 ESP_LOGI(TAG, "Enabling pairing mode for fresh pairing...");
 _bleManager->enablePairingMode;

 ESP_LOGI(TAG, "BILATERAL BOND CLEAR COMPLETE");
 ESP_LOGI(TAG, " Bonds cleared: %d -> %d", bondCountBefore, bondCountAfter);
 ESP_LOGI(TAG, " Pairing mode: ENABLED");
 ESP_LOGI(TAG, " Next step: Device ready for fresh pairing");
 ESP_LOGI(TAG, " REMINDER: User should also 'Forget Device' in iOS Settings");

 sendAck(seqNum);
}


void ProtocolHandler::handleSetWifi(uint16_t seqNum, const uint8_t* payload, size_t len) {
 ESP_LOGI(TAG, "MSG_SET_WIFI (0x26) received");
 ESP_LOGI(TAG, " Protocol: Wi-Fi/NTP Time Sync");
 ESP_LOGI(TAG, " Payload length: %u bytes", len);
 
 ESP_LOGW(TAG, " [DISABLED] Wi-Fi provisioning is currently disabled");
 ESP_LOGI(TAG, " -> Command acknowledged but credentials NOT stored");
 ESP_LOGI(TAG, " -> Device will use BLE time sync (Tier 2) exclusively");
 sendAck(seqNum);
 return;
 
 if (len < 3) {
 ESP_LOGE(TAG, "Payload too short: %u < 3 bytes", len);
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 uint8_t ssidLen = payload[0];
 
 if (ssidLen == 0) {
 ESP_LOGE(TAG, "SSID length is 0 (empty SSID not allowed)");
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 if (ssidLen > WifiPayload::SSID_MAX_LENGTH) {
 ESP_LOGE(TAG, "SSID too long: %u > %u", ssidLen, WifiPayload::SSID_MAX_LENGTH);
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 if (len < 1 + ssidLen + 1) {
 ESP_LOGE(TAG, "Payload truncated after SSID: expected %u, got %u", 
 1 + ssidLen + 1, len);
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 String ssid = "";
 for (uint8_t i = 0; i < ssidLen; i++) {
 ssid += (char)payload[1 + i];
 }
 
 uint8_t passLen = payload[1 + ssidLen];
 
 if (passLen > WifiPayload::PASS_MAX_LENGTH) {
 ESP_LOGE(TAG, "Password too long: %u > %u", passLen, WifiPayload::PASS_MAX_LENGTH);
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 if (len < 1 + ssidLen + 1 + passLen) {
 ESP_LOGE(TAG, "Payload truncated at password: expected %u, got %u", 
 1 + ssidLen + 1 + passLen, len);
 sendNack(seqNum, ErrorCode::ERR_INVALID_PAYLOAD);
 return;
 }
 
 String password = "";
 for (uint8_t i = 0; i < passLen; i++) {
 password += (char)payload[1 + ssidLen + 1 + i];
 }
 
 ESP_LOGI(TAG, "Parsed credentials: SSID='%s' (len=%u), Password=[%u chars]",
 ssid.c_str, ssidLen, passLen);
 
 TimeService* timeSvc = TimeService::getInstance;
 if (timeSvc != nullptr) {
 timeSvc->startProvisioningMode;
 } else {
 ESP_LOGW(TAG, "TimeService not available - provisioning timeout protection disabled");
 }
 
 WifiService* wifi = WifiService::getInstance;
 
 if (wifi == nullptr) {
 ESP_LOGE(TAG, "WifiService not available");
 sendNack(seqNum, ErrorCode::ERR_INTERNAL);
 return;
 }
 
 bool success = wifi->setCredentials(ssid, password);
 
 if (success) {
 ESP_LOGI(TAG, "WI-FI CREDENTIALS SAVED SUCCESSFULLY");
 ESP_LOGI(TAG, " SSID: %s", ssid.c_str);
 ESP_LOGI(TAG, " Status: Connection will be attempted");
 ESP_LOGI(TAG, " NTP: Will sync once connected");
 sendAck(seqNum);
 } else {
 ESP_LOGE(TAG, "Failed to save Wi-Fi credentials");
 sendNack(seqNum, ErrorCode::ERR_FLASH_WRITE);
 }
}


bool ProtocolHandler::sendDetectionEvent(const DetectionEvent* event, bool bypassGovernor) {
 if (event == nullptr) return false;
 
 
 if (!bypassGovernor) {
 uint32_t now = millis;
 
 if (now - _detectionWindowStart > Protocol::DETECTION_RATE_WINDOW_MS) {
 _detectionWindowStart = now;
 _detectionEventCount = 0;
 }
 
 if (_detectionEventCount >= Protocol::MAX_DETECTIONS_PER_WINDOW) {
 ESP_LOGW(TAG, "PANIC GOVERNOR: Hit event dropped (rate limit %u/%ums exceeded)",
 Protocol::MAX_DETECTIONS_PER_WINDOW, Protocol::DETECTION_RATE_WINDOW_MS);
 
 ESP_LOGD(TAG, " Dropped detection ID=%lu (stored locally, not sent)", event->id);
 
 Sensor_HAL* sensorHal = Sensor_HAL::getInstance;
 if (sensorHal != nullptr) {
 sensorHal->suppressDetections(1000);
 ESP_LOGI(TAG, "PANIC GOVERNOR: Sensor detection suppression enabled (1000ms)");
 }
 
 return false;
 }
 
 _detectionEventCount++;
 } else {
 ESP_LOGD(TAG, "sendDetectionEvent: Governor bypassed for sync (ID=%lu)", event->id);
 }
 
 return sendMessage(MessageType::MSG_DETECTION_EVENT, 
 reinterpret_cast<const uint8_t*>(event), 
 sizeof(DetectionEvent),
 true);
}

bool ProtocolHandler::sendBatteryStatus(uint8_t percent, bool isCharging) {
 BatteryStatusPayload payload;
 payload.percent = percent;
 payload.isCharging = isCharging ? 1 : 0;
 payload.voltageRaw = 0;
 
 return sendMessage(MessageType::MSG_BATTERY_STATUS,
 reinterpret_cast<uint8_t*>(&payload),
 sizeof(BatteryStatusPayload),
 false);
}


bool ProtocolHandler::sendWifiStatus(uint8_t state, uint8_t reason, int8_t rssi, uint32_t ip) {
 if (!_initialized || _bleManager == nullptr || !_bleManager->isConnected) {
 ESP_LOGD(TAG, "sendWifiStatus: BLE not connected, skipping");
 return false;
 }
 
 uint8_t payload[7];
 payload[0] = state;
 payload[1] = reason;
 payload[2] = (uint8_t)rssi;
 
 payload[3] = ip & 0xFF;
 payload[4] = (ip >> 8) & 0xFF;
 payload[5] = (ip >> 16) & 0xFF;
 payload[6] = (ip >> 24) & 0xFF;
 
 ESP_LOGI(TAG, "Sending Wi-Fi status: state=%u, reason=%u, rssi=%d, ip=%lu", 
 state, reason, rssi, ip);
 
 return sendMessage(MessageType::MSG_DEVICE_STATUS,
 payload,
 sizeof(payload),
 false);
}

bool ProtocolHandler::sendHelloAck(uint16_t inResponseTo) {
 HelloAckPayload payload;
 
 payload.fwMajor = FIRMWARE_VERSION_MAJOR;
 payload.fwMinor = FIRMWARE_VERSION_MINOR;
 payload.fwPatch = FIRMWARE_VERSION_PATCH;
 payload.buildType = FIRMWARE_BUILD_TYPE;
 payload.buildNumber = 0;
 
 if (onGetBatteryStatus) {
 bool charging = false;
 onGetBatteryStatus(payload.batteryPercent, charging);
 payload.isCharging = charging ? 1 : 0;
 } else {
 payload.batteryPercent = 0;
 payload.isCharging = 0;
 }
 
 payload.lastEventId = _historyService ? _historyService->getLatestEventId : 0;
 
 payload.currentMillis = HAL::getSystemUptime;
 
 payload.bondedDevices = _bleManager ? _bleManager->getBondCount : 0;
 
 ConfigService* config = ConfigService::getInstance;
 if (config != nullptr) {
 payload.sensitivity = config->getSensitivity;
 payload.configCrc32 = config->getConfigCrc;
 payload.configSignature = config->getConfigSignature;
 } else {
 payload.sensitivity = 50;
 payload.configCrc32 = 0;
 payload.configSignature = 0;
 }
 
 esp_read_mac(payload.hardwareId, ESP_MAC_BT);
 payload.reserved2[0] = 0;
 payload.reserved2[1] = 0;
 
 ESP_LOGI(TAG, "HELLO_ACK: Hardware ID (MAC): %02X:%02X:%02X:%02X:%02X:%02X",
 payload.hardwareId[0], payload.hardwareId[1], payload.hardwareId[2],
 payload.hardwareId[3], payload.hardwareId[4], payload.hardwareId[5]);
 
 return sendMessage(MessageType::MSG_HELLO_ACK,
 reinterpret_cast<uint8_t*>(&payload),
 sizeof(HelloAckPayload),
 false);
}

bool ProtocolHandler::sendPong(uint16_t inResponseTo) {
 return sendMessage(MessageType::MSG_PONG, nullptr, 0, false);
}


bool ProtocolHandler::sendSleepNotification {
 ESP_LOGI(TAG, "Sending Sleep Notification to App");
 ESP_LOGI(TAG, " Message: MSG_SLEEP (0x07)");
 ESP_LOGI(TAG, " Reason: Connected idle timeout");
 ESP_LOGI(TAG, " Action: Device will enter deep sleep after disconnect");
 
 return sendMessage(MessageType::MSG_SLEEP, nullptr, 0, false);
}

bool ProtocolHandler::sendConfigData {
 ConfigService* config = ConfigService::getInstance;
 if (config == nullptr) {
 ESP_LOGE(TAG, "sendConfigData: ConfigService not available");
 return false;
 }
 
 ConfigDataPayload payload;
 payload.sensitivity = config->getSensitivity;
 payload.ledBrightness = config->getLedBrightness;
 payload.reserved[0] = 0;
 payload.reserved[1] = 0;
 
 ESP_LOGI(TAG, "Sending CONFIG_DATA: sensitivity=%u, ledBrightness=%u",
 payload.sensitivity, payload.ledBrightness);
 
 return sendMessage(MessageType::MSG_CONFIG_DATA,
 reinterpret_cast<uint8_t*>(&payload),
 sizeof(ConfigDataPayload),
 false);
}


int8_t ProtocolHandler::findFreeSlot {
 for (uint8_t i = 0; i < Protocol::MAX_PENDING_MESSAGES; i++) {
 if (!_pendingMessages[i].active) {
 return i;
 }
 }
 return -1;
}

bool ProtocolHandler::trackPendingLocked(uint16_t seqNum, 
 MessageType type,
 const uint8_t* data, 
 size_t len) {
 int8_t slot = findFreeSlot;
 if (slot < 0) {
 ESP_LOGW(TAG, "Pending queue full!");
 return false;
 }
 
 _pendingMessages[slot].active = true;
 _pendingMessages[slot].seqNum = seqNum;
 _pendingMessages[slot].msgType = type;
 _pendingMessages[slot].sentTime = millis;
 _pendingMessages[slot].retryCount = 0;
 _pendingMessages[slot].dataLen = len;
 memcpy(_pendingMessages[slot].data, data, len);
 
 return true;
}

bool ProtocolHandler::clearPendingLocked(uint16_t seqNum) {
 for (uint8_t i = 0; i < Protocol::MAX_PENDING_MESSAGES; i++) {
 if (_pendingMessages[i].active && _pendingMessages[i].seqNum == seqNum) {
 _pendingMessages[i].active = false;
 return true;
 }
 }
 return false;
}

bool ProtocolHandler::trackPending(uint16_t seqNum, 
 MessageType type,
 const uint8_t* data, 
 size_t len) {
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
 ESP_LOGE(TAG, "trackPending: Failed to acquire mutex");
 return false;
 }
 
 bool result = trackPendingLocked(seqNum, type, data, len);
 
 xSemaphoreGive(_mutex);
 
 return result;
}

bool ProtocolHandler::clearPending(uint16_t seqNum) {
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
 ESP_LOGE(TAG, "clearPending: Failed to acquire mutex");
 return false;
 }
 
 bool result = clearPendingLocked(seqNum);
 
 xSemaphoreGive(_mutex);
 return result;
}

void ProtocolHandler::clearAllPending {
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 for (uint8_t i = 0; i < Protocol::MAX_PENDING_MESSAGES; i++) {
 _pendingMessages[i].active = false;
 }
 xSemaphoreGive(_mutex);
 }
 
 ESP_LOGI(TAG, "Cleared all pending messages");
}

void ProtocolHandler::resetForNewConnection {
 ESP_LOGI(TAG, "Session Reset");
 ESP_LOGI(TAG, " Clearing pending messages from previous session");
 ESP_LOGI(TAG, " Resetting security counters for fresh start");
 
 clearAllPending;
 
 _invalidAckCount = 0;
 _lastSecurityEvent = 0;
 
 _txSeqNum = 0;
 _expectedRxSeqNum = 0;
 
 ESP_LOGI(TAG, "Session reset complete - ready for new connection");
}


bool ProtocolHandler::validateAckSecurity(uint16_t seqNum) {
 unsigned long now = millis;
 
 bool found = false;
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
 for (uint8_t i = 0; i < Protocol::MAX_PENDING_MESSAGES; i++) {
 if (_pendingMessages[i].active && _pendingMessages[i].seqNum == seqNum) {
 found = true;
 break;
 }
 }
 xSemaphoreGive(_mutex);
 }
 
 if (found) {
 if (_invalidAckCount > 0) {
 ESP_LOGD(TAG, "Valid ACK received, resetting security counter");
 }
 return true;
 }
 
 
 if (now - _lastSecurityEvent > Protocol::SECURITY_RESET_WINDOW_MS) {
 _invalidAckCount = 0;
 }
 
 _invalidAckCount++;
 _lastSecurityEvent = now;
 _errorCount++;
 
 ESP_LOGW(TAG, "SECURITY: Phantom ACK detected for Seq=%u (count: %u/%u)",
 seqNum, _invalidAckCount, Protocol::INVALID_ACK_THRESHOLD);
 
 if (_invalidAckCount >= Protocol::INVALID_ACK_THRESHOLD) {
 handleSecurityThresholdExceeded;
 }
 
 return false;
}

void ProtocolHandler::handleSecurityThresholdExceeded {
 ESP_LOGW(TAG, "SECURITY: Phantom ACK threshold exceeded!");
 ESP_LOGW(TAG, " Invalid ACK count: %u", _invalidAckCount);
 ESP_LOGW(TAG, " Note: Likely stale ACKs from previous session (benign)");
 ESP_LOGW(TAG, " Action: Counter reset only - Queue PRESERVED");
 
 _invalidAckCount = 0;
 
 
}


void ProtocolHandler::processRetries {
 if (!_initialized || !_bleManager || !_bleManager->isConnected) {
 return;
 }
 
 uint32_t now = millis;
 
 for (uint8_t i = 0; i < Protocol::MAX_PENDING_MESSAGES; i++) {
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
 continue;
 }
 
 if (!_pendingMessages[i].active) {
 xSemaphoreGive(_mutex);
 continue;
 }
 
 bool shouldRetry = false;
 bool shouldGiveUp = false;
 
 if (now - _pendingMessages[i].sentTime > Timing::ACK_TIMEOUT_MS) {
 if (_pendingMessages[i].retryCount < Timing::MAX_RETRY_COUNT) {
 _pendingMessages[i].retryCount++;
 _pendingMessages[i].sentTime = now;
 _retryCount++;
 shouldRetry = true;
 } else {
 _pendingMessages[i].active = false;
 _errorCount++;
 shouldGiveUp = true;
 }
 }
 
 uint8_t localRetryCount = _pendingMessages[i].retryCount;
 uint16_t localSeqNum = _pendingMessages[i].seqNum;
 MessageType localMsgType = _pendingMessages[i].msgType;
 size_t localDataLen = _pendingMessages[i].dataLen;
 
 uint8_t localData[Protocol::HEADER_SIZE + Protocol::MAX_PAYLOAD_SIZE + Protocol::CRC_SIZE];
 if (shouldRetry) {
 memcpy(localData, _pendingMessages[i].data, localDataLen);
 }
 
 xSemaphoreGive(_mutex);
 
 
 if (shouldRetry) {
 ESP_LOGW(TAG, "Retry #%u for Seq=%u (Type=0x%02X)",
 localRetryCount, localSeqNum, 
 static_cast<uint8_t>(localMsgType));
 
 _bleManager->sendNotification(localData, localDataLen);
 } else if (shouldGiveUp) {
 ESP_LOGE(TAG, "Gave up on Seq=%u after %u retries",
 localSeqNum, Timing::MAX_RETRY_COUNT);
 }
 }
}


uint8_t ProtocolHandler::getPendingCount const {
 uint8_t count = 0;
 for (uint8_t i = 0; i < Protocol::MAX_PENDING_MESSAGES; i++) {
 if (_pendingMessages[i].active) count++;
 }
 return count;
}

bool ProtocolHandler::hasPendingSpace const {
 return getPendingCount < Protocol::MAX_PENDING_MESSAGES;
}

bool ProtocolHandler::isAwaitingAck const {
 return getPendingCount > 0;
}

void ProtocolHandler::getStats(uint32_t* txCount, 
 uint32_t* rxCount, 
 uint32_t* retryCount, 
 uint32_t* errorCount) const {
 if (txCount) *txCount = _txCount;
 if (rxCount) *rxCount = _rxCount;
 if (retryCount) *retryCount = _retryCount;
 if (errorCount) *errorCount = _errorCount;
}

void ProtocolHandler::resetStats {
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 _txCount = 0;
 _rxCount = 0;
 _retryCount = 0;
 _errorCount = 0;
 xSemaphoreGive(_mutex);
 }
}


void ProtocolHandler::handleOtaInfo(uint16_t seqNum) {
 if (_otaManager == nullptr) {
 ESP_LOGE(TAG, "OTA Manager not set");
 sendNack(seqNum, ErrorCode::ERR_INVALID_STATE);
 return;
 }
 
 _otaManager->handleInfoRequest(seqNum);
 sendAck(seqNum);
}

void ProtocolHandler::handleOtaStart(uint16_t seqNum, const uint8_t* payload, size_t len) {
 if (_otaManager == nullptr) {
 ESP_LOGE(TAG, "OTA Manager not set");
 sendNack(seqNum, ErrorCode::ERR_INVALID_STATE);
 return;
 }
 
 if (_otaManager->handleStart(payload, len)) {
 sendAck(seqNum);
 } else {
 sendNack(seqNum, ErrorCode::ERR_OTA_FAILED);
 }
}

void ProtocolHandler::handleOtaData(uint16_t seqNum, const uint8_t* payload, size_t len) {
 if (_otaManager == nullptr || len < 2) {
 sendNack(seqNum, ErrorCode::ERR_INVALID_STATE);
 return;
 }
 
 uint16_t chunkIndex = (payload[0] << 8) | payload[1];
 
 const uint8_t* firmwareData = payload + 2;
 size_t firmwareLen = len - 2;
 
 _otaManager->handleData(chunkIndex, firmwareData, firmwareLen);
}

void ProtocolHandler::handleOtaCommit(uint16_t seqNum) {
 if (_otaManager == nullptr) {
 sendNack(seqNum, ErrorCode::ERR_INVALID_STATE);
 return;
 }
 
 if (_otaManager->handleCommit) {
 sendAck(seqNum);
 } else {
 sendNack(seqNum, ErrorCode::ERR_OTA_FAILED);
 }
}

void ProtocolHandler::handleOtaAbort(uint16_t seqNum) {
 if (_otaManager != nullptr) {
 _otaManager->handleAbort;
 }
 sendAck(seqNum);
}


extern RTC_NOINIT_ATTR uint32_t g_rtc_magic;
extern RTC_NOINIT_ATTR uint32_t g_boot_mode;

void ProtocolHandler::handleEnterOtaMode(uint16_t seqNum) {
 ESP_LOGW(TAG, "MSG_ENTER_OTA_MODE received");
 ESP_LOGW(TAG, "Preparing to reboot into Flasher Mode...");
 
 sendAck(seqNum);
 
 g_rtc_magic = BootConfig::RTC_MAGIC_KEY;
 g_boot_mode = BootConfig::MODE_FLASHER;
 
 vTaskDelay(pdMS_TO_TICKS(200));
 
 ESP_LOGW(TAG, "Rebooting into Flasher Mode NOW...");
 Serial.flush;
 
 esp_restart;
 
}
