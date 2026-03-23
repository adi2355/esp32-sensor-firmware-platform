
#ifndef HISTORY_SERVICE_H
#define HISTORY_SERVICE_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "../Config.h"
#include "../Domain/DetectionEvent.h"
#include "../Domain/ProtocolDefs.h"

enum class StorageCmd : uint8_t {
 SAVE_HIT,
 FORCE_FLUSH,
 RESTORE,
 CLEAR_NVS
};

struct StorageMessage {
 StorageCmd cmd;
 uint32_t eventId;
};

struct RTCState {
 uint32_t magicNumber;
 
 uint32_t crc32;
 
 uint32_t bootCount;
 
 uint16_t head;
 uint16_t tail;
 
 uint32_t globalEventId;
 
 DetectionEvent buffer[HistoryConfig::BUFFER_SIZE];
 
 uint8_t reserved[2];
};

class HistoryService {
private:
 static RTC_DATA_ATTR RTCState _state;
 
 SemaphoreHandle_t _stateMutex;
 
 QueueHandle_t _storageQueue;
 
 TaskHandle_t _storageTaskHandle;
 
 portMUX_TYPE _mux;
 
 bool _initialized;
 
 volatile bool _storageWorkerRunning;
 
 uint32_t _lastNvsFlushId;
 uint8_t _eventsSinceFlush;
 
 volatile bool _timeSyncReceivedThisBoot;
 
 volatile bool _flushPending;
 volatile bool _bleConnected;
 volatile unsigned long _connectionStartTime;
 
 uint32_t calculateBufferCRC const;
 
 void updateCRC;
 
 bool validateState const;
 
 void resetState;
 
 uint16_t getEventCount const;
 
 static void storageWorker(void* pvParameters);
 
 void processStorageCommand(const StorageMessage& msg);
 
public:
 HistoryService;
 
 ~HistoryService;
 
 void init;
 
 bool startStorageWorker;
 
 void stopStorageWorker;
 
 uint32_t logDetection(uint16_t durationMs);
 
 bool queueStorageCommand(StorageCmd cmd, uint32_t eventId = 0);
 
 bool getNextUnsynced(DetectionEvent* outEvent);
 
 bool getUnsyncedEvents(DetectionEvent* outEvents, uint8_t maxCount, uint8_t* outCount);
 
 bool markAsSynced(uint32_t eventId);
 
 uint8_t markMultipleAsSynced(const uint32_t* eventIds, uint8_t count);
 
 bool hasPendingData const;
 
 uint16_t getPendingCount const;
 
 uint32_t getBootCount const;
 
 uint32_t getLatestEventId const;
 
 void fastForwardEventId(uint32_t newId);
 
 void setBleConnected(bool connected);
 
 bool getEventsSince(uint32_t sinceId, 
 DetectionEvent* outEvents, 
 uint8_t maxCount, 
 uint8_t* outCount);
 
 void clear;
 
 uint16_t getCapacity const { return HistoryConfig::BUFFER_SIZE; }
 
 uint16_t getStoredCount const;
 
 void forceUpdateCRC { updateCRC; }
 
 bool hasOverflowed const;
 
 void getDiagnostics(char* buffer, size_t bufferSize) const;
 
 
 bool flushToNvs;
 
 uint8_t restoreFromNvs;
 
 bool needsNvsFlush const;
 
 void forceNvsSync;
 
 bool isFlushing const { return _flushPending; }
 
 void clearNvs;
 
 
 void applyTimeSync(uint32_t currentEpoch);
 
 void onSystemTimeSynced(uint32_t currentEpoch);
 
private:
 bool saveMetadataToNvs;
 
 bool loadMetadataFromNvs;
};

#endif

