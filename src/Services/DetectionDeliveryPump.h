
#ifndef DETECTION_DELIVERY_PUMP_H
#define DETECTION_DELIVERY_PUMP_H

#include <Arduino.h>

class BLEManager;
class HistoryService;
class ProtocolHandler;

namespace DetectionDeliveryPumpConfig {
 constexpr uint32_t MIN_PUMP_INTERVAL_MS = 150;

 constexpr uint8_t MAX_PENDING_BEFORE_BACKOFF = 8;
}


class DetectionDeliveryPump {
private:
 BLEManager* _bleManager;
 HistoryService* _historyService;
 ProtocolHandler* _protocolHandler;

 uint32_t _lastPumpAttemptMs;

 bool _initialized;

 uint32_t _pumpSuccessCount;
 uint32_t _pumpSkipCount;

 bool isTransportReady const;

 bool isProtocolReady const;

public:
 DetectionDeliveryPump;

 bool init(BLEManager* bleManager,
 HistoryService* historyService,
 ProtocolHandler* protocolHandler);

 void update;

 uint32_t getSuccessCount const { return _pumpSuccessCount; }

 uint32_t getSkipCount const { return _pumpSkipCount; }

 void resetStats {
 _pumpSuccessCount = 0;
 _pumpSkipCount = 0;
 }
};

#endif
