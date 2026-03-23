
#ifndef INFERENCE_SERVICE_H
#define INFERENCE_SERVICE_H

#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "../Config.h"
#include "../Domain/SampleWindow.h"

class DataLoggerService;
class PersonalizationService;

class InferenceService {
public:
 InferenceService;
 ~InferenceService;

 bool init(DataLoggerService* dataLogger,
 PersonalizationService* personalization = nullptr);

 void shutdown;


 bool getLatestResult(InferenceResult* outResult) const;
 InferenceResult peekResult const { return _lastResult; }
 bool isRunning const { return _taskRunning.load(std::memory_order_relaxed); }
 TaskHandle_t getTaskHandle const { return _inferenceTaskHandle; }


 uint32_t getInferenceCount const { return _inferenceCount; }
 uint32_t getAverageLatencyUs const;
 void printStatus const;


 static constexpr uint8_t NUM_FEATURES = 10;

 static void extractFeatures(const SampleRecord* samples, uint8_t count, float* features);

 static void extractFeaturesNormalized(
 const SampleRecord* samples, uint8_t count, float* features,
 float dynamicRange);

 void predict(const float* features, GestureLabel& outLabel, uint8_t& outConf) const;

private:
 std::atomic<bool> _taskRunning;
 bool _initialized;
 DataLoggerService* _dataLogger;
 PersonalizationService* _personalization;

 QueueHandle_t _resultQueue;
 TaskHandle_t _inferenceTaskHandle;

 InferenceResult _lastResult;

 uint32_t _inferenceCount;
 uint32_t _totalLatencyUs;

 static constexpr bool WEIGHTS_TRAINED = false;
 static constexpr uint8_t NUM_CLASSES = 9;

 static const float _weights[NUM_CLASSES][NUM_FEATURES];
 static const float _biases[NUM_CLASSES];
 static const GestureLabel _classLabels[NUM_CLASSES];

 static void inferenceTask(void* pvParameters);
 void runInference;

 void predictHybrid(const float* features, InferenceResult& result) const;

 static void softmax(float* logits, uint8_t count);
};

#endif
