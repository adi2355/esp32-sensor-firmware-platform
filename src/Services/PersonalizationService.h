
#ifndef PERSONALIZATION_SERVICE_H
#define PERSONALIZATION_SERVICE_H

#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "../Config.h"
#include "../Domain/SampleWindow.h"

#pragma pack(push, 1)

struct MLCalibrationData {
 uint8_t version;
 uint8_t numCalibratedClasses;
 uint8_t reserved[2];

 float baselineMean;
 float baselineSigma;
 float dynamicRange;

 float prototypes[PersonalizationConfig::NUM_CLASSES][PersonalizationConfig::NUM_FEATURES];

 float biasOffsets[PersonalizationConfig::NUM_CLASSES];

 uint8_t sampleCounts[PersonalizationConfig::NUM_CLASSES];

 uint8_t reserved2[3];

 uint32_t crc32;
};

#pragma pack(pop)

static_assert(sizeof(MLCalibrationData) == 428, "MLCalibrationData must be exactly 428 bytes for NVS schema stability");


class PersonalizationService {
public:
 PersonalizationService;
 ~PersonalizationService;

 bool init;

 void shutdown;


 bool startCalibration;

 bool captureWindow(const float* features, GestureLabel label);

 bool commitCalibration;

 bool resetCalibration;

 void printStatus const;


 bool isCalibrated const;

 uint8_t getAdaptationQuality const;

 bool getBaselineParams(float& outMean, float& outSigma, float& outDynamicRange) const;

 bool getPrototype(uint8_t classIdx, float* outFeatures) const;

 float getBiasOffset(uint8_t classIdx) const;

 void updateSensorBaseline(float mean, float sigma, float dynamicRange);

 bool forceFlush;

 bool isCalibrating const { return _calibrating; }

private:
 MLCalibrationData _data;
 bool _initialized;
 bool _dirty;
 bool _calibrating;

 struct CaptureAccum {
 double featureSum[PersonalizationConfig::NUM_FEATURES];
 uint16_t count;
 GestureLabel label;
 };
 CaptureAccum _captureAccum;

 MLCalibrationData _snapshot;

 SemaphoreHandle_t _mutex;
 TaskHandle_t _workerTask;
 std::atomic<bool> _workerRunning;
 TimerHandle_t _commitTimer;

 bool readFromNvs;
 bool writeToNvs;
 uint32_t calculateCrc const;
 void resetDataToDefaults;

 void publishSnapshot;

 static void workerTask(void* pvParameters);
 static void commitTimerCallback(TimerHandle_t xTimer);

 static constexpr uint32_t NOTIFY_SAVE = (1 << 0);
 static constexpr uint32_t NOTIFY_SHUTDOWN = (1 << 1);

 static constexpr const char* TAG = "ML_PERSON";
};

#endif
