
#include "PersonalizationService.h"
#include <Preferences.h>
#include <math.h>

static uint32_t calcCRC32(const uint8_t* data, size_t len) {
 uint32_t crc = 0xFFFFFFFF;
 for (size_t i = 0; i < len; i++) {
 crc ^= data[i];
 for (int j = 0; j < 8; j++) {
 crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
 }
 }
 return ~crc;
}

PersonalizationService::PersonalizationService
 : _initialized(false)
 , _dirty(false)
 , _calibrating(false)
 , _mutex(nullptr)
 , _workerTask(nullptr)
 , _workerRunning(false)
 , _commitTimer(nullptr)
{
 resetDataToDefaults;
 publishSnapshot;
 memset(&_captureAccum, 0, sizeof(_captureAccum));
 _captureAccum.label = GestureLabel::INVALID;
}

PersonalizationService::~PersonalizationService {
 shutdown;
 if (_mutex) {
 vSemaphoreDelete(_mutex);
 _mutex = nullptr;
 }
 if (_commitTimer) {
 xTimerDelete(_commitTimer, portMAX_DELAY);
 _commitTimer = nullptr;
 }
}

void PersonalizationService::resetDataToDefaults {
 memset(&_data, 0, sizeof(MLCalibrationData));
 _data.version = PersonalizationConfig::CALIBRATION_VERSION;
}

void PersonalizationService::publishSnapshot {
 memcpy(&_snapshot, &_data, sizeof(MLCalibrationData));
}

bool PersonalizationService::init {
 if (_initialized) return true;

 ESP_LOGI(TAG, "Initializing PersonalizationService...");

 _mutex = xSemaphoreCreateMutex;
 if (!_mutex) {
 ESP_LOGE(TAG, "Failed to create mutex");
 return false;
 }

 _commitTimer = xTimerCreate(
 "MLPersonTimer",
 pdMS_TO_TICKS(PersonalizationConfig::COMMIT_DELAY_MS),
 pdFALSE,
 this,
 commitTimerCallback
 );
 if (!_commitTimer) {
 ESP_LOGE(TAG, "Failed to create commit timer");
 return false;
 }

 if (readFromNvs) {
 publishSnapshot;
 ESP_LOGI(TAG, "Loaded calibration: %u classes calibrated, baseline=%.1f+/-%.1f",
 _data.numCalibratedClasses, _data.baselineMean, _data.baselineSigma);
 } else {
 ESP_LOGI(TAG, "No existing calibration found (using defaults)");
 }

 _workerRunning.store(true, std::memory_order_release);

 BaseType_t result = xTaskCreatePinnedToCore(
 workerTask,
 "MLPersonW",
 PersonalizationConfig::WORKER_TASK_STACK_SIZE,
 this,
 PersonalizationConfig::WORKER_TASK_PRIORITY,
 &_workerTask,
 FreeRTOSConfig::APP_CORE
 );
 if (result != pdPASS) {
 _workerRunning.store(false, std::memory_order_release);
 ESP_LOGE(TAG, "Failed to create worker task (result=%d)", result);
 return false;
 }

 _initialized = true;
 ESP_LOGI(TAG, "PersonalizationService initialized");
 return true;
}

void PersonalizationService::shutdown {
 if (!_initialized) return;

 ESP_LOGI(TAG, "Shutting down PersonalizationService...");

 if (_dirty) {
 forceFlush;
 }

 _workerRunning.store(false, std::memory_order_release);
 if (_workerTask) {
 xTaskNotify(_workerTask, NOTIFY_SHUTDOWN, eSetBits);
 for (int i = 0; i < 15; i++) {
 vTaskDelay(pdMS_TO_TICKS(100));
 if (eTaskGetState(_workerTask) == eDeleted) break;
 }
 _workerTask = nullptr;
 }

 _initialized = false;
 ESP_LOGI(TAG, "PersonalizationService shutdown complete");
}

bool PersonalizationService::readFromNvs {
 Preferences prefs;
 if (!prefs.begin(PersonalizationConfig::NVS_NAMESPACE, true)) {
 return false;
 }

 size_t len = prefs.getBytesLength(PersonalizationConfig::NVS_KEY_DATA);
 if (len != sizeof(MLCalibrationData)) {
 prefs.end;
 return false;
 }

 MLCalibrationData temp;
 size_t read = prefs.getBytes(PersonalizationConfig::NVS_KEY_DATA,
 &temp, sizeof(MLCalibrationData));
 prefs.end;

 if (read != sizeof(MLCalibrationData)) {
 return false;
 }

 if (temp.version != PersonalizationConfig::CALIBRATION_VERSION) {
 ESP_LOGW(TAG, "NVS version mismatch: got %u, expected %u",
 temp.version, PersonalizationConfig::CALIBRATION_VERSION);
 return false;
 }

 uint32_t expectedCrc = calcCRC32(
 reinterpret_cast<const uint8_t*>(&temp),
 sizeof(MLCalibrationData) - sizeof(uint32_t)
 );
 if (temp.crc32 != expectedCrc) {
 ESP_LOGW(TAG, "NVS CRC mismatch (0x%08X vs 0x%08X)", temp.crc32, expectedCrc);
 return false;
 }

 memcpy(&_data, &temp, sizeof(MLCalibrationData));
 return true;
}

bool PersonalizationService::writeToNvs {
 _data.crc32 = calculateCrc;

 Preferences prefs;
 if (!prefs.begin(PersonalizationConfig::NVS_NAMESPACE, false)) {
 ESP_LOGE(TAG, "Failed to open NVS namespace for write");
 return false;
 }

 size_t written = prefs.putBytes(PersonalizationConfig::NVS_KEY_DATA,
 &_data, sizeof(MLCalibrationData));
 prefs.end;

 if (written != sizeof(MLCalibrationData)) {
 ESP_LOGE(TAG, "NVS write failed: %u/%u bytes", written, sizeof(MLCalibrationData));
 return false;
 }

 _dirty = false;
 ESP_LOGI(TAG, "Calibration saved to NVS (%u bytes, CRC=0x%08X)",
 sizeof(MLCalibrationData), _data.crc32);
 return true;
}

uint32_t PersonalizationService::calculateCrc const {
 return calcCRC32(
 reinterpret_cast<const uint8_t*>(&_data),
 sizeof(MLCalibrationData) - sizeof(uint32_t)
 );
}

void PersonalizationService::workerTask(void* pvParameters) {
 PersonalizationService* self = static_cast<PersonalizationService*>(pvParameters);
 ESP_LOGI(self->TAG, "PersonalizationWorker started on core %d", xPortGetCoreID);

 while (self->_workerRunning.load(std::memory_order_acquire)) {
 uint32_t notification = 0;
 if (xTaskNotifyWait(0, UINT32_MAX, &notification, pdMS_TO_TICKS(1000)) == pdTRUE) {
 if (notification & NOTIFY_SAVE) {
 if (self->_mutex && xSemaphoreTake(self->_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
 self->writeToNvs;
 xSemaphoreGive(self->_mutex);
 }
 }
 if (notification & NOTIFY_SHUTDOWN) {
 break;
 }
 }
 }

 ESP_LOGI(self->TAG, "PersonalizationWorker exiting");
 vTaskDelete(nullptr);
}

void PersonalizationService::commitTimerCallback(TimerHandle_t xTimer) {
 PersonalizationService* self = static_cast<PersonalizationService*>(pvTimerGetTimerID(xTimer));
 if (self && self->_workerTask) {
 xTaskNotify(self->_workerTask, NOTIFY_SAVE, eSetBits);
 }
}

bool PersonalizationService::startCalibration {
 if (!_initialized) return false;

 memset(&_captureAccum, 0, sizeof(CaptureAccum));
 _captureAccum.label = GestureLabel::INVALID;
 _calibrating = true;

 ESP_LOGI(TAG, "Calibration capture mode started");
 return true;
}

bool PersonalizationService::captureWindow(const float* features, GestureLabel label) {
 if (!_calibrating || !features) return false;
 if (label == GestureLabel::INVALID || label == GestureLabel::UNLABELED) return false;

 uint8_t classIdx = static_cast<uint8_t>(label);
 if (classIdx >= PersonalizationConfig::NUM_CLASSES) return false;

 if (_captureAccum.label != label) {
 memset(_captureAccum.featureSum, 0, sizeof(_captureAccum.featureSum));
 _captureAccum.count = 0;
 _captureAccum.label = label;
 }

 if (_captureAccum.count >= PersonalizationConfig::MAX_CALIBRATION_WINDOWS) {
 ESP_LOGW(TAG, "Max calibration windows reached for class %u", classIdx);
 return false;
 }

 for (uint8_t f = 0; f < PersonalizationConfig::NUM_FEATURES; f++) {
 _captureAccum.featureSum[f] += static_cast<double>(features[f]);
 }
 _captureAccum.count++;

 ESP_LOGI(TAG, "Captured window %u for class %s",
 _captureAccum.count, gestureLabelToString(label));
 return true;
}

bool PersonalizationService::commitCalibration {
 if (!_initialized || !_calibrating) return false;

 if (_captureAccum.count < PersonalizationConfig::MIN_CALIBRATION_WINDOWS) {
 ESP_LOGW(TAG, "Not enough windows: %u < %u minimum",
 _captureAccum.count, PersonalizationConfig::MIN_CALIBRATION_WINDOWS);
 return false;
 }

 uint8_t classIdx = static_cast<uint8_t>(_captureAccum.label);
 if (classIdx >= PersonalizationConfig::NUM_CLASSES) return false;

 if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
 ESP_LOGE(TAG, "Failed to acquire mutex for commit - data NOT lost, retry");
 return false;
 }

 for (uint8_t f = 0; f < PersonalizationConfig::NUM_FEATURES; f++) {
 _data.prototypes[classIdx][f] =
 static_cast<float>(_captureAccum.featureSum[f] / _captureAccum.count);
 }
 _data.sampleCounts[classIdx] = (_captureAccum.count > 255) ? 255 : _captureAccum.count;

 uint8_t calibCount = 0;
 for (uint8_t c = 0; c < PersonalizationConfig::NUM_CLASSES; c++) {
 if (_data.sampleCounts[c] > 0) calibCount++;
 }
 _data.numCalibratedClasses = calibCount;

 _dirty = true;
 publishSnapshot;
 xSemaphoreGive(_mutex);

 if (_commitTimer) {
 xTimerReset(_commitTimer, 0);
 }

 ESP_LOGI(TAG, "Committed prototype for class %s (%u windows, %u total classes)",
 gestureLabelToString(_captureAccum.label),
 _captureAccum.count, calibCount);

 memset(&_captureAccum, 0, sizeof(CaptureAccum));
 _captureAccum.label = GestureLabel::INVALID;

 return true;
}

bool PersonalizationService::resetCalibration {
 if (!_initialized) return false;

 if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
 ESP_LOGE(TAG, "Failed to acquire mutex for reset");
 return false;
 }

 resetDataToDefaults;
 _dirty = true;
 _calibrating = false;
 publishSnapshot;
 xSemaphoreGive(_mutex);

 forceFlush;

 ESP_LOGI(TAG, "Calibration reset to defaults");
 return true;
}

bool PersonalizationService::isCalibrated const {
 return _snapshot.numCalibratedClasses > 0;
}

uint8_t PersonalizationService::getAdaptationQuality const {
 if (_snapshot.numCalibratedClasses == 0) return 0;

 uint16_t totalSamples = 0;
 for (uint8_t c = 0; c < PersonalizationConfig::NUM_CLASSES; c++) {
 totalSamples += _snapshot.sampleCounts[c];
 }

 uint16_t maxTotal = PersonalizationConfig::NUM_CLASSES * PersonalizationConfig::MAX_CALIBRATION_WINDOWS;
 uint16_t quality = (totalSamples * 100) / maxTotal;
 return (quality > 100) ? 100 : static_cast<uint8_t>(quality);
}

bool PersonalizationService::getBaselineParams(float& outMean, float& outSigma, float& outDynamicRange) const {
 outMean = _snapshot.baselineMean;
 outSigma = _snapshot.baselineSigma;
 outDynamicRange = _snapshot.dynamicRange;
 return (_snapshot.dynamicRange > 0.0f);
}

bool PersonalizationService::getPrototype(uint8_t classIdx, float* outFeatures) const {
 if (classIdx >= PersonalizationConfig::NUM_CLASSES || !outFeatures) return false;
 if (_snapshot.sampleCounts[classIdx] == 0) return false;

 memcpy(outFeatures, _snapshot.prototypes[classIdx],
 PersonalizationConfig::NUM_FEATURES * sizeof(float));
 return true;
}

float PersonalizationService::getBiasOffset(uint8_t classIdx) const {
 if (classIdx >= PersonalizationConfig::NUM_CLASSES) return 0.0f;
 return _snapshot.biasOffsets[classIdx];
}

void PersonalizationService::updateSensorBaseline(float mean, float sigma, float dynamicRange) {
 if (!_initialized) return;

 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 _data.baselineMean = mean;
 _data.baselineSigma = sigma;
 _data.dynamicRange = dynamicRange;
 _dirty = true;
 publishSnapshot;
 xSemaphoreGive(_mutex);

 if (_commitTimer) {
 xTimerReset(_commitTimer, 0);
 }

 ESP_LOGI(TAG, "Sensor baseline updated: mean=%.1f, sigma=%.1f, range=%.1f",
 mean, sigma, dynamicRange);
 }
}

bool PersonalizationService::forceFlush {
 if (!_initialized || !_dirty) return true;

 if (_commitTimer) {
 xTimerStop(_commitTimer, 0);
 }

 bool ok = false;
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
 ok = writeToNvs;
 xSemaphoreGive(_mutex);
 }
 return ok;
}

void PersonalizationService::printStatus const {
#ifndef DIST_BUILD
 Serial.printf("\n=== ML Personalization Status ===\n");
 Serial.printf(" Initialized: %s\n", _initialized ? "yes" : "no");
 Serial.printf(" Calibrating: %s\n", _calibrating ? "yes" : "no");
 Serial.printf(" Calibrated: %s (%u classes)\n",
 isCalibrated ? "yes" : "no", _snapshot.numCalibratedClasses);
 Serial.printf(" Adaptation: %u%%\n", getAdaptationQuality);
 Serial.printf(" Baseline: mean=%.2f, sigma=%.2f, range=%.2f\n",
 _snapshot.baselineMean, _snapshot.baselineSigma, _snapshot.dynamicRange);
 Serial.printf(" Dirty: %s\n", _dirty ? "yes" : "no");
 Serial.printf(" Version: %u\n", _snapshot.version);

 Serial.printf(" Classes:\n");
 for (uint8_t c = 0; c < PersonalizationConfig::NUM_CLASSES; c++) {
 if (_snapshot.sampleCounts[c] > 0) {
 Serial.printf(" [%u] %s: %u samples, bias=%.3f\n",
 c, gestureLabelToString(static_cast<GestureLabel>(c)),
 _snapshot.sampleCounts[c], _snapshot.biasOffsets[c]);
 }
 }

 if (_calibrating && _captureAccum.count > 0) {
 Serial.printf(" Capture buffer: %u windows for %s\n",
 _captureAccum.count,
 gestureLabelToString(_captureAccum.label));
 }
 Serial.printf("=================================\n\n");
#endif
}
