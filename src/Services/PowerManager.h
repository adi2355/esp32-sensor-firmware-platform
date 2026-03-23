
#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <Arduino.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_pm.h"
#include "../Config.h"

class BLEManager;
class HistoryService;
class ProtocolHandler;
class OTAManager;

enum class PowerState : uint8_t {
 BOOT,
 WAKE_MODE,
 ACTIVE,
 DETECTION_ACTIVE,
 SLEEP_PREP,
 DEEP_SLEEP
};

enum class WakeReason : uint8_t {
 POWER_ON,
 BUTTON,
 SENSOR,
 TIMER,
 BROWNOUT,
 SOFTWARE,
 UNKNOWN
};

class PowerManager {
private:
 BLEManager* _bleManager;
 HistoryService* _historyService;
 ProtocolHandler* _protocolHandler;
 OTAManager* _otaManager;
 
 PowerState _currentState;
 PowerState _previousState;
 WakeReason _wakeReason;
 
 unsigned long _stateEntryTime;
 unsigned long _sleepPrepStartTime;
 unsigned long _lastBatteryCheckTime;
 
 uint8_t _batteryPercent;
 bool _isCharging;
 uint16_t _lastAdcReading;

 float _smoothedPercent;
 uint8_t _batterySampleCount;
 unsigned long _lastBatteryLogTime;
 
 bool _initialized;
 bool _sleepDeferred;
 bool _brownoutDetected;
 
 uint8_t _consecutiveDetections;
 unsigned long _lastHitTime;
 static constexpr uint8_t PARTY_MODE_DETECTION_THRESHOLD = 3;
 static constexpr unsigned long PARTY_MODE_WINDOW_MS = 30000;
 bool _partyModeActive;
 
 uint8_t _sleepCancelCount;
 
 bool _wokeFromSensor;
 
 float _rollingProxAverage;
 uint16_t _rollingProxSampleCount;
 
 unsigned long _lastSleepCancellationTime;
 
 unsigned long _lastActivityTime;
 
 portMUX_TYPE _mux;
 
 WakeReason detectWakeReason;
 
 uint32_t readBatteryVoltage;

 static uint8_t voltageToPercent(uint32_t voltageMv);
 
 bool checkChargingStatus;
 
 void configureWakeSources;
 
public:
 PowerManager;
 
void init(BLEManager* bleManager, HistoryService* historyService, ProtocolHandler* protocolHandler = nullptr);
 
 
 PowerState getState const { return _currentState; }
 
 const char* getStateName const;
 
 WakeReason getWakeReason const { return _wakeReason; }
 
 const char* getWakeReasonName const;
 
 void transitionTo(PowerState newState);
 
 unsigned long getTimeInState const;
 
 
 void updateBatteryStatus;
 
 uint8_t getBatteryPercent const { return _batteryPercent; }

 uint16_t getBatteryVoltageMv const { return _lastAdcReading; }

 bool isCharging const { return _isCharging; }
 
 bool isBatteryLow const;
 
 bool isBatteryCritical const;
 
 
 void requestSleep;
 
 void cancelSleep;
 
 void deferSleep;
 
 bool isSleepDeferred const { return _sleepDeferred; }
 
void enterDeepSleep;

bool canSafelyEnterSleep const;
 
 unsigned long getSleepPrepRemaining const;
 
 bool shouldForceSleep const;
 
 bool incrementSleepCancelCount;
 
 
 unsigned long getLastSleepCancellationTime const { return _lastSleepCancellationTime; }
 
 bool wokeFromSensor const { return _wokeFromSensor; }
 
 void clearSensorWakeFlag;
 
 void updateRollingProxAverage(uint16_t proximity);
 
 uint16_t getDynamicSleepCancelThreshold const;
 
 void resetRollingProxAverage;
 
 
 void checkIdleTimeout;
 
 
 void update;
 
 bool checkBrownout;
 
 
 void notifyActivity;
 
 unsigned long getTimeSinceActivity const;
 
 
 void setOTAManager(OTAManager* otaManager);

 void (*onBeforeSleep) = nullptr;

 
 void getDiagnostics(char* buffer, size_t bufferSize) const;
};

#endif
