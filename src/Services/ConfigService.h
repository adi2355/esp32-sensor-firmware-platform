
#ifndef CONFIG_SERVICE_H
#define CONFIG_SERVICE_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "../Domain/UserConfig.h"

class Sensor_HAL;

class ConfigService {
private:
 static ConfigService* _instance;
 
 Sensor_HAL* _sensorHal;
 
 UserConfig _config;
 
 bool _initialized;
 bool _dirty;
 
 TimerHandle_t _commitTimer;
 
 TaskHandle_t _configWorkerTask;
 volatile bool _configWorkerRunning;
 
 static constexpr uint32_t NOTIFY_SAVE_CONFIG = (1 << 0);
 static constexpr uint32_t NOTIFY_SHUTDOWN = (1 << 1);
 
 SemaphoreHandle_t _mutex;
 
 static constexpr const char* TAG = "CONFIG";
 
 static constexpr uint32_t COMMIT_DELAY_MS = 30000;
 
 ConfigService;
 
 ConfigService(const ConfigService&) = delete;
 ConfigService& operator=(const ConfigService&) = delete;
 
 bool writeToNvs;
 
 bool readFromNvs;
 
 uint32_t calculateConfigCrc const;
 
 void applySensitivityToHal;
 
 static void commitTimerCallback(TimerHandle_t xTimer);
 
 static void configWorkerTask(void* pvParameters);
 
public:
 static ConfigService* getInstance;
 
 bool init(Sensor_HAL* sensorHal);
 
 
 uint8_t getSensitivity const;
 
 void setSensitivity(uint8_t sensitivity);
 
 
 uint8_t getLedBrightness const;
 
 void setLedBrightness(uint8_t brightness);
 
 
 bool isDirty const;
 
 bool forceFlush;
 
 bool factoryReset;
 
 
 void getConfig(UserConfig* outConfig) const;
 
 uint32_t getConfigCrc const;
 
 uint32_t getConfigSignature const;
 
 
 uint32_t getLastKnownEpoch const;
 
 void updateLastKnownEpoch(uint32_t epochSeconds);
 
 
 void getDiagnostics(char* buffer, size_t bufferSize) const;
};

#endif

