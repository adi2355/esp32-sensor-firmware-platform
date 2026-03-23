
#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <Arduino.h>
#include "esp_sntp.h"
#include <esp_system.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "../Config.h"

namespace TimeConfig {
 constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
 constexpr const char* NTP_SERVER_2 = "time.google.com";
 constexpr const char* NTP_SERVER_3 = "time.cloudflare.com";
 
 constexpr uint32_t MIN_VALID_EPOCH = 1577836800;
 
 constexpr uint32_t SNTP_SYNC_INTERVAL_MS = 3600000;
 
 constexpr uint32_t TIME_CHECK_INTERVAL_MS = 1000;
 
 
 constexpr uint32_t ONE_SHOT_SYNC_TIMEOUT_MS = 20000;
 
 constexpr uint32_t WIFI_CONNECT_GRACE_MS = 5000;
 
 constexpr uint32_t PROVISIONING_TIMEOUT_MS = 300000;
}

enum class ClockState : uint8_t {
 UNSYNCED,
 SYNCING,
 VALID
};

class TimeService {
public:
 
 static TimeService* getInstance;
 
 
 void init;
 
 void update;
 
 void startSync;
 
 void stopSync;
 
 
 void evaluateBootSyncPolicy;
 
 bool isOneShotSyncActive const;
 
 void startProvisioningMode;
 
 static const char* getResetReasonString(esp_reset_reason_t reason);
 
 
 bool isTimeValid const;
 
 uint32_t getEpoch const;
 
 ClockState getState const;
 
 const char* getStateName const;
 
 
 using TimeSyncCallback = void (*)(uint32_t epoch);
 
 void setTimeSyncCallback(TimeSyncCallback callback);
 
 
 void getDiagnostics(char* buffer, size_t bufferSize) const;

private:
 TimeService;
 
 TimeService(const TimeService&) = delete;
 TimeService& operator=(const TimeService&) = delete;
 
 static TimeService* _instance;
 
 
 ClockState _state;
 bool _initialized;
 bool _sntpStarted;
 bool _callbackFired;
 unsigned long _lastTimeCheck;
 unsigned long _syncStartTime;
 
 TimeSyncCallback _onTimeSync;
 
 
 TimerHandle_t _syncTimeoutTimer;
 
 bool _oneShotSyncActive;
 
 esp_reset_reason_t _bootReason;
 
 
 bool checkTimeValidity;
 
 void initSNTP;
 
 static void sntpSyncNotification(struct timeval* tv);
 
 
 void handleSyncSuccess;
 
 void handleSyncTimeout;
 
 static void syncTimeoutCallback(TimerHandle_t xTimer);
};

#endif

