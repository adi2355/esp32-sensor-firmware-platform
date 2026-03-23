
#include "DetectionDeliveryPump.h"
#include "BLEManager.h"
#include "HistoryService.h"
#include "ProtocolHandler.h"
#include "../Domain/DetectionEvent.h"

static const char* TAG = "HIT_PUMP";


DetectionDeliveryPump::DetectionDeliveryPump
 : _bleManager(nullptr)
 , _historyService(nullptr)
 , _protocolHandler(nullptr)
 , _lastPumpAttemptMs(0)
 , _initialized(false)
 , _pumpSuccessCount(0)
 , _pumpSkipCount(0)
{
}


bool DetectionDeliveryPump::init(BLEManager* bleManager,
 HistoryService* historyService,
 ProtocolHandler* protocolHandler) {
 if (bleManager == nullptr) {
 ESP_LOGE(TAG, "init failed: BLEManager is null");
 return false;
 }
 if (historyService == nullptr) {
 ESP_LOGE(TAG, "init failed: HistoryService is null");
 return false;
 }
 if (protocolHandler == nullptr) {
 ESP_LOGE(TAG, "init failed: ProtocolHandler is null");
 return false;
 }

 _bleManager = bleManager;
 _historyService = historyService;
 _protocolHandler = protocolHandler;
 _lastPumpAttemptMs = millis;
 _initialized = true;

 ESP_LOGI(TAG, "DetectionDeliveryPump initialized (interval=%lums, maxPending=%u)",
 DetectionDeliveryPumpConfig::MIN_PUMP_INTERVAL_MS,
 DetectionDeliveryPumpConfig::MAX_PENDING_BEFORE_BACKOFF);

 return true;
}


bool DetectionDeliveryPump::isTransportReady const {
 if (_bleManager == nullptr) return false;

 return _bleManager->isConnected && _bleManager->isNotifySubscribed;
}

bool DetectionDeliveryPump::isProtocolReady const {
 if (_protocolHandler == nullptr) return false;

 uint8_t pendingCount = _protocolHandler->getPendingCount;
 return pendingCount < DetectionDeliveryPumpConfig::MAX_PENDING_BEFORE_BACKOFF;
}


void DetectionDeliveryPump::update {

 if (!_initialized) {
 return;
 }

 const uint32_t now = millis;
 if (now - _lastPumpAttemptMs < DetectionDeliveryPumpConfig::MIN_PUMP_INTERVAL_MS) {
 return;
 }
 _lastPumpAttemptMs = now;

 if (!isTransportReady) {
 _pumpSkipCount++;
 return;
 }

 if (!isProtocolReady) {
 _pumpSkipCount++;
 ESP_LOGD(TAG, "Pump skip: protocol saturated (pending=%u)",
 _protocolHandler->getPendingCount);
 return;
 }


 DetectionEvent event;
 if (!_historyService->getNextUnsynced(&event)) {
 return;
 }

 bool sent = _protocolHandler->sendDetectionEvent(&event, false);

 if (sent) {
 _pumpSuccessCount++;
 ESP_LOGI(TAG, "[PUMP] Delivered unsynced detection (id=%lu, pumpCount=%lu)",
 event.id, _pumpSuccessCount);
 } else {
 ESP_LOGW(TAG, "[PUMP] Failed to deliver detection (id=%lu)", event.id);
 }
}
