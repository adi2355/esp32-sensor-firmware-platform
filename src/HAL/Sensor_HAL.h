
#ifndef SENSOR_HAL_H
#define SENSOR_HAL_H

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include "HAL_Base.h"

void IRAM_ATTR sensorInterruptHandler;

class Sensor_HAL {
private:
 static Sensor_HAL* _instance;
 
 bool _initialized;
 bool _powerOn;
 bool _i2cInitialized;
 
 uint16_t _wakeThreshold;
 
 std::atomic<uint16_t> _detectionThreshold;
 
 std::atomic<uint16_t> _criticalDetectionThreshold;
 
 
 std::atomic<uint8_t> _userSensitivity;
 
 uint16_t _baseWakeThreshold;
 uint16_t _baseDetectionThreshold;
 uint16_t _baseCriticalThreshold;

 float _lastCalMean;
 float _lastCalSigma;

 std::atomic<int16_t> _environmentBackoff{0};

 
 std::atomic<uint16_t> _lastReading;
 std::atomic<unsigned long> _lastReadTime;
 
 std::atomic<bool> _interruptTriggered;
 std::atomic<unsigned long> _lastInterruptTime;
 
 std::atomic<bool> _hitsSuppressed;
 std::atomic<unsigned long> _suppressEndTime;
 
 portMUX_TYPE _mux;
 
 friend void IRAM_ATTR sensorInterruptHandler;
 
 Sensor_HAL;
 
 Sensor_HAL(const Sensor_HAL&) = delete;
 Sensor_HAL& operator=(const Sensor_HAL&) = delete;
 
public:
 static Sensor_HAL* getInstance;
 
 HalError init;
 
 uint16_t readProximity;
 
 uint16_t getLastReading const;
 
 HalError setWakeThreshold(uint16_t threshold);
 
 HalError setDetectionThreshold(uint16_t threshold);
 
 uint16_t getWakeThreshold const { return _wakeThreshold; }
 
 uint16_t getDetectionThreshold const {
 int32_t effective = static_cast<int32_t>(_detectionThreshold.load(std::memory_order_relaxed))
 + _environmentBackoff.load(std::memory_order_relaxed);
 if (effective < 0) effective = 0;
 if (effective > 65535) effective = 65535;
 return static_cast<uint16_t>(effective);
 }

 uint16_t getScaledDetectionThreshold const { return _detectionThreshold.load(std::memory_order_relaxed); }
 
 HalError setCriticalDetectionThreshold(uint16_t threshold);
 
 uint16_t getCriticalDetectionThreshold const {
 int32_t effective = static_cast<int32_t>(_criticalDetectionThreshold.load(std::memory_order_relaxed))
 + _environmentBackoff.load(std::memory_order_relaxed);
 if (effective < 0) effective = 0;
 if (effective > 65535) effective = 65535;
 return static_cast<uint16_t>(effective);
 }

 uint16_t getScaledCriticalDetectionThreshold const {
 return _criticalDetectionThreshold.load(std::memory_order_relaxed);
 }
 
 
 void setUserSensitivity(uint8_t sensitivity);
 
 
 void suppressDetections(uint32_t durationMs);
 
 bool areHitsSuppressed const;
 
 uint8_t getUserSensitivity const {
 return _userSensitivity.load(std::memory_order_relaxed);
 }
 
 uint16_t getBaseWakeThreshold const { return _baseWakeThreshold; }
 
 uint16_t getBaseDetectionThreshold const { return _baseDetectionThreshold; }
 
 uint16_t getBaseCriticalThreshold const { return _baseCriticalThreshold; }

 float getLastCalibrationMean const { return _lastCalMean; }

 float getLastCalibrationSigma const { return _lastCalSigma; }


 void applyEnvironmentBackoff(int16_t offset);

 void clearEnvironmentBackoff;

 int16_t getEnvironmentBackoff const {
 return _environmentBackoff.load(std::memory_order_relaxed);
 }

 void setBaseThresholds(uint16_t wake, uint16_t detection, uint16_t critical);

 void configureForWakeMode;
 
 void configureForHitDetection;
 
 bool configureForDeepSleep;
 
 bool isProximityBelowThreshold(uint16_t threshold);
 
 HalError calibrate(uint32_t durationMs, 
 uint16_t* outWakeThreshold, 
 uint16_t* outDetectionThreshold,
 uint16_t* outCriticalThreshold = nullptr);
 
 uint16_t getSensorId;
 
 bool isPresent;
 
 bool isI2CReady const { return _i2cInitialized; }
 
 bool ensureI2CReady;
 
 void powerDown;
 
 void powerUp;
 
 
 HalError enableInterrupt;
 
 void disableInterrupt;
 
 bool checkTrigger;
 
 bool isInterruptTriggered const { 
 return _interruptTriggered.load(std::memory_order_relaxed); 
 }
 
 void clearInterrupt;
 
private:
 bool writeRegister(uint8_t reg, uint16_t value);
 
 bool readRegister(uint8_t reg, uint16_t* value);
 
 void initializeRegisters;
 
 void recalculateThresholdsFromSensitivity;
 
 void recoverI2CBus;
};


inline Sensor_HAL* Sensor_HAL::getInstance {
 static Sensor_HAL instance;
 _instance = &instance;
 return _instance;
}

inline uint16_t Sensor_HAL::getLastReading const {
 return _lastReading.load(std::memory_order_relaxed);
}

#endif

