
#ifndef DATA_LOGGER_SERVICE_H
#define DATA_LOGGER_SERVICE_H

#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "../Config.h"
#include "../Domain/SampleWindow.h"

class DataLoggerService {
public:
 DataLoggerService;
 ~DataLoggerService;

 bool init;

 void shutdown;


 void appendSample(uint16_t proximity, uint16_t dt_ms);


 bool startSession(GestureLabel label);

 bool stopSession;

 bool deleteAll;


 LoggerState getState const { return _state; }

 const char* getStateName const;

 uint16_t getCurrentSessionWindowCount const { return _windowsWritten; }

 uint32_t getDropCount const { return _dropCount; }

 uint16_t getCurrentSessionId const { return _currentSessionId; }

 void printStatus const;

 bool getSpiffsUsage(size_t& totalBytes, size_t& usedBytes) const;


 void setBootCount(uint8_t bootCount) { _cachedBootCount = bootCount; }

 void setRuntimeFlags(uint8_t flags) { _cachedFlags.store(flags, std::memory_order_relaxed); }


 bool getCurrentWindow(SampleWindow* outWindow) const;

 void setInferenceTaskHandle(TaskHandle_t handle) { _inferenceTaskHandle = handle; }

private:
 std::atomic<LoggerState> _state;
 std::atomic<bool> _initialized;
 std::atomic<bool> _loggerTaskRunning;

 uint16_t _currentSessionId;
 uint16_t _currentWindowId;
 uint16_t _windowsWritten;
 GestureLabel _currentLabel;

 std::atomic<uint32_t> _dropCount;
 std::atomic<uint32_t> _totalWindowsWritten;

 SampleRecord _sampleBuf[MLConfig::WINDOW_SIZE];
 uint8_t _sampleIndex;
 uint32_t _windowStartUptime;

 uint8_t _cachedBootCount;
 std::atomic<uint8_t> _cachedFlags;

 SampleWindow _inferenceWindow;
 mutable SemaphoreHandle_t _inferenceMutex;

 QueueHandle_t _loggerQueue;
 TaskHandle_t _loggerTaskHandle;
 TaskHandle_t _inferenceTaskHandle;

 bool _spiffsMounted;
 uint16_t _nextSessionId;


 static void loggerTask(void* pvParameters);

 void processWrite(const SampleWindow& window);

 bool mountSpiffs;

 void unmountSpiffs;

 bool ensureDirectory;

 uint16_t rebuildNextSessionId;

 uint16_t loadNextSessionId;

 bool appendManifest(uint16_t sessionId, GestureLabel label);

 void buildSessionPath(uint16_t sessionId, char* outPath, size_t pathLen) const;

 void fillWindowHeader(WindowHeader& header) const;

 static uint32_t calcCRC32(const uint8_t* data, size_t len);
};

#endif
