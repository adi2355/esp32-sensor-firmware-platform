
#include "DataLoggerService.h"
#include "../HAL/HAL_Base.h"
#include "../HAL/Sensor_HAL.h"
#include "ConfigService.h"
#include "BLEManager.h"
#include "SPIFFS.h"
#include "FS.h"

static const char* TAG = "ML_LOGGER";

uint32_t DataLoggerService::calcCRC32(const uint8_t* data, size_t len) {
 uint32_t crc = 0xFFFFFFFF;
 for (size_t i = 0; i < len; i++) {
 crc ^= data[i];
 for (int j = 0; j < 8; j++) {
 crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
 }
 }
 return ~crc;
}

uint32_t SampleWindow::calculateCRC const {
 const size_t headerBytesBeforeCRC = sizeof(WindowHeader) - sizeof(uint32_t);
 const size_t samplesBytes = sizeof(SampleRecord) * MLConfig::WINDOW_SIZE;

 uint32_t crc = 0xFFFFFFFF;

 const uint8_t* p = reinterpret_cast<const uint8_t*>(&header);
 for (size_t i = 0; i < headerBytesBeforeCRC; i++) {
 crc ^= p[i];
 for (int j = 0; j < 8; j++) {
 crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
 }
 }

 p = reinterpret_cast<const uint8_t*>(samples);
 for (size_t i = 0; i < samplesBytes; i++) {
 crc ^= p[i];
 for (int j = 0; j < 8; j++) {
 crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
 }
 }

 return ~crc;
}

bool SampleWindow::isValid const {
 if (header.magic != MLConfig::WINDOW_MAGIC) return false;
 if (header.version != MLConfig::WINDOW_VERSION) return false;
 if (header.sampleCount > MLConfig::WINDOW_SIZE) return false;
 if (header.crc32 != calculateCRC) return false;
 return true;
}

DataLoggerService::DataLoggerService
 : _state(LoggerState::IDLE)
 , _initialized(false)
 , _loggerTaskRunning(false)
 , _currentSessionId(0)
 , _currentWindowId(0)
 , _windowsWritten(0)
 , _currentLabel(GestureLabel::UNLABELED)
 , _dropCount(0)
 , _totalWindowsWritten(0)
 , _sampleIndex(0)
 , _windowStartUptime(0)
 , _inferenceMutex(nullptr)
 , _loggerQueue(nullptr)
 , _loggerTaskHandle(nullptr)
 , _inferenceTaskHandle(nullptr)
 , _cachedBootCount(0)
 , _cachedFlags(0)
 , _spiffsMounted(false)
 , _nextSessionId(1)
{
 memset(_sampleBuf, 0, sizeof(_sampleBuf));
 memset(&_inferenceWindow, 0, sizeof(_inferenceWindow));
}

DataLoggerService::~DataLoggerService {
 shutdown;
 if (_inferenceMutex) {
 vSemaphoreDelete(_inferenceMutex);
 _inferenceMutex = nullptr;
 }
 if (_loggerQueue) {
 vQueueDelete(_loggerQueue);
 _loggerQueue = nullptr;
 }
}

bool DataLoggerService::init {
 if (_initialized.load(std::memory_order_acquire)) {
 ESP_LOGW(TAG, "Already initialized");
 return true;
 }

 ESP_LOGI(TAG, "Initializing ML DataLoggerService...");

 _inferenceMutex = xSemaphoreCreateMutex;
 if (!_inferenceMutex) {
 ESP_LOGE(TAG, "Failed to create inference mutex");
 return false;
 }

 _loggerQueue = xQueueCreate(
 MLConfig::LOGGER_QUEUE_DEPTH,
 sizeof(SampleWindow)
 );
 if (!_loggerQueue) {
 ESP_LOGE(TAG, "Failed to create logger queue");
 return false;
 }

 if (!mountSpiffs) {
 ESP_LOGE(TAG, "SPIFFS mount failed - DataLogger disabled");
 }

 if (_spiffsMounted) {
 _nextSessionId = rebuildNextSessionId;
 ESP_LOGI(TAG, "Next session ID: %u", _nextSessionId);
 }

 _loggerTaskRunning.store(true, std::memory_order_release);

 BaseType_t result = xTaskCreatePinnedToCore(
 loggerTask,
 "LoggerTask",
 MLConfig::LOGGER_TASK_STACK_SIZE,
 this,
 MLConfig::LOGGER_TASK_PRIORITY,
 &_loggerTaskHandle,
 FreeRTOSConfig::APP_CORE
 );

 if (result != pdPASS) {
 _loggerTaskRunning.store(false, std::memory_order_release);
 ESP_LOGE(TAG, "Failed to create LoggerTask! result=%d", result);
 return false;
 }

 _initialized.store(true, std::memory_order_release);

 ESP_LOGI(TAG, "DataLoggerService initialized: queue=%d, task=LoggerTask, core=%d",
 MLConfig::LOGGER_QUEUE_DEPTH, FreeRTOSConfig::APP_CORE);

 return true;
}

void DataLoggerService::shutdown {
 if (!_initialized.load(std::memory_order_acquire)) return;

 ESP_LOGI(TAG, "Shutting down DataLoggerService...");

 LoggerState current = _state.load(std::memory_order_acquire);
 if (current == LoggerState::RECORDING || current == LoggerState::ARMED) {
 stopSession;
 }

 _loggerTaskRunning.store(false, std::memory_order_release);

 if (_loggerTaskHandle) {
 for (int i = 0; i < 15; i++) {
 vTaskDelay(pdMS_TO_TICKS(100));
 if (eTaskGetState(_loggerTaskHandle) == eDeleted) break;
 }
 _loggerTaskHandle = nullptr;
 }

 unmountSpiffs;

 _initialized.store(false, std::memory_order_release);
 ESP_LOGI(TAG, "DataLoggerService shutdown complete");
}

void DataLoggerService::appendSample(uint16_t proximity, uint16_t dt_ms) {
 if (_sampleIndex == 0) {
 _windowStartUptime = HAL::getSystemUptime;
 }

 _sampleBuf[_sampleIndex].proximity = proximity;
 _sampleBuf[_sampleIndex].dt_ms = dt_ms;
 _sampleIndex++;

 if (_sampleIndex >= MLConfig::WINDOW_SIZE) {
 if (_inferenceMutex && xSemaphoreTake(_inferenceMutex, 0) == pdTRUE) {
 memcpy(_inferenceWindow.samples, _sampleBuf, sizeof(_sampleBuf));
 fillWindowHeader(_inferenceWindow.header);
 _inferenceWindow.header.label = static_cast<uint8_t>(GestureLabel::UNLABELED);
 _inferenceWindow.header.crc32 = _inferenceWindow.calculateCRC;
 xSemaphoreGive(_inferenceMutex);

 if (_inferenceTaskHandle) {
 xTaskNotifyGive(_inferenceTaskHandle);
 }
 }

 LoggerState current = _state.load(std::memory_order_acquire);
 if (current == LoggerState::RECORDING) {
 SampleWindow writeWindow;
 memcpy(writeWindow.samples, _sampleBuf, sizeof(_sampleBuf));
 fillWindowHeader(writeWindow.header);
 writeWindow.header.label = static_cast<uint8_t>(_currentLabel);
 writeWindow.header.sessionId = _currentSessionId;
 writeWindow.header.windowId = _currentWindowId;
 writeWindow.header.crc32 = writeWindow.calculateCRC;

 if (xQueueSend(_loggerQueue, &writeWindow, 0) == pdTRUE) {
 _currentWindowId++;
 } else {
 _dropCount.fetch_add(1, std::memory_order_relaxed);
 ESP_LOGW(TAG, "Logger queue full - window dropped (total drops: %u)",
 _dropCount.load(std::memory_order_relaxed));
 }
 }

 if (MLConfig::WINDOW_STRIDE < MLConfig::WINDOW_SIZE) {
 const uint8_t keepSamples = MLConfig::WINDOW_SIZE - MLConfig::WINDOW_STRIDE;
 memmove(_sampleBuf, &_sampleBuf[MLConfig::WINDOW_STRIDE],
 keepSamples * sizeof(SampleRecord));
 _sampleIndex = keepSamples;

 _windowStartUptime += MLConfig::WINDOW_STRIDE * Timing::SAMPLING_PERIOD_MS;
 } else {
 _sampleIndex = 0;
 }
 }
}

bool DataLoggerService::startSession(GestureLabel label) {
 LoggerState current = _state.load(std::memory_order_acquire);
 if (current == LoggerState::RECORDING || current == LoggerState::ARMED) {
 if (current == LoggerState::RECORDING && _currentLabel == label) {
 ESP_LOGI(TAG, "Session %u already recording with label=%s",
 _currentSessionId, gestureLabelToString(label));
 return true;
 }
 ESP_LOGW(TAG, "Cannot start session - already %s",
 current == LoggerState::RECORDING ? "recording" : "armed");
 return false;
 }

 if (!_spiffsMounted) {
 ESP_LOGE(TAG, "Cannot start session - SPIFFS not mounted");
 _state.store(LoggerState::ERROR, std::memory_order_release);
 return false;
 }

 if (label == GestureLabel::INVALID) {
 ESP_LOGE(TAG, "Cannot start session - invalid label");
 return false;
 }

 _currentSessionId = _nextSessionId++;
 _currentWindowId = 0;
 _windowsWritten = 0;
 _currentLabel = label;
 _sampleIndex = 0;

 appendManifest(_currentSessionId, label);

 _state.store(LoggerState::RECORDING, std::memory_order_release);

 ESP_LOGI(TAG, "Session %u started: label=%s",
 _currentSessionId, gestureLabelToString(label));
 return true;
}

bool DataLoggerService::stopSession {
 LoggerState current = _state.load(std::memory_order_acquire);
 if (current != LoggerState::RECORDING && current != LoggerState::ARMED) {
 ESP_LOGI(TAG, "No active session to stop (already %s)", getStateName);
 return true;
 }

 _state.store(LoggerState::IDLE, std::memory_order_release);

 ESP_LOGI(TAG, "Session %u stopped: %u windows written, %u drops",
 _currentSessionId, _windowsWritten,
 _dropCount.load(std::memory_order_relaxed));

 return true;
}

bool DataLoggerService::deleteAll {
 LoggerState current = _state.load(std::memory_order_acquire);
 if (current == LoggerState::RECORDING || current == LoggerState::ARMED) {
 stopSession;
 }

 if (!_spiffsMounted) {
 ESP_LOGE(TAG, "Cannot delete - SPIFFS not mounted");
 return false;
 }

 ESP_LOGW(TAG, "Deleting all ML data from SPIFFS...");

 File root = SPIFFS.open("/");
 if (!root) {
 ESP_LOGE(TAG, "Failed to open SPIFFS root");
 return false;
 }

 uint16_t deleted = 0;
 File file = root.openNextFile;
 while (file) {
 const char* name = file.name;
 file.close;

 if (name && strncmp(name, "/ml_", 4) == 0) {
 if (SPIFFS.remove(name)) {
 deleted++;
 }
 }
 file = root.openNextFile;
 }
 root.close;

 _nextSessionId = 1;
 _totalWindowsWritten.store(0, std::memory_order_relaxed);
 _dropCount.store(0, std::memory_order_relaxed);

 ESP_LOGI(TAG, "Deleted %u ML files. Session counter reset.", deleted);
 return true;
}

const char* DataLoggerService::getStateName const {
 switch (_state.load(std::memory_order_relaxed)) {
 case LoggerState::IDLE: return "IDLE";
 case LoggerState::ARMED: return "ARMED";
 case LoggerState::RECORDING: return "RECORDING";
 case LoggerState::ERROR: return "ERROR";
 default: return "UNKNOWN";
 }
}

void DataLoggerService::printStatus const {
#ifndef DIST_BUILD
 size_t total = 0, used = 0;
 getSpiffsUsage(total, used);

 Serial.printf("\n=== ML DataLogger Status ===\n");
 Serial.printf(" State: %s\n", getStateName);
 Serial.printf(" Session ID: %u\n", _currentSessionId);
 Serial.printf(" Windows (session): %u\n", _windowsWritten);
 Serial.printf(" Windows (total): %u\n", _totalWindowsWritten.load(std::memory_order_relaxed));
 Serial.printf(" Drops (total): %u\n", _dropCount.load(std::memory_order_relaxed));
 Serial.printf(" SPIFFS: %u / %u bytes (%.1f%%)\n",
 used, total, total > 0 ? (100.0f * used / total) : 0.0f);
 Serial.printf(" Next session ID: %u\n", _nextSessionId);
 Serial.printf(" Window size: %u samples (%u bytes)\n",
 MLConfig::WINDOW_SIZE, (uint32_t)SampleWindow::serializedSize);
 Serial.printf(" Stride: %u samples\n", MLConfig::WINDOW_STRIDE);
 Serial.printf(" Sample rate: %u Hz\n", 1000 / Timing::SAMPLING_PERIOD_MS);
 Serial.printf("============================\n\n");
#endif
}

bool DataLoggerService::getSpiffsUsage(size_t& totalBytes, size_t& usedBytes) const {
 if (!_spiffsMounted) {
 totalBytes = 0;
 usedBytes = 0;
 return false;
 }
 totalBytes = SPIFFS.totalBytes;
 usedBytes = SPIFFS.usedBytes;
 return true;
}

bool DataLoggerService::getCurrentWindow(SampleWindow* outWindow) const {
 if (!outWindow) return false;

 if (_inferenceMutex && xSemaphoreTake(_inferenceMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
 memcpy(outWindow, &_inferenceWindow, sizeof(SampleWindow));
 xSemaphoreGive(_inferenceMutex);
 return true;
 }
 return false;
}

void DataLoggerService::loggerTask(void* pvParameters) {
 DataLoggerService* self = static_cast<DataLoggerService*>(pvParameters);
 ESP_LOGI(TAG, "LoggerTask started on core %d", xPortGetCoreID);

 SampleWindow window;

 while (self->_loggerTaskRunning.load(std::memory_order_acquire)) {
 if (xQueueReceive(self->_loggerQueue, &window, pdMS_TO_TICKS(500)) == pdTRUE) {
 self->processWrite(window);
 }
 }

 while (xQueueReceive(self->_loggerQueue, &window, 0) == pdTRUE) {
 self->processWrite(window);
 }

 ESP_LOGI(TAG, "LoggerTask exiting");
 vTaskDelete(nullptr);
}

void DataLoggerService::processWrite(const SampleWindow& window) {
 if (!_spiffsMounted) {
 ESP_LOGE(TAG, "Cannot write - SPIFFS not mounted");
 _state.store(LoggerState::ERROR, std::memory_order_release);
 return;
 }

 char path[32];
 buildSessionPath(window.header.sessionId, path, sizeof(path));

 File file = SPIFFS.open(path, FILE_APPEND);
 if (!file) {
 ESP_LOGE(TAG, "Failed to open %s for append", path);
 _state.store(LoggerState::ERROR, std::memory_order_release);
 return;
 }

 size_t written = file.write(
 reinterpret_cast<const uint8_t*>(&window),
 sizeof(SampleWindow)
 );
 file.close;

 if (written != sizeof(SampleWindow)) {
 ESP_LOGE(TAG, "Partial write to %s: %u/%u bytes",
 path, written, sizeof(SampleWindow));
 _state.store(LoggerState::ERROR, std::memory_order_release);
 return;
 }

 _windowsWritten++;
 _totalWindowsWritten.fetch_add(1, std::memory_order_relaxed);

 if (_windowsWritten % 10 == 0) {
 ESP_LOGI(TAG, "Session %u: %u windows written",
 _currentSessionId, _windowsWritten);
 }
}

bool DataLoggerService::mountSpiffs {
 if (_spiffsMounted) return true;

 if (!SPIFFS.begin(false, "/spiffs", 10)) {
 ESP_LOGE(TAG, "SPIFFS mount failed (will NOT auto-format to protect data)");
 ESP_LOGE(TAG, "To format manually: call deleteAll or use pio run --target uploadfs");
 return false;
 }

 _spiffsMounted = true;

 size_t total = SPIFFS.totalBytes;
 size_t used = SPIFFS.usedBytes;
 ESP_LOGI(TAG, "SPIFFS mounted: %u total, %u used (%.1f%%)",
 total, used, total > 0 ? (100.0f * used / total) : 0.0f);

 return true;
}

void DataLoggerService::unmountSpiffs {
 if (!_spiffsMounted) return;
 SPIFFS.end;
 _spiffsMounted = false;
 ESP_LOGI(TAG, "SPIFFS unmounted");
}

bool DataLoggerService::ensureDirectory {
 return _spiffsMounted;
}

uint16_t DataLoggerService::rebuildNextSessionId {
 if (!_spiffsMounted) return 1;

 uint16_t maxId = 0;

 File root = SPIFFS.open("/");
 if (!root) return 1;

 File file = root.openNextFile;
 while (file) {
 const char* name = file.name;
 file.close;

 if (name && strncmp(name, "/ml_s", 5) == 0) {
 uint16_t id = atoi(name + 5);
 if (id > maxId) maxId = id;
 }
 file = root.openNextFile;
 }
 root.close;

 return maxId + 1;
}

uint16_t DataLoggerService::loadNextSessionId {
 return rebuildNextSessionId;
}

bool DataLoggerService::appendManifest(uint16_t sessionId, GestureLabel label) {
 if (!_spiffsMounted) return false;

 File manifest = SPIFFS.open("/ml_manifest.txt", FILE_APPEND);
 if (!manifest) {
 ESP_LOGW(TAG, "Failed to open manifest for append (non-fatal)");
 return false;
 }

 manifest.printf("%u,%s\n", sessionId, gestureLabelToString(label));
 manifest.close;
 return true;
}

void DataLoggerService::buildSessionPath(uint16_t sessionId, char* outPath, size_t pathLen) const {
 snprintf(outPath, pathLen, "/ml_s%04u.bin", sessionId);
}

void DataLoggerService::fillWindowHeader(WindowHeader& header) const {
 header.magic = MLConfig::WINDOW_MAGIC;
 header.version = MLConfig::WINDOW_VERSION;
 header.sampleCount = MLConfig::WINDOW_SIZE;
 header.sessionId = _currentSessionId;
 header.windowId = _currentWindowId;

 header.startUptimeMs = _windowStartUptime;
 header.startEpoch = 0;

 uint32_t currentUptime = HAL::getSystemUptime;
 struct timeval tv;
 gettimeofday(&tv, nullptr);
 if (tv.tv_sec > 1577836800) {
 uint32_t currentEpoch = static_cast<uint32_t>(tv.tv_sec);
 if (currentUptime >= _windowStartUptime) {
 header.startEpoch = currentEpoch - (currentUptime - _windowStartUptime) / 1000;
 }
 }

 Sensor_HAL* sensor = Sensor_HAL::getInstance;
 if (sensor) {
 header.wakeThreshold = sensor->getWakeThreshold;
 header.detectionThreshold = sensor->getDetectionThreshold;
 header.criticalDetectionThreshold = sensor->getCriticalDetectionThreshold;
 } else {
 header.wakeThreshold = 0;
 header.detectionThreshold = 0;
 header.criticalDetectionThreshold = 0;
 }

 ConfigService* config = ConfigService::getInstance;
 if (config) {
 header.sensitivity = config->getSensitivity;
 } else {
 header.sensitivity = 50;
 }

 header.bootCount = _cachedBootCount;
 header.reserved0 = 0;
 header.reserved1 = 0;

 header.flags = _cachedFlags.load(std::memory_order_relaxed);

 header.crc32 = 0;
}
