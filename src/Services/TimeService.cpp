
#include "TimeService.h"
#include "WifiService.h"

static const char* TAG = "TIME";

static TimeService* g_timeServiceInstance = nullptr;


TimeService* TimeService::_instance = nullptr;

TimeService* TimeService::getInstance {
 if (_instance == nullptr) {
 _instance = new TimeService;
 g_timeServiceInstance = _instance;
 }
 return _instance;
}


TimeService::TimeService
 : _state(ClockState::UNSYNCED)
 , _initialized(false)
 , _sntpStarted(false)
 , _callbackFired(false)
 , _lastTimeCheck(0)
 , _syncStartTime(0)
 , _onTimeSync(nullptr)
 , _syncTimeoutTimer(nullptr)
 , _oneShotSyncActive(false)
 , _bootReason(ESP_RST_UNKNOWN)
{
 ESP_LOGI(TAG, "TimeService constructed");
}


void TimeService::init {
 if (_initialized) {
 ESP_LOGW(TAG, "TimeService already initialized");
 return;
 }
 
 ESP_LOGI(TAG, "Initializing TimeService (One-Shot Policy)...");
 
 setenv("TZ", "UTC0", 1);
 tzset;
 
 _bootReason = esp_reset_reason;
 ESP_LOGI(TAG, "Boot reason: %s (%d)", getResetReasonString(_bootReason), (int)_bootReason);
 
 _syncTimeoutTimer = xTimerCreate(
 "NTPTimeout",
 pdMS_TO_TICKS(TimeConfig::ONE_SHOT_SYNC_TIMEOUT_MS),
 pdFALSE,
 this,
 syncTimeoutCallback
 );
 
 if (_syncTimeoutTimer == nullptr) {
 ESP_LOGE(TAG, "CRITICAL: Failed to create sync timeout timer!");
 } else {
 ESP_LOGI(TAG, "Sync timeout timer created (timeout: %lu ms)", TimeConfig::ONE_SHOT_SYNC_TIMEOUT_MS);
 }
 
 _initialized = true;
 _state = ClockState::UNSYNCED;
 
 ESP_LOGI(TAG, "TimeService initialized. State: %s", getStateName);
}


void TimeService::update {
 if (!_initialized) {
 return;
 }
 
 unsigned long now = millis;
 
 if (now - _lastTimeCheck < TimeConfig::TIME_CHECK_INTERVAL_MS) {
 return;
 }
 _lastTimeCheck = now;
 
 switch (_state) {
 case ClockState::UNSYNCED:
 break;
 
 case ClockState::SYNCING:
 if (checkTimeValidity) {
 _state = ClockState::VALID;
 
 uint32_t epoch = getEpoch;
 
 ESP_LOGI(TAG, "TIME SYNCHRONIZED!");
 ESP_LOGI(TAG, " Epoch: %lu", epoch);
 ESP_LOGI(TAG, " Sync took: %lu ms", now - _syncStartTime);
 
 if (_onTimeSync && !_callbackFired) {
 _callbackFired = true;
 ESP_LOGI(TAG, "Triggering time sync callback for timestamp backfill");
 _onTimeSync(epoch);
 }
 
 if (_oneShotSyncActive) {
 handleSyncSuccess;
 }
 }
 break;
 
 case ClockState::VALID:
 
 if (!checkTimeValidity) {
 ESP_LOGW(TAG, "Time validity lost! Returning to UNSYNCED");
 _state = ClockState::UNSYNCED;
 }
 break;
 }
}


void TimeService::startSync {
 if (!_initialized) {
 ESP_LOGE(TAG, "Cannot start sync - not initialized");
 return;
 }
 
 if (checkTimeValidity) {
 _state = ClockState::VALID;
 ESP_LOGI(TAG, "Time already valid - skipping sync");
 
 if (_onTimeSync && !_callbackFired) {
 _callbackFired = true;
 ESP_LOGI(TAG, "Triggering time sync callback (time was already valid)");
 _onTimeSync(getEpoch);
 }
 return;
 }
 
 ESP_LOGI(TAG, "Starting SNTP synchronization...");
 
 _state = ClockState::SYNCING;
 _syncStartTime = millis;
 
 initSNTP;
}

void TimeService::stopSync {
 if (!_sntpStarted) {
 return;
 }
 
 ESP_LOGI(TAG, "Stopping SNTP synchronization");
 
 sntp_stop;
 _sntpStarted = false;
 
}


void TimeService::evaluateBootSyncPolicy {
 ESP_LOGI(TAG, "EVALUATING BOOT SYNC POLICY ");

 ESP_LOGW(TAG, " [SKIP] Policy Override: Wi-Fi Disabled by User Request");
 ESP_LOGI(TAG, " -> Skipping Wi-Fi/NTP checks.");
 ESP_LOGI(TAG, " -> Will rely exclusively on BLE time sync (Tier 2) or RTC.");
 return;
 
 if (checkTimeValidity) {
 uint32_t epoch = getEpoch;
 ESP_LOGI(TAG, " [SKIP] Time already valid (epoch: %lu)", epoch);
 ESP_LOGI(TAG, " -> Wi-Fi NOT needed. Using RTC time.");
 _state = ClockState::VALID;
 
 if (_onTimeSync && !_callbackFired) {
 _callbackFired = true;
 _onTimeSync(epoch);
 }
 return;
 }
 
 
 ESP_LOGI(TAG, " Boot reason: %s (%d)", getResetReasonString(_bootReason), (int)_bootReason);
 
 switch (_bootReason) {
 case ESP_RST_DEEPSLEEP:
 ESP_LOGW(TAG, " [SKIP] Deep Sleep Wake - Wi-Fi DISABLED to save power");
 ESP_LOGI(TAG, " -> Will use BLE time sync if phone connects");
 return;
 
 case ESP_RST_BROWNOUT:
 ESP_LOGE(TAG, " [SKIP] Brownout Recovery - Wi-Fi DISABLED (boot loop prevention)");
 ESP_LOGW(TAG, " -> Battery critically low. Charge device.");
 return;
 
 case ESP_RST_PANIC:
 case ESP_RST_INT_WDT:
 case ESP_RST_TASK_WDT:
 case ESP_RST_WDT:
 ESP_LOGW(TAG, " [SKIP] Crash Recovery - Wi-Fi DISABLED (conservative recovery)");
 return;
 
 case ESP_RST_POWERON:
 case ESP_RST_SW:
 case ESP_RST_SDIO:
 default:
 ESP_LOGI(TAG, " [OK] Boot reason allows Wi-Fi sync");
 break;
 }
 
 WifiService* wifi = WifiService::getInstance;
 if (!wifi->hasCredentials) {
 ESP_LOGI(TAG, " [SKIP] No Wi-Fi credentials configured");
 ESP_LOGI(TAG, " -> User can configure via BLE + App");
 return;
 }
 
 ESP_LOGI(TAG, " [OK] Wi-Fi credentials available");
 
 
 ESP_LOGI(TAG, "STARTING ONE-SHOT WI-FI TIME SYNC");
 ESP_LOGI(TAG, " Timeout: %lu ms", TimeConfig::ONE_SHOT_SYNC_TIMEOUT_MS);
 
 _oneShotSyncActive = true;
 _syncStartTime = millis;
 
 if (_syncTimeoutTimer != nullptr) {
 if (xTimerStart(_syncTimeoutTimer, 0) != pdPASS) {
 ESP_LOGW(TAG, "Warning: Failed to start timeout timer");
 }
 }
 
 wifi->startConnection;
 
 ESP_LOGI(TAG, "Wi-Fi connection initiated. Waiting for NTP sync...");
}

bool TimeService::isOneShotSyncActive const {
 return _oneShotSyncActive;
}

const char* TimeService::getResetReasonString(esp_reset_reason_t reason) {
 switch (reason) {
 case ESP_RST_UNKNOWN: return "UNKNOWN";
 case ESP_RST_POWERON: return "POWER_ON";
 case ESP_RST_EXT: return "EXTERNAL";
 case ESP_RST_SW: return "SOFTWARE";
 case ESP_RST_PANIC: return "PANIC";
 case ESP_RST_INT_WDT: return "INTERRUPT_WDT";
 case ESP_RST_TASK_WDT: return "TASK_WDT";
 case ESP_RST_WDT: return "OTHER_WDT";
 case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
 case ESP_RST_BROWNOUT: return "BROWNOUT";
 case ESP_RST_SDIO: return "SDIO";
 default: return "UNRECOGNIZED";
 }
}


void TimeService::startProvisioningMode {
 ESP_LOGI(TAG, "ENTERING PROVISIONING MODE");
 ESP_LOGI(TAG, " Reason: MSG_SET_WIFI received from app");
 ESP_LOGI(TAG, " Action: Extending timeout from %lu ms to %lu ms",
 TimeConfig::ONE_SHOT_SYNC_TIMEOUT_MS,
 TimeConfig::PROVISIONING_TIMEOUT_MS);
 
 _oneShotSyncActive = true;
 _syncStartTime = millis;
 
 if (_syncTimeoutTimer != nullptr) {
 BaseType_t result = xTimerChangePeriod(
 _syncTimeoutTimer,
 pdMS_TO_TICKS(TimeConfig::PROVISIONING_TIMEOUT_MS),
 pdMS_TO_TICKS(100)
 );
 
 if (result == pdPASS) {
 ESP_LOGI(TAG, " Timer extended: %lu ms -> %lu ms",
 TimeConfig::ONE_SHOT_SYNC_TIMEOUT_MS,
 TimeConfig::PROVISIONING_TIMEOUT_MS);
 } else {
 ESP_LOGW(TAG, " WARNING: Failed to extend timeout timer");
 }
 } else {
 ESP_LOGW(TAG, " WARNING: Timeout timer not initialized");
 }
 
 ESP_LOGI(TAG, "Provisioning mode active. Device will stay awake for Wi-Fi config.");
}

void TimeService::handleSyncSuccess {
 ESP_LOGI(TAG, "ONE-SHOT SYNC SUCCESS");
 
 uint32_t syncDuration = millis - _syncStartTime;
 uint32_t epoch = getEpoch;
 
 ESP_LOGI(TAG, " Epoch: %lu", epoch);
 ESP_LOGI(TAG, " Sync Duration: %lu ms", syncDuration);
 
 if (_syncTimeoutTimer != nullptr) {
 xTimerStop(_syncTimeoutTimer, 0);
 }
 
 _oneShotSyncActive = false;
 
 ESP_LOGI(TAG, "Time acquired. SHUTTING DOWN WI-FI to save power...");
 
 WifiService* wifi = WifiService::getInstance;
 wifi->shutdown;
 
 stopSync;
 
 ESP_LOGI(TAG, "Wi-Fi shutdown complete. Battery savings active.");
}

void TimeService::handleSyncTimeout {
 ESP_LOGW(TAG, "ONE-SHOT SYNC TIMEOUT");
 
 uint32_t elapsed = millis - _syncStartTime;
 
 ESP_LOGW(TAG, " Elapsed: %lu ms", elapsed);
 ESP_LOGW(TAG, " Limit: %lu ms", TimeConfig::ONE_SHOT_SYNC_TIMEOUT_MS);
 ESP_LOGW(TAG, " Result: NTP sync FAILED");
 
 _oneShotSyncActive = false;
 
 ESP_LOGW(TAG, "SHUTTING DOWN WI-FI (timeout protection)...");
 
 WifiService* wifi = WifiService::getInstance;
 wifi->shutdown;
 
 stopSync;
 
 ESP_LOGW(TAG, "Wi-Fi shutdown complete. Fallback to BLE/relative time.");
}

void TimeService::syncTimeoutCallback(TimerHandle_t xTimer) {
 TimeService* self = static_cast<TimeService*>(pvTimerGetTimerID(xTimer));
 
 if (self != nullptr) {
 self->handleSyncTimeout;
 }
}


bool TimeService::isTimeValid const {
 return _state == ClockState::VALID;
}

uint32_t TimeService::getEpoch const {
 time_t now;
 time(&now);
 
 if (now < TimeConfig::MIN_VALID_EPOCH) {
 return 0;
 }
 
 return (uint32_t)now;
}

ClockState TimeService::getState const {
 return _state;
}

const char* TimeService::getStateName const {
 switch (_state) {
 case ClockState::UNSYNCED: return "UNSYNCED";
 case ClockState::SYNCING: return "SYNCING";
 case ClockState::VALID: return "VALID";
 default: return "UNKNOWN";
 }
}


void TimeService::setTimeSyncCallback(TimeSyncCallback callback) {
 _onTimeSync = callback;
}


bool TimeService::checkTimeValidity {
 time_t now;
 time(&now);
 
 return (now >= TimeConfig::MIN_VALID_EPOCH);
}

void TimeService::initSNTP {
 if (_sntpStarted) {
 sntp_stop;
 _sntpStarted = false;
 }
 
 ESP_LOGI(TAG, "Configuring SNTP...");
 ESP_LOGI(TAG, " Server 1: %s", TimeConfig::NTP_SERVER_1);
 ESP_LOGI(TAG, " Server 2: %s", TimeConfig::NTP_SERVER_2);
 ESP_LOGI(TAG, " Server 3: %s", TimeConfig::NTP_SERVER_3);
 
 sntp_setoperatingmode(SNTP_OPMODE_POLL);
 
 sntp_setservername(0, (char*)TimeConfig::NTP_SERVER_1);
 sntp_setservername(1, (char*)TimeConfig::NTP_SERVER_2);
 sntp_setservername(2, (char*)TimeConfig::NTP_SERVER_3);
 
 sntp_set_time_sync_notification_cb(sntpSyncNotification);
 
 sntp_init;
 _sntpStarted = true;
 
 ESP_LOGI(TAG, "SNTP started - waiting for time sync...");
}

void TimeService::sntpSyncNotification(struct timeval* tv) {
 ESP_LOGI(TAG, "SNTP sync notification received");
 
 if (tv != nullptr) {
 ESP_LOGI(TAG, " Time received: %ld.%06ld", (long)tv->tv_sec, (long)tv->tv_usec);
 }
 
 
}


void TimeService::getDiagnostics(char* buffer, size_t bufferSize) const {
 uint32_t epoch = getEpoch;
 
 snprintf(buffer, bufferSize,
 "Time: state=%s, epoch=%lu, sntpStarted=%d, callbackFired=%d",
 getStateName,
 epoch,
 _sntpStarted ? 1 : 0,
 _callbackFired ? 1 : 0);
}

