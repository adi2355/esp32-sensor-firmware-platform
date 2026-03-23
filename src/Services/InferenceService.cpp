
#include "InferenceService.h"
#include "DataLoggerService.h"
#include "PersonalizationService.h"
#include "../HAL/HAL_Base.h"
#include <math.h>

static const char* TAG = "ML_INFER";


const float InferenceService::_weights[NUM_CLASSES][NUM_FEATURES] = {

 { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }
};

const float InferenceService::_biases[NUM_CLASSES] = {

 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const GestureLabel InferenceService::_classLabels[NUM_CLASSES] = {
 GestureLabel::IDLE,
 GestureLabel::APPROACH,
 GestureLabel::HOVER,
 GestureLabel::QUICK_PASS,
 GestureLabel::COVER_HOLD,
 GestureLabel::CONFIRMED_EVENT,
 GestureLabel::SURFACE_REFLECTION,
 GestureLabel::NOISE,
 GestureLabel::UNKNOWN
};

InferenceService::InferenceService
 : _taskRunning(false)
 , _initialized(false)
 , _dataLogger(nullptr)
 , _personalization(nullptr)
 , _resultQueue(nullptr)
 , _inferenceTaskHandle(nullptr)
 , _inferenceCount(0)
 , _totalLatencyUs(0)
{
 memset(&_lastResult, 0, sizeof(_lastResult));
}

InferenceService::~InferenceService {
 shutdown;
 if (_resultQueue) {
 vQueueDelete(_resultQueue);
 _resultQueue = nullptr;
 }
}

bool InferenceService::init(DataLoggerService* dataLogger,
 PersonalizationService* personalization) {
 if (_initialized) return true;
 if (!dataLogger) {
 ESP_LOGE(TAG, "DataLoggerService pointer is null");
 return false;
 }

 _dataLogger = dataLogger;
 _personalization = personalization;

 _resultQueue = xQueueCreate(1, sizeof(InferenceResult));
 if (!_resultQueue) {
 ESP_LOGE(TAG, "Failed to create result queue");
 return false;
 }

 _taskRunning.store(true, std::memory_order_release);

 BaseType_t result = xTaskCreatePinnedToCore(
 inferenceTask,
 "InferTask",
 MLConfig::INFERENCE_TASK_STACK_SIZE,
 this,
 MLConfig::INFERENCE_TASK_PRIORITY,
 &_inferenceTaskHandle,
 FreeRTOSConfig::APP_CORE
 );

 if (result != pdPASS) {
 _taskRunning.store(false, std::memory_order_release);
 ESP_LOGE(TAG, "Failed to create InferenceTask! result=%d", result);
 return false;
 }

 _initialized = true;

 ESP_LOGI(TAG, "InferenceService initialized: %u features, %u classes, personalization=%s",
 NUM_FEATURES, NUM_CLASSES,
 _personalization ? "AVAILABLE" : "NONE");
 return true;
}

void InferenceService::shutdown {
 if (!_initialized) return;

 _taskRunning.store(false, std::memory_order_release);

 if (_inferenceTaskHandle) {
 for (int i = 0; i < 15; i++) {
 vTaskDelay(pdMS_TO_TICKS(100));
 if (eTaskGetState(_inferenceTaskHandle) == eDeleted) break;
 }
 _inferenceTaskHandle = nullptr;
 }

 _initialized = false;
 ESP_LOGI(TAG, "InferenceService shutdown");
}

bool InferenceService::getLatestResult(InferenceResult* outResult) const {
 if (!outResult || !_resultQueue) return false;
 return xQueueReceive(_resultQueue, outResult, 0) == pdTRUE;
}

uint32_t InferenceService::getAverageLatencyUs const {
 if (_inferenceCount == 0) return 0;
 return _totalLatencyUs / _inferenceCount;
}

void InferenceService::printStatus const {
#ifndef DIST_BUILD
 Serial.printf("\n=== ML Inference Status ===\n");
 Serial.printf(" Running: %s\n", _taskRunning ? "yes" : "no");
 Serial.printf(" Model trained: %s\n", WEIGHTS_TRAINED ? "YES" : "NO (placeholder)");
 Serial.printf(" Personalization: %s\n",
 _personalization && _personalization->isCalibrated ? "CALIBRATED" : "NONE");
 Serial.printf(" Inferences: %u\n", _inferenceCount);
 Serial.printf(" Avg latency: %u us\n", getAverageLatencyUs);
 Serial.printf(" Last prediction: %s (conf=%u%%, global=%u%%, local=%u%%)\n",
 gestureLabelToString(_lastResult.getLabel),
 _lastResult.confidence,
 _lastResult.globalConfidence,
 _lastResult.localConfidence);
 Serial.printf(" Adaptation: %u%%\n", _lastResult.adaptationQuality);
 Serial.printf(" Flags: 0x%02X\n", _lastResult.flags);
 Serial.printf(" Features: %u\n", NUM_FEATURES);
 Serial.printf(" Classes: %u\n", NUM_CLASSES);
 if (!WEIGHTS_TRAINED) {
 Serial.printf(" WARNING: Predictions are RANDOM (untrained weights)\n");
 Serial.printf(" Run tools/train_model.py to generate real weights.\n");
 }
 Serial.printf("===========================\n\n");
#endif
}

void InferenceService::inferenceTask(void* pvParameters) {
 InferenceService* self = static_cast<InferenceService*>(pvParameters);
 ESP_LOGI(TAG, "InferenceTask started on core %d", xPortGetCoreID);

 if (self->_dataLogger) {
 self->_dataLogger->setInferenceTaskHandle(xTaskGetCurrentTaskHandle);
 }

 while (self->_taskRunning.load(std::memory_order_acquire)) {
 uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

 if (notified > 0 && self->_dataLogger) {
 self->runInference;
 }
 }

 ESP_LOGI(TAG, "InferenceTask exiting");
 vTaskDelete(nullptr);
}

void InferenceService::runInference {
 SampleWindow window;
 if (!_dataLogger->getCurrentWindow(&window)) {
 return;
 }

 unsigned long startUs = micros;

 float features[NUM_FEATURES];

 bool useNormalized = false;
 float dynRange = 0.0f;

 if (_personalization) {
 float mean, sigma, range;
 if (_personalization->getBaselineParams(mean, sigma, range) && range > 0.0f) {
 dynRange = range;
 useNormalized = true;
 }
 }

 if (useNormalized) {
 extractFeaturesNormalized(window.samples, window.header.sampleCount,
 features, dynRange);
 } else {
 extractFeatures(window.samples, window.header.sampleCount, features);
 }

 InferenceResult result;
 memset(&result, 0, sizeof(result));
 predictHybrid(features, result);

 unsigned long endUs = micros;
 uint32_t latencyUs = (endUs >= startUs) ? (endUs - startUs) : 0;

 result.latencyUs = (latencyUs > 65535) ? 65535 : static_cast<uint16_t>(latencyUs);
 result.timestampMs = HAL::getSystemUptime;

 xQueueOverwrite(_resultQueue, &result);
 _lastResult = result;
 _inferenceCount++;
 _totalLatencyUs += latencyUs;

 if (_inferenceCount % 10 == 0) {
 ESP_LOGI(TAG, "Inference #%u: %s (conf=%u%%, g=%u%% l=%u%%, %uus)",
 _inferenceCount,
 gestureLabelToString(result.getLabel),
 result.confidence,
 result.globalConfidence,
 result.localConfidence,
 latencyUs);
 }
}

void InferenceService::extractFeatures(
 const SampleRecord* samples,
 uint8_t count,
 float* features
) {
 extractFeaturesNormalized(samples, count, features,
 PersonalizationConfig::DEFAULT_PROX_NORM);
}


void InferenceService::extractFeaturesNormalized(
 const SampleRecord* samples,
 uint8_t count,
 float* features,
 float dynamicRange
) {
 if (!samples || count == 0 || !features) {
 for (int i = 0; i < NUM_FEATURES; i++) features[i] = 0.0f;
 return;
 }

 for (int i = 0; i < NUM_FEATURES; i++) features[i] = 0.0f;
}


void InferenceService::predict(
 const float* features,
 GestureLabel& outLabel,
 uint8_t& outConf
) const {


 outLabel = GestureLabel::UNKNOWN;
 outConf = 0;
}


void InferenceService::predictHybrid(
 const float* features,
 InferenceResult& result
) const {


 memset(&result, 0, sizeof(result));
 result.predictedLabel = static_cast<uint8_t>(GestureLabel::UNKNOWN);
 result.confidence = 0;
 result.flags = InferenceResult::FLAG_BELOW_THRESHOLD;
}

void InferenceService::softmax(float* logits, uint8_t count) {
 float maxLogit = logits[0];
 for (uint8_t i = 1; i < count; i++) {
 if (logits[i] > maxLogit) maxLogit = logits[i];
 }

 float sumExp = 0.0f;
 for (uint8_t i = 0; i < count; i++) {
 logits[i] = expf(logits[i] - maxLogit);
 sumExp += logits[i];
 }

 if (sumExp > 0.0f) {
 for (uint8_t i = 0; i < count; i++) {
 logits[i] /= sumExp;
 }
 }
}
