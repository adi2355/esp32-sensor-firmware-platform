
#include "HistoryService.h"
#include "BLEManager.h"
#include "PowerManager.h"
#include "ConfigService.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include "../HAL/HAL_Base.h"

static const char* TAG = "HISTORY";

extern PowerManager powerManager;

static const char* NVS_NAMESPACE = "devicedata";
static const char* NVS_KEY_BOOT_COUNT = "bootCnt";
static const char* NVS_KEY_EVENT_ID = "eventId";
static const char* NVS_KEY_HEAD = "head";
static const char* NVS_KEY_TAIL = "tail";
static const char* NVS_KEY_EVENTS = "events";

RTC_DATA_ATTR RTCState HistoryService::_state;

HistoryService::HistoryService
 : _stateMutex(nullptr)
 , _storageQueue(nullptr)
 , _storageTaskHandle(nullptr)
 , _mux(portMUX_INITIALIZER_UNLOCKED)
 , _initialized(false)
 , _storageWorkerRunning(false)
 , _lastNvsFlushId(0)
 , _eventsSinceFlush(0)
 , _timeSyncReceivedThisBoot(false)
 , _flushPending(false)
 , _bleConnected(false)
 , _connectionStartTime(0)
{
 
 _stateMutex = xSemaphoreCreateMutex;
 if (_stateMutex == nullptr) {
 ESP_LOGE(TAG, "FATAL: Failed to create state mutex!");
 }
 
 _storageQueue = xQueueCreate(
 FreeRTOSConfig::STORAGE_QUEUE_DEPTH,
 sizeof(StorageMessage)
 );
 if (_storageQueue == nullptr) {
 ESP_LOGE(TAG, "FATAL: Failed to create storage queue!");
 }
 
 ESP_LOGI(TAG, "HistoryService constructed: mutex=%p, queue=%p",
 _stateMutex, _storageQueue);
}

HistoryService::~HistoryService {
 stopStorageWorker;
 
 if (_stateMutex != nullptr) {
 vSemaphoreDelete(_stateMutex);
 _stateMutex = nullptr;
 }
 
 if (_storageQueue != nullptr) {
 vQueueDelete(_storageQueue);
 _storageQueue = nullptr;
 }
 
 ESP_LOGI(TAG, "HistoryService destroyed");
}

void HistoryService::init {
 ESP_LOGI(TAG, "Initializing HistoryService (- Offline-Capable, Session Continuity)");
 
 if (_stateMutex == nullptr || _storageQueue == nullptr) {
 ESP_LOGE(TAG, "FATAL: FreeRTOS primitives not created! mutex=%p, queue=%p",
 _stateMutex, _storageQueue);
 if (_stateMutex == nullptr) {
 _stateMutex = xSemaphoreCreateMutex;
 }
 if (_storageQueue == nullptr) {
 _storageQueue = xQueueCreate(FreeRTOSConfig::STORAGE_QUEUE_DEPTH, sizeof(StorageMessage));
 }
 }
 
 esp_reset_reason_t resetReason = esp_reset_reason;
 
 
 bool isColdBoot = (resetReason == ESP_RST_POWERON) ||
 (resetReason == ESP_RST_BROWNOUT) ||
 (_state.magicNumber != HistoryConfig::RTC_MAGIC_NUMBER);
 
 if (isColdBoot) {
 ESP_LOGI(TAG, "Cold boot detected (reason=%d, magic=0x%08X) - Starting NEW session.",
 resetReason, _state.magicNumber);
 
 uint8_t restoredCount = restoreFromNvs;
 
 if (restoredCount > 0) {
 ESP_LOGI(TAG, "Restored %u events from NVS backup", restoredCount);
 _state.bootCount++;
 ESP_LOGI(TAG, "Session bootCount incremented to %lu (cold boot with restore)", _state.bootCount);
 } else {
 ESP_LOGI(TAG, "No NVS backup found, resetting state");
 resetState;
 _state.bootCount = 1;
 ESP_LOGI(TAG, "Session bootCount set to 1 (fresh cold boot)");
 }
 } else {
 
 if (!validateState) {
 ESP_LOGW(TAG, "RTC state corruption detected! CRC mismatch.");
 
 uint8_t restoredCount = restoreFromNvs;
 
 if (restoredCount > 0) {
 ESP_LOGI(TAG, "Recovered %u events from NVS backup", restoredCount);
 _state.bootCount++;
 ESP_LOGI(TAG, "Session bootCount incremented to %lu (warm boot recovery)", _state.bootCount);
 } else {
 ESP_LOGW(TAG, "No backup available, resetting state");
 resetState;
 _state.bootCount = 1;
 }
 } else {
 ESP_LOGI(TAG, "Warm boot (Deep Sleep wake): Continuing session %lu. Events: %u",
 _state.bootCount, getEventCount);
 }
 }
 
 
 updateCRC;
 
 _lastNvsFlushId = _state.globalEventId;
 _eventsSinceFlush = 0;
 
 _initialized = true;
 
 if (!startStorageWorker) {
 ESP_LOGE(TAG, "WARNING: Storage worker failed to start! NVS writes will be synchronous.");
 }
 
 ESP_LOGI(TAG, "HistoryService initialized. Capacity: %u, Used: %u, Pending: %u",
 HistoryConfig::BUFFER_SIZE, getStoredCount, getPendingCount);
}

uint32_t HistoryService::logDetection(uint16_t durationMs) {
 if (!_initialized) {
 ESP_LOGE(TAG, "logDetection called before init!");
 return 0;
 }
 
 if (_stateMutex == nullptr) {
 ESP_LOGE(TAG, "logDetection: mutex is NULL!");
 return 0;
 }
 
 uint32_t eventId = 0;
 bool overflow = false;
 
 const TickType_t mutexTimeout = pdMS_TO_TICKS(100);
 
 if (xSemaphoreTake(_stateMutex, mutexTimeout) != pdTRUE) {
 ESP_LOGE(TAG, "logDetection: Failed to acquire mutex (timeout)!");
 return 0;
 }
 
 
 uint16_t nextHead = (_state.head + 1) % HistoryConfig::BUFFER_SIZE;
 
 if (nextHead == _state.tail) {
 _state.tail = (_state.tail + 1) % HistoryConfig::BUFFER_SIZE;
 overflow = true;
 }
 
 DetectionEvent* event = &_state.buffer[_state.head];
 event->id = ++_state.globalEventId;
 
 event->timestampMs = HAL::getSystemUptime;
 
 event->bootCount = _state.bootCount;
 event->durationMs = durationMs;
 
 
 constexpr uint32_t MIN_VALID_EPOCH = 1577836800;
 
 uint32_t currentEpoch = time(NULL);
 
 if (currentEpoch >= MIN_VALID_EPOCH) {
 event->timestamp = currentEpoch;
 
 if (!_timeSyncReceivedThisBoot) {
 event->setTimeEstimated;
 ESP_LOGD(TAG, "Event %lu: Forward-chained timestamp=%lu (TIME_ESTIMATED)", 
 event->id, currentEpoch);
 } else {
 ESP_LOGD(TAG, "Event %lu: Accurate timestamp=%lu (post-TIME_SYNC)", 
 event->id, currentEpoch);
 }

 ConfigService::getInstance->updateLastKnownEpoch(currentEpoch);
 } else {
 event->timestamp = 0;
 ESP_LOGD(TAG, "Event %lu: No valid time source, timestamp=0 (will backfill)", event->id);
 }
 
 memset(event->reserved, 0, sizeof(event->reserved));
 
 event->clearFlags;
 
 if (overflow) {
 event->setOverflow;
 }
 
 _state.head = nextHead;
 
 updateCRC;
 
 eventId = event->id;
 
 _eventsSinceFlush++;
 bool shouldFlush = needsNvsFlush;
 
 xSemaphoreGive(_stateMutex);
 
 ESP_LOGI(TAG, "Hit logged: ID=%lu, Duration=%ums, Overflow=%d",
 eventId, durationMs, overflow);
 
 
 constexpr uint16_t CRITICAL_FLUSH_THRESHOLD = (HistoryConfig::BUFFER_SIZE * 9) / 10;
 
 uint16_t currentEventCount = getStoredCount;
 bool isCriticallyFull = (currentEventCount >= CRITICAL_FLUSH_THRESHOLD);
 bool bleIsConnected = _bleConnected;
 bool flushAlreadyPending = _flushPending;
 
 bool shouldQueueFlush = false;
 const char* flushReason = nullptr;
 
 if (isCriticallyFull) {
 shouldQueueFlush = true;
 flushReason = "CRITICAL (buffer 90%+ full)";
 } else if (shouldFlush && !bleIsConnected) {
 shouldQueueFlush = true;
 flushReason = "normal (BLE disconnected)";
 } else if (shouldFlush && bleIsConnected) {
 shouldQueueFlush = false;
 flushReason = "deferred (BLE connected, waiting for disconnect)";
 }
 
 if (shouldQueueFlush && !flushAlreadyPending) {
 _flushPending = true;
 
 if (!queueStorageCommand(StorageCmd::SAVE_HIT, eventId)) {
 _flushPending = false;
 ESP_LOGW(TAG, "Storage queue full! NVS backup deferred.");
 } else {
 ESP_LOGD(TAG, "NVS flush queued: %s, events=%u", flushReason, currentEventCount);
 }
 } else if (shouldFlush && flushAlreadyPending) {
 ESP_LOGD(TAG, "NVS flush skipped: already pending, events=%u", currentEventCount);
 } else if (shouldFlush && bleIsConnected) {
 ESP_LOGD(TAG, "NVS flush deferred: BLE connected, events=%u", currentEventCount);
 }
 
 return eventId;
}

bool HistoryService::getNextUnsynced(DetectionEvent* outEvent) {
 if (!_initialized || _stateMutex == nullptr || outEvent == nullptr) {
 return false;
 }
 
 bool found = false;
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint16_t current = _state.tail;
 
 while (current != _state.head) {
 if (!_state.buffer[current].isSynced) {
 *outEvent = _state.buffer[current];
 found = true;
 break;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 xSemaphoreGive(_stateMutex);
 }
 
 return found;
}

bool HistoryService::getUnsyncedEvents(DetectionEvent* outEvents, uint8_t maxCount, uint8_t* outCount) {
 if (!_initialized || outEvents == nullptr || outCount == nullptr) {
 return false;
 }
 if (_stateMutex == nullptr) return false;
 
 *outCount = 0;
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint16_t current = _state.tail;
 
 while (current != _state.head && *outCount < maxCount) {
 if (!_state.buffer[current].isSynced) {
 outEvents[*outCount] = _state.buffer[current];
 (*outCount)++;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 xSemaphoreGive(_stateMutex);
 }
 
 return true;
}

bool HistoryService::markAsSynced(uint32_t eventId) {
 if (!_initialized || _stateMutex == nullptr) return false;
 
 bool found = false;
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint16_t current = _state.tail;
 
 while (current != _state.head) {
 if (_state.buffer[current].id == eventId) {
 _state.buffer[current].setSynced;
 found = true;
 updateCRC;
 break;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 xSemaphoreGive(_stateMutex);
 }
 
 if (found) {
 ESP_LOGD(TAG, "Event %lu marked as synced", eventId);
 } else {
 ESP_LOGW(TAG, "Event %lu not found for sync marking", eventId);
 }
 
 return found;
}

uint8_t HistoryService::markMultipleAsSynced(const uint32_t* eventIds, uint8_t count) {
 if (!_initialized || eventIds == nullptr || count == 0) {
 return 0;
 }
 if (_stateMutex == nullptr) return 0;
 
 uint8_t markedCount = 0;
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 for (uint8_t i = 0; i < count; i++) {
 uint16_t current = _state.tail;
 
 while (current != _state.head) {
 if (_state.buffer[current].id == eventIds[i]) {
 if (!_state.buffer[current].isSynced) {
 _state.buffer[current].setSynced;
 markedCount++;
 }
 break;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 }
 
 if (markedCount > 0) {
 updateCRC;
 }
 
 xSemaphoreGive(_stateMutex);
 }
 
 ESP_LOGD(TAG, "Marked %u/%u events as synced", markedCount, count);
 
 return markedCount;
}

bool HistoryService::hasPendingData const {
 if (!_initialized || _stateMutex == nullptr) return false;
 
 bool hasPending = false;
 
 if (xSemaphoreTake(const_cast<SemaphoreHandle_t>(_stateMutex), pdMS_TO_TICKS(50)) == pdTRUE) {
 uint16_t current = _state.tail;
 
 while (current != _state.head) {
 if (!_state.buffer[current].isSynced) {
 hasPending = true;
 break;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 xSemaphoreGive(const_cast<SemaphoreHandle_t>(_stateMutex));
 }
 
 return hasPending;
}

uint16_t HistoryService::getPendingCount const {
 if (!_initialized || _stateMutex == nullptr) return 0;
 
 uint16_t count = 0;
 
 if (xSemaphoreTake(const_cast<SemaphoreHandle_t>(_stateMutex), pdMS_TO_TICKS(50)) == pdTRUE) {
 uint16_t current = _state.tail;
 
 while (current != _state.head) {
 if (!_state.buffer[current].isSynced) {
 count++;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 xSemaphoreGive(const_cast<SemaphoreHandle_t>(_stateMutex));
 }
 
 return count;
}

uint32_t HistoryService::getBootCount const {
 return _state.bootCount;
}

uint32_t HistoryService::getLatestEventId const {
 return _state.globalEventId;
}

uint16_t HistoryService::getStoredCount const {
 return getEventCount;
}

bool HistoryService::hasOverflowed const {
 if (_stateMutex == nullptr) return false;
 
 bool overflow = false;
 
 if (xSemaphoreTake(const_cast<SemaphoreHandle_t>(_stateMutex), pdMS_TO_TICKS(50)) == pdTRUE) {
 uint16_t current = _state.tail;
 
 while (current != _state.head) {
 if (_state.buffer[current].wasOverflow) {
 overflow = true;
 break;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 xSemaphoreGive(const_cast<SemaphoreHandle_t>(_stateMutex));
 }
 
 return overflow;
}

bool HistoryService::getEventsSince(uint32_t sinceId, 
 DetectionEvent* outEvents, 
 uint8_t maxCount, 
 uint8_t* outCount) {
 if (!_initialized || outEvents == nullptr || outCount == nullptr) {
 return false;
 }
 if (_stateMutex == nullptr) return false;
 
 *outCount = 0;
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint16_t current = _state.tail;
 
 while (current != _state.head && *outCount < maxCount) {
 if (_state.buffer[current].id > sinceId) {
 outEvents[*outCount] = _state.buffer[current];
 (*outCount)++;
 }
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 xSemaphoreGive(_stateMutex);
 }
 
 return true;
}

void HistoryService::clear {
 if (_stateMutex == nullptr) return;
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 _state.head = 0;
 _state.tail = 0;
 _state.globalEventId = 0;
 memset(_state.buffer, 0, sizeof(_state.buffer));
 updateCRC;
 
 xSemaphoreGive(_stateMutex);
 }
 
 ESP_LOGI(TAG, "History buffer cleared");
}

void HistoryService::getDiagnostics(char* buffer, size_t bufferSize) const {
 snprintf(buffer, bufferSize,
 "History: boot=%lu, id=%lu, stored=%u, pending=%u, head=%u, tail=%u, crc=0x%08X",
 _state.bootCount,
 _state.globalEventId,
 getStoredCount,
 getPendingCount,
 _state.head,
 _state.tail,
 _state.crc32);
}


uint32_t HistoryService::calculateBufferCRC const {
 return calculateCRC32(
 reinterpret_cast<const uint8_t*>(_state.buffer),
 sizeof(_state.buffer)
 );
}

void HistoryService::updateCRC {
 _state.crc32 = calculateBufferCRC;
}

bool HistoryService::validateState const {
 if (_state.magicNumber != HistoryConfig::RTC_MAGIC_NUMBER) {
 return false;
 }
 
 uint32_t calculatedCRC = calculateBufferCRC;
 return (calculatedCRC == _state.crc32);
}

void HistoryService::resetState {
 memset(&_state, 0, sizeof(RTCState));
 _state.magicNumber = HistoryConfig::RTC_MAGIC_NUMBER;
 _state.bootCount = 0;
 _state.head = 0;
 _state.tail = 0;
 _state.globalEventId = 0;
 updateCRC;
}

uint16_t HistoryService::getEventCount const {
 if (_state.head >= _state.tail) {
 return _state.head - _state.tail;
 } else {
 return HistoryConfig::BUFFER_SIZE - _state.tail + _state.head;
 }
}


bool HistoryService::flushToNvs {
 if (!_initialized) return false;
 
 if (powerManager.isBatteryCritical && !powerManager.isCharging) {
 ESP_LOGW(TAG, "NVS flush SKIPPED - Battery Critical!");
 ESP_LOGW(TAG, " Battery: %u%%, Charging: No", powerManager.getBatteryPercent);
 ESP_LOGW(TAG, " Data preserved in RTC RAM only. Charge device to persist.");
 return false;
 }
 
 BLEManager* ble = BLEManager::getInstance;
 if (ble != nullptr && ble->isPairingModeEnabled) {
 ESP_LOGW(TAG, "NVS flush deferred - Pairing Active");
 return false;
 }
 
 ESP_LOGI(TAG, "Flushing to NVS...");
 
 DetectionEvent localBuffer[HistoryConfig::BUFFER_SIZE];
 uint32_t localBootCount;
 uint32_t localEventId;
 uint16_t localHead;
 uint16_t localTail;
 
 if (_stateMutex == nullptr) {
 ESP_LOGE(TAG, "flushToNvs: mutex is NULL!");
 return false;
 }
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
 ESP_LOGE(TAG, "flushToNvs: Failed to acquire mutex!");
 return false;
 }
 
 localBootCount = _state.bootCount;
 localEventId = _state.globalEventId;
 localHead = _state.head;
 localTail = _state.tail;
 memcpy(localBuffer, _state.buffer, sizeof(localBuffer));
 _lastNvsFlushId = _state.globalEventId;
 _eventsSinceFlush = 0;
 xSemaphoreGive(_stateMutex);
 
 
 esp_task_wdt_reset;
 
 Preferences prefs;
 if (!prefs.begin(NVS_NAMESPACE, false)) {
 ESP_LOGE(TAG, "Failed to open NVS namespace");
 return false;
 }
 
 bool success = true;
 success &= prefs.putULong(NVS_KEY_BOOT_COUNT, localBootCount);
 success &= prefs.putULong(NVS_KEY_EVENT_ID, localEventId);
 success &= prefs.putUShort(NVS_KEY_HEAD, localHead);
 success &= prefs.putUShort(NVS_KEY_TAIL, localTail);
 
 esp_task_wdt_reset;
 
 size_t bufferSize = HistoryConfig::BUFFER_SIZE * sizeof(DetectionEvent);
 size_t written = 0;
 
 for (int attempt = 0; attempt < 3; attempt++) {
 written = prefs.putBytes(NVS_KEY_EVENTS, localBuffer, bufferSize);
 
 if (written == bufferSize) {
 if (attempt > 0) {
 ESP_LOGI(TAG, "NVS write succeeded on retry %d", attempt);
 }
 break;
 }
 
 ESP_LOGW(TAG, "NVS write attempt %d failed: %u/%u bytes", 
 attempt + 1, written, bufferSize);
 
 esp_task_wdt_reset;
 
 delay(10);
 }
 
 esp_task_wdt_reset;
 
 prefs.end;
 
 if (written != bufferSize) {
 ESP_LOGE(TAG, "NVS write FAILED after 3 attempts: %u/%u bytes", written, bufferSize);
 return false;
 }
 
 if (!success) {
 ESP_LOGE(TAG, "NVS metadata write failed");
 return false;
 }
 
 ESP_LOGI(TAG, "NVS flush complete: eventId=%lu, stored=%u events",
 localEventId, getStoredCount);
 
 return true;
}

uint8_t HistoryService::restoreFromNvs {
 ESP_LOGI(TAG, "Attempting NVS restore...");
 
 Preferences prefs;
 if (!prefs.begin(NVS_NAMESPACE, true)) {
 ESP_LOGW(TAG, "No NVS backup found");
 return 0;
 }
 
 if (!prefs.isKey(NVS_KEY_EVENT_ID)) {
 ESP_LOGW(TAG, "NVS backup empty");
 prefs.end;
 return 0;
 }
 
 uint32_t nvsBootCount = prefs.getULong(NVS_KEY_BOOT_COUNT, 0);
 uint32_t nvsEventId = prefs.getULong(NVS_KEY_EVENT_ID, 0);
 uint16_t nvsHead = prefs.getUShort(NVS_KEY_HEAD, 0);
 uint16_t nvsTail = prefs.getUShort(NVS_KEY_TAIL, 0);
 
 DetectionEvent nvsBuffer[HistoryConfig::BUFFER_SIZE];
 size_t bufferSize = HistoryConfig::BUFFER_SIZE * sizeof(DetectionEvent);
 size_t restored = prefs.getBytes(NVS_KEY_EVENTS, nvsBuffer, bufferSize);
 
 prefs.end;
 
 uint16_t eventCount = 0;
 
 if (_stateMutex != nullptr && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
 _state.bootCount = nvsBootCount;
 _state.globalEventId = nvsEventId;
 _state.head = nvsHead;
 _state.tail = nvsTail;
 memcpy(_state.buffer, nvsBuffer, sizeof(_state.buffer));
 
 _state.magicNumber = HistoryConfig::RTC_MAGIC_NUMBER;
 
 updateCRC;
 
 _lastNvsFlushId = _state.globalEventId;
 _eventsSinceFlush = 0;
 
 eventCount = getEventCount;
 
 xSemaphoreGive(_stateMutex);
 }
 
 ESP_LOGI(TAG, "NVS restore complete: bootCount=%lu, eventId=%lu, events=%u",
 nvsBootCount, nvsEventId, eventCount);
 
 return (uint8_t)eventCount;
}

bool HistoryService::needsNvsFlush const {
 return _eventsSinceFlush >= HistoryConfig::NVS_FLUSH_THRESHOLD;
}

void HistoryService::forceNvsSync {
 flushToNvs;
}

void HistoryService::clearNvs {
 Preferences prefs;
 if (prefs.begin(NVS_NAMESPACE, false)) {
 prefs.clear;
 prefs.end;
 ESP_LOGI(TAG, "NVS backup cleared");
 }
}

bool HistoryService::saveMetadataToNvs {
 if (_stateMutex == nullptr) return false;
 
 uint32_t bootCount, eventId;
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 bootCount = _state.bootCount;
 eventId = _state.globalEventId;
 xSemaphoreGive(_stateMutex);
 } else {
 return false;
 }
 
 Preferences prefs;
 if (!prefs.begin(NVS_NAMESPACE, false)) {
 return false;
 }
 
 prefs.putULong(NVS_KEY_BOOT_COUNT, bootCount);
 prefs.putULong(NVS_KEY_EVENT_ID, eventId);
 
 prefs.end;
 return true;
}

bool HistoryService::loadMetadataFromNvs {
 Preferences prefs;
 if (!prefs.begin(NVS_NAMESPACE, true)) {
 return false;
 }
 
 if (!prefs.isKey(NVS_KEY_BOOT_COUNT)) {
 prefs.end;
 return false;
 }
 
 uint32_t bootCount = prefs.getULong(NVS_KEY_BOOT_COUNT, 0);
 uint32_t eventId = prefs.getULong(NVS_KEY_EVENT_ID, 0);
 prefs.end;
 
 if (_stateMutex != nullptr && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 _state.bootCount = bootCount;
 _state.globalEventId = eventId;
 xSemaphoreGive(_stateMutex);
 return true;
 }
 
 return false;
}


bool HistoryService::queueStorageCommand(StorageCmd cmd, uint32_t eventId) {
 if (_storageQueue == nullptr) {
 ESP_LOGE(TAG, "queueStorageCommand: Queue is NULL!");
 return false;
 }
 
 StorageMessage msg;
 msg.cmd = cmd;
 msg.eventId = eventId;
 
 if (xQueueSend(_storageQueue, &msg, 0) != pdTRUE) {
 ESP_LOGW(TAG, "Storage queue full! cmd=%d, eventId=%lu",
 static_cast<int>(cmd), eventId);
 return false;
 }
 
 ESP_LOGD(TAG, "Queued storage cmd=%d, eventId=%lu", 
 static_cast<int>(cmd), eventId);
 return true;
}

bool HistoryService::startStorageWorker {
 if (_storageWorkerRunning) {
 ESP_LOGW(TAG, "Storage worker already running");
 return true;
 }
 
 if (_storageQueue == nullptr) {
 ESP_LOGE(TAG, "Cannot start worker: queue is NULL");
 return false;
 }
 
 ESP_LOGI(TAG, "Starting Storage Worker task...");
 
 BaseType_t result = xTaskCreatePinnedToCore(
 storageWorker,
 "StorageWorker",
 FreeRTOSConfig::STORAGE_TASK_STACK_SIZE,
 this,
 FreeRTOSConfig::STORAGE_TASK_PRIORITY,
 &_storageTaskHandle,
 FreeRTOSConfig::APP_CORE
 );
 
 if (result != pdPASS) {
 ESP_LOGE(TAG, "Failed to create Storage Worker task! result=%d", result);
 _storageTaskHandle = nullptr;
 return false;
 }
 
 _storageWorkerRunning = true;
 ESP_LOGI(TAG, "Storage Worker started: handle=%p, priority=%d, core=%d",
 _storageTaskHandle,
 FreeRTOSConfig::STORAGE_TASK_PRIORITY,
 FreeRTOSConfig::APP_CORE);
 
 return true;
}

void HistoryService::stopStorageWorker {
 if (!_storageWorkerRunning && _storageTaskHandle == nullptr) {
 ESP_LOGD(TAG, "Storage Worker already stopped");
 return;
 }
 
 ESP_LOGI(TAG, "[HISTORY] Stopping Storage Worker (Safe Termination)...");
 
 _storageWorkerRunning = false;
 
 if (_storageTaskHandle != nullptr) {
 ESP_LOGD(TAG, "Waiting for active operations to complete...");
 vTaskDelay(pdMS_TO_TICKS(200));
 }
 
 StorageMessage msg;
 int processedCount = 0;
 while (xQueueReceive(_storageQueue, &msg, 0) == pdTRUE) {
 ESP_LOGI(TAG, "Processing remaining storage cmd=%d", static_cast<int>(msg.cmd));
 processStorageCommand(msg);
 processedCount++;
 
 esp_task_wdt_reset;
 }
 if (processedCount > 0) {
 ESP_LOGI(TAG, "Processed %d remaining queue items", processedCount);
 }
 
 if (_storageTaskHandle != nullptr) {
 ESP_LOGI(TAG, "Deleting Storage Worker task (handle=%p)...", _storageTaskHandle);
 
 vTaskDelete(_storageTaskHandle);
 _storageTaskHandle = nullptr;
 
 ESP_LOGI(TAG, "Storage Worker task deleted successfully");
 }
 
 ESP_LOGI(TAG, "[HISTORY] Storage Worker stopped safely ");
}

void HistoryService::storageWorker(void* pvParameters) {
 HistoryService* self = static_cast<HistoryService*>(pvParameters);
 
 if (self == nullptr) {
 ESP_LOGE(TAG, "storageWorker: NULL instance! Task will be killed by manager.");
 esp_task_wdt_add(nullptr);
 while (true) {
 esp_task_wdt_reset;
 vTaskDelay(pdMS_TO_TICKS(1000));
 }
 }
 
 ESP_LOGI(TAG, "Storage Worker started on core %d", xPortGetCoreID);
 
 esp_task_wdt_add(nullptr);
 
 StorageMessage msg;
 
 while (true) {
 if (!self->_storageWorkerRunning) {
 esp_task_wdt_reset;
 vTaskDelay(pdMS_TO_TICKS(100));
 continue;
 }
 
 if (xQueueReceive(self->_storageQueue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
 
 if (!self->_storageWorkerRunning) {
 xQueueSendToFront(self->_storageQueue, &msg, 0);
 continue;
 }
 
 esp_task_wdt_reset;
 
 ESP_LOGD(TAG, "Worker processing cmd=%d, eventId=%lu",
 static_cast<int>(msg.cmd), msg.eventId);
 
 if (self->_bleConnected) {
 unsigned long connDuration = 0;
 if (self->_connectionStartTime > 0) {
 connDuration = millis - self->_connectionStartTime;
 }
 
 constexpr unsigned long HANDSHAKE_WINDOW_MS = 4000;
 
 if (connDuration < HANDSHAKE_WINDOW_MS) {
 ESP_LOGD(TAG, "Pre-flash yield (handshake window, age=%lu ms)", connDuration);
 vTaskDelay(pdMS_TO_TICKS(100));
 } else {
 ESP_LOGD(TAG, "Pre-flash yield (stable, age=%lu ms)", connDuration);
 vTaskDelay(pdMS_TO_TICKS(50));
 }
 esp_task_wdt_reset;
 }
 
 self->processStorageCommand(msg);
 
 esp_task_wdt_reset;
 
 uint32_t yieldMs;
 if (!self->_bleConnected) {
 yieldMs = FreeRTOSConfig::STORAGE_TASK_YIELD_MS;
 } else {
 unsigned long connDuration = 0;
 if (self->_connectionStartTime > 0) {
 connDuration = millis - self->_connectionStartTime;
 }
 
 constexpr unsigned long HANDSHAKE_WINDOW_MS = 4000;
 if (connDuration < HANDSHAKE_WINDOW_MS) {
 yieldMs = 150;
 } else {
 yieldMs = 100;
 }
 }
 vTaskDelay(pdMS_TO_TICKS(yieldMs));
 }
 
 esp_task_wdt_reset;
 }
 
}

void HistoryService::processStorageCommand(const StorageMessage& msg) {
 switch (msg.cmd) {
 case StorageCmd::SAVE_HIT:
 case StorageCmd::FORCE_FLUSH: {
 bool success = flushToNvs;
 
 _flushPending = false;
 
 if (success) {
 ESP_LOGD(TAG, "NVS flush complete (triggered by eventId=%lu)", msg.eventId);
 } else {
 ESP_LOGW(TAG, "NVS flush failed!");
 }
 break;
 }
 
 case StorageCmd::RESTORE: {
 uint8_t restored = restoreFromNvs;
 ESP_LOGI(TAG, "Restored %u events from NVS", restored);
 break;
 }
 
 case StorageCmd::CLEAR_NVS: {
 clearNvs;
 ESP_LOGI(TAG, "NVS cleared");
 break;
 }
 
 default:
 ESP_LOGW(TAG, "Unknown storage command: %d", static_cast<int>(msg.cmd));
 break;
 }
}


void HistoryService::applyTimeSync(uint32_t currentEpoch) {
 if (!_initialized || _stateMutex == nullptr) {
 ESP_LOGE(TAG, "applyTimeSync: Not initialized or mutex NULL");
 return;
 }
 
 struct timeval tv;
 tv.tv_sec = currentEpoch;
 tv.tv_usec = (millis % 1000) * 1000;
 
 int result = settimeofday(&tv, NULL);
 if (result == 0) {
 ESP_LOGI(TAG, "applyTimeSync: System clock updated to epoch %lu", currentEpoch);
 } else {
 ESP_LOGW(TAG, "applyTimeSync: Failed to update system clock (errno=%d)", errno);
 }
 
 _timeSyncReceivedThisBoot = true;
 
 uint32_t currentUptime = HAL::getSystemUptime;
 
 ESP_LOGI(TAG, "applyTimeSync: epoch=%lu, uptime=%lu ms (TIME_SYNC received)", currentEpoch, currentUptime);
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
 uint16_t current = _state.tail;
 uint32_t updatedCount = 0;
 uint32_t skippedDifferentBoot = 0;
 uint32_t skippedAlreadySet = 0;
 
 while (current != _state.head) {
 DetectionEvent* evt = &_state.buffer[current];
 
 if (evt->bootCount == _state.bootCount) {
 if (evt->timestamp == 0) {
 uint32_t ageMs = 0;
 if (currentUptime >= evt->timestampMs) {
 ageMs = currentUptime - evt->timestampMs;
 } else {
 ESP_LOGW(TAG, "Uptime wraparound detected for event %lu", evt->id);
 ageMs = 0;
 }
 
 
 uint32_t bootEpoch = 0;
 uint32_t uptimeSec = currentUptime / 1000;
 if (currentEpoch >= uptimeSec) {
 bootEpoch = currentEpoch - uptimeSec;
 } else {
 bootEpoch = 0;
 }
 
 evt->timestamp = bootEpoch + (evt->timestampMs / 1000);
 
 updatedCount++;
 
 ESP_LOGD(TAG, "Backfilled event %lu: uptime=%lu -> epoch=%lu (bootAnchor=%lu)",
 evt->id, evt->timestampMs, evt->timestamp, bootEpoch);
 } else {
 skippedAlreadySet++;
 }
 } else {
 skippedDifferentBoot++;
 }
 
 current = (current + 1) % HistoryConfig::BUFFER_SIZE;
 }
 
 if (updatedCount > 0) {
 updateCRC;
 
 ESP_LOGI(TAG, "Time Sync Backfill: Updated %lu events with absolute timestamps",
 updatedCount);
 
 if (_storageWorkerRunning && !_flushPending) {
 _flushPending = true;
 if (!queueStorageCommand(StorageCmd::SAVE_HIT, 0)) {
 _flushPending = false;
 ESP_LOGW(TAG, "Failed to queue NVS flush after time sync backfill");
 }
 }
 }
 
 if (skippedDifferentBoot > 0 || skippedAlreadySet > 0) {
 ESP_LOGD(TAG, "Time Sync Backfill: Skipped %lu (different boot), %lu (already set)",
 skippedDifferentBoot, skippedAlreadySet);
 }
 
 xSemaphoreGive(_stateMutex);
 } else {
 ESP_LOGE(TAG, "applyTimeSync: Failed to acquire mutex");
 }
}


void HistoryService::onSystemTimeSynced(uint32_t currentEpoch) {
 ESP_LOGI(TAG, "System Time Sync (NTP): Received epoch %lu", currentEpoch);
 
 applyTimeSync(currentEpoch);
}


void HistoryService::setBleConnected(bool connected) {
 bool wasConnected = _bleConnected;
 _bleConnected = connected;
 
 if (!wasConnected && connected) {
 _connectionStartTime = millis;
 ESP_LOGI(TAG, "BLE connected - tracking connection start for yield timing");
 }
 
 if (wasConnected && !connected) {
 ESP_LOGI(TAG, "BLE disconnected - checking for deferred flush");
 
 _connectionStartTime = 0;
 
 if (needsNvsFlush && !_flushPending) {
 _flushPending = true;
 if (!queueStorageCommand(StorageCmd::SAVE_HIT, _state.globalEventId)) {
 _flushPending = false;
 ESP_LOGW(TAG, "Failed to queue deferred flush on disconnect");
 } else {
 ESP_LOGI(TAG, "Deferred NVS flush queued (BLE disconnected)");
 }
 }
 }
 
 ESP_LOGD(TAG, "BLE connection state: %s -> %s", 
 wasConnected ? "connected" : "disconnected",
 connected ? "connected" : "disconnected");
}


void HistoryService::fastForwardEventId(uint32_t newId) {
 if (!_initialized) {
 ESP_LOGE(TAG, "fastForwardEventId called before init!");
 return;
 }
 
 if (_stateMutex == nullptr) {
 ESP_LOGE(TAG, "fastForwardEventId: mutex is NULL!");
 return;
 }
 
 bool didUpdate = false;
 uint32_t oldId = 0;
 
 if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 if (newId > _state.globalEventId) {
 oldId = _state.globalEventId;
 _state.globalEventId = newId;
 
 updateCRC;
 
 _eventsSinceFlush = HistoryConfig::NVS_FLUSH_THRESHOLD;
 
 didUpdate = true;
 }
 
 xSemaphoreGive(_stateMutex);
 }
 
 if (didUpdate) {
 ESP_LOGI(TAG, "Event ID fast-forwarded: %lu -> %lu (jumped %lu IDs)", 
 oldId, newId, newId - oldId);
 
 
 ESP_LOGI(TAG, "Persisting synced Event ID to NVS...");
 
 bool flushOk = false;
 if (_storageWorkerRunning) {
 flushOk = queueStorageCommand(StorageCmd::FORCE_FLUSH, newId);
 if (flushOk) {
 ESP_LOGI(TAG, "Event ID sync queued for async NVS persist");
 }
 }
 
 if (!flushOk) {
 flushOk = flushToNvs;
 if (flushOk) {
 ESP_LOGI(TAG, "Event ID sync persisted to NVS successfully (sync)");
 } else {
 ESP_LOGW(TAG, "NVS flush failed - Event ID sync only in RTC memory");
 }
 }
 } else {
 ESP_LOGD(TAG, "fastForwardEventId: No action needed (current >= new)");
 }
}
