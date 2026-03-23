
#include "PowerManager.h"
#include "BLEManager.h"
#include "HistoryService.h"
#include "ProtocolHandler.h"
#include "OTAManager.h"
#include "ConfigService.h"
#include "../HAL/LED_HAL.h"
#include "../HAL/Sensor_HAL.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "esp_pm.h"

static const char* TAG = "POWER";


PowerManager::PowerManager
 : _bleManager(nullptr)
 , _historyService(nullptr)
 , _protocolHandler(nullptr)
 , _otaManager(nullptr)
 , _currentState(PowerState::BOOT)
 , _previousState(PowerState::BOOT)
 , _wakeReason(WakeReason::UNKNOWN)
 , _stateEntryTime(0)
 , _sleepPrepStartTime(0)
 , _lastBatteryCheckTime(0)
 , _batteryPercent(100)
 , _isCharging(false)
 , _lastAdcReading(0)
 , _smoothedPercent(100.0f)
 , _batterySampleCount(0)
 , _lastBatteryLogTime(0)
 , _initialized(false)
 , _sleepDeferred(false)
 , _brownoutDetected(false)
 , _consecutiveDetections(0)
 , _lastHitTime(0)
 , _partyModeActive(false)
 , _sleepCancelCount(0)
 , _wokeFromSensor(false)
 , _rollingProxAverage(0.0f)
 , _rollingProxSampleCount(0)
 , _lastSleepCancellationTime(0)
 , _lastActivityTime(0)
 , _mux(portMUX_INITIALIZER_UNLOCKED)
{
}


void PowerManager::init(BLEManager* bleManager, HistoryService* historyService, ProtocolHandler* protocolHandler) {
 _bleManager = bleManager;
 _historyService = historyService;
 _protocolHandler = protocolHandler;
 
 analogReadResolution(12);
 analogSetAttenuation(ADC_11db);
 pinMode(Pins::BAT_ADC, INPUT);
 
 gpio_reset_pin(Pins::CHARGING);
 pinMode(Pins::CHARGING, INPUT_PULLUP);
 
 _wakeReason = detectWakeReason;
 
 if (_wakeReason == WakeReason::SENSOR) {
 _wokeFromSensor = true;
 ESP_LOGI(TAG, "Sensor wake detected - quick sleep mode enabled");
 }
 
 
 #if defined(CONFIG_PM_ENABLE)
 esp_pm_config_esp32s3_t pm_config;
 pm_config.max_freq_mhz = 240;
 pm_config.min_freq_mhz = 80;
 pm_config.light_sleep_enable = true;
 
 esp_err_t err = esp_pm_configure(&pm_config);
 if (err == ESP_OK) {
 ESP_LOGI(TAG, "Power Management: Automatic Light Sleep ENABLED (80-240MHz)");
 } else {
 ESP_LOGW(TAG, "Power Management: Light Sleep configure failed (err=%d).", err);
 ESP_LOGW(TAG, " Note: This is common in Arduino if CONFIG_PM_ENABLE is missing in pre-compiled libs.");
 ESP_LOGW(TAG, " Bluetooth Modem Sleep should still work via BLE stack.");
 }
 #else
 ESP_LOGW(TAG, "Power Management: CONFIG_PM_ENABLE not defined - Light Sleep DISABLED");
 ESP_LOGW(TAG, " Add -DCONFIG_PM_ENABLE=1 to build_flags in platformio.ini");
 #endif
 
 ESP_LOGI(TAG, "PowerManager initialized");
 ESP_LOGI(TAG, " Wake reason: %s", getWakeReasonName);
 
 updateBatteryStatus;
 ESP_LOGI(TAG, " Battery: %u%% %s", _batteryPercent, _isCharging ? "(charging)" : "");
 
 _initialized = true;
 _stateEntryTime = millis;
}


void PowerManager::setOTAManager(OTAManager* otaManager) {
 _otaManager = otaManager;
 if (_otaManager) {
 ESP_LOGI(TAG, "OTAManager linked for Process-Lock checks");
 }
}


WakeReason PowerManager::detectWakeReason {
 esp_reset_reason_t resetReason = esp_reset_reason;
 esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause;
 
 if (resetReason == ESP_RST_BROWNOUT) {
 _brownoutDetected = true;
 return WakeReason::BROWNOUT;
 }
 
 switch (wakeupCause) {
 case ESP_SLEEP_WAKEUP_EXT0:
 return WakeReason::BUTTON;
 
 case ESP_SLEEP_WAKEUP_EXT1:
 return WakeReason::SENSOR;
 
 case ESP_SLEEP_WAKEUP_TIMER:
 return WakeReason::TIMER;
 
 case ESP_SLEEP_WAKEUP_UNDEFINED:
 switch (resetReason) {
 case ESP_RST_POWERON:
 return WakeReason::POWER_ON;
 case ESP_RST_SW:
 return WakeReason::SOFTWARE;
 default:
 return WakeReason::UNKNOWN;
 }
 
 default:
 return WakeReason::UNKNOWN;
 }
}


const char* PowerManager::getStateName const {
 switch (_currentState) {
 case PowerState::BOOT: return "BOOT";
 case PowerState::WAKE_MODE: return "WAKE_MODE";
 case PowerState::ACTIVE: return "ACTIVE";
 case PowerState::DETECTION_ACTIVE: return "DETECTION_ACTIVE";
 case PowerState::SLEEP_PREP: return "SLEEP_PREP";
 case PowerState::DEEP_SLEEP: return "DEEP_SLEEP";
 default: return "UNKNOWN";
 }
}

const char* PowerManager::getWakeReasonName const {
 switch (_wakeReason) {
 case WakeReason::POWER_ON: return "POWER_ON";
 case WakeReason::BUTTON: return "BUTTON";
 case WakeReason::SENSOR: return "SENSOR";
 case WakeReason::TIMER: return "TIMER";
 case WakeReason::BROWNOUT: return "BROWNOUT";
 case WakeReason::SOFTWARE: return "SOFTWARE";
 case WakeReason::UNKNOWN: return "UNKNOWN";
 default: return "INVALID";
 }
}

void PowerManager::transitionTo(PowerState newState) {
 if (_currentState == newState) {
 return;
 }
 
 if (newState == PowerState::SLEEP_PREP || newState == PowerState::DEEP_SLEEP) {
 if (_partyModeActive) {
 ESP_LOGI(TAG, "Party Mode RESET (entering sleep - session ended)");
 _partyModeActive = false;
 _consecutiveDetections = 0;
 }
 }
 
 _previousState = _currentState;
 _currentState = newState;
 _stateEntryTime = millis;
 _sleepDeferred = false;
 
 ESP_LOGI(TAG, "State: %s -> %s", 
 getStateName, 
 (newState == PowerState::WAKE_MODE) ? "WAKE_MODE" :
 (newState == PowerState::ACTIVE) ? "ACTIVE" :
 (newState == PowerState::DETECTION_ACTIVE) ? "DETECTION_ACTIVE" :
 (newState == PowerState::SLEEP_PREP) ? "SLEEP_PREP" :
 (newState == PowerState::DEEP_SLEEP) ? "DEEP_SLEEP" : "UNKNOWN");
 
 LED_HAL* led = LED_HAL::getInstance;
 
 switch (newState) {
 case PowerState::WAKE_MODE:
 if (led) {
 bool advertising = _bleManager != nullptr && _bleManager->isAdvertising;
 if (advertising) {
 led->setPattern(LEDConfig::COLOR_PAIRING, BlinkPattern::SLOW_BLINK);
 } else {
 led->setColor(LEDConfig::COLOR_PAIRING);
 }
 }
 
 resetRollingProxAverage;
 
 _lastActivityTime = millis;
 ESP_LOGD(TAG, "Activity timer reset (entering WAKE_MODE)");
 break;
 
 case PowerState::ACTIVE:
 if (_partyModeActive) {
 if (led) led->setColor(LEDConfig::COLOR_ACTIVE);
 } else {
 if (led) led->setColor(LEDConfig::COLOR_ACTIVE);
 }
 
 _sleepCancelCount = 0;
 ESP_LOGD(TAG, "Sleep cancel count reset (ACTIVE state reached)");
 
 if (_wokeFromSensor) {
 _wokeFromSensor = false;
 ESP_LOGD(TAG, "Sensor wake confirmed valid (BLE connected)");
 }
 
 _lastActivityTime = millis;
 ESP_LOGI(TAG, "Connected idle timer started (%lu ms)", 
 Timing::CONNECTED_IDLE_TIMEOUT_MS);
 break;
 
 case PowerState::DETECTION_ACTIVE:
 if (led) led->setColor(LEDConfig::COLOR_HIT);
 
 if (_wokeFromSensor) {
 _wokeFromSensor = false;
 ESP_LOGI(TAG, "Sensor wake confirmed valid (detection confirmed)");
 }
 
 {
 unsigned long now = millis;
 if (now - _lastHitTime < PARTY_MODE_WINDOW_MS) {
 _consecutiveDetections++;
 } else {
 _consecutiveDetections = 1;
 }
 _lastHitTime = now;
 
 if (_consecutiveDetections >= PARTY_MODE_DETECTION_THRESHOLD) {
 _partyModeActive = true;
 ESP_LOGI(TAG, "Party Mode ACTIVATED! (%u detections)", _consecutiveDetections);
 }
 }
 break;
 
 case PowerState::SLEEP_PREP:
 _sleepPrepStartTime = millis;
 
 
 if (led) led->setPattern(LEDConfig::COLOR_SLEEP_PREP, BlinkPattern::FAST_BLINK);
 ESP_LOGI(TAG, "Sleep prep started (cancel count: %u/%u) - will sleep in %lu ms", 
 _sleepCancelCount, Timing::MAX_SLEEP_CANCEL_COUNT, Timing::SLEEP_PREP_TIME_MS);
 break;
 
 case PowerState::DEEP_SLEEP:
 _sleepCancelCount = 0;
 ESP_LOGD(TAG, "Sleep cancel count reset (entering DEEP_SLEEP)");
 break;
 
 default:
 break;
 }
}

unsigned long PowerManager::getTimeInState const {
 return millis - _stateEntryTime;
}


void PowerManager::updateBatteryStatus {
 uint32_t voltageMv = readBatteryVoltage;

 _lastAdcReading = (uint16_t)(voltageMv > 65535 ? 65535 : voltageMv);

 bool rawChargingState = (digitalRead(Pins::CHARGING) == LOW);

 bool isExternalPower = (voltageMv < BatteryConfig::EXTERNAL_POWER_THRESHOLD_MV);

 if (isExternalPower) {
 _batteryPercent = 100;
 _smoothedPercent = 100.0f;
 _isCharging = false;

 ESP_LOGD(TAG, "USB Power (V=%u mV) - Force 100%%",
 (unsigned)voltageMv);
 }
 else {

 uint8_t rawPercent = voltageToPercent(voltageMv);

 if (_batterySampleCount < 255) {
 _batterySampleCount++;
 }

 if (_batterySampleCount <= BatteryConfig::MIN_SAMPLES_FOR_STABLE) {
 float initAlpha = 1.0f / (float)_batterySampleCount;
 _smoothedPercent = (_smoothedPercent * (1.0f - initAlpha)) + (rawPercent * initAlpha);

 ESP_LOGV(TAG, "Init phase (%u/%u) raw=%u%% smoothed=%.1f%%",
 _batterySampleCount, BatteryConfig::MIN_SAMPLES_FOR_STABLE,
 rawPercent, _smoothedPercent);
 }
 else {
 _smoothedPercent = (BatteryConfig::SMOOTHING_ALPHA * (float)rawPercent) +
 ((1.0f - BatteryConfig::SMOOTHING_ALPHA) * _smoothedPercent);
 }

 uint8_t smoothedInt = (uint8_t)(_smoothedPercent + 0.5f);

 if (smoothedInt > 100) {
 smoothedInt = 100;
 }

 int8_t delta = (int8_t)smoothedInt - (int8_t)_batteryPercent;

 if (delta < 0) {
 delta = -delta;
 }

 if ((uint8_t)delta >= BatteryConfig::HYSTERESIS_PERCENT) {
 _batteryPercent = smoothedInt;

 ESP_LOGD(TAG, "Updated (V=%u mV, raw=%u%%, smoothed=%.1f%%, display=%u%%)",
 (unsigned)voltageMv, rawPercent, _smoothedPercent, _batteryPercent);
 }
 else {
 ESP_LOGV(TAG, "Hysteresis (V=%u mV, raw=%u%%, smoothed=%.1f%%, display=%u%% [unchanged])",
 (unsigned)voltageMv, rawPercent, _smoothedPercent, _batteryPercent);
 }

 _isCharging = rawChargingState;

 if (_isCharging && smoothedInt > _batteryPercent) {
 _batteryPercent = smoothedInt;
 ESP_LOGD(TAG, "Charging increase (display=%u%%)", _batteryPercent);
 }

 if (millis - _lastBatteryLogTime > 60000) {
 ESP_LOGI(TAG, "Battery: V=%u mV, raw=%u%%, smoothed=%.1f%%, display=%u%%, %s",
 (unsigned)voltageMv, rawPercent, _smoothedPercent, _batteryPercent,
 _isCharging ? "CHARGING" : "discharging");
 _lastBatteryLogTime = millis;
 }
 }

 _lastBatteryCheckTime = millis;
}

uint32_t PowerManager::readBatteryVoltage {
 uint32_t sum = 0;

 for (uint8_t i = 0; i < BatteryConfig::ADC_SAMPLES; i++) {
 sum += analogReadMilliVolts(Pins::BAT_ADC);
 delay(1);
 }

 uint32_t avgPinMv = sum / BatteryConfig::ADC_SAMPLES;

 uint32_t batteryMv = avgPinMv * 2;

 return batteryMv;
}

uint8_t PowerManager::voltageToPercent(uint32_t voltageMv) {
 if (voltageMv >= BatteryConfig::OCV_SOC_TABLE[0].voltageMv) {
 return 100;
 }

 if (voltageMv <= BatteryConfig::OCV_SOC_TABLE[BatteryConfig::OCV_TABLE_SIZE - 1].voltageMv) {
 return 0;
 }

 for (uint8_t i = 1; i < BatteryConfig::OCV_TABLE_SIZE; i++) {
 const BatteryConfig::OcvSocEntry& higher = BatteryConfig::OCV_SOC_TABLE[i - 1];
 const BatteryConfig::OcvSocEntry& lower = BatteryConfig::OCV_SOC_TABLE[i];

 if (voltageMv > lower.voltageMv) {

 if (voltageMv >= higher.voltageMv) {
 return higher.percent;
 }

 uint32_t voltageRange = higher.voltageMv - lower.voltageMv;
 uint32_t percentRange = higher.percent - lower.percent;
 uint32_t voltageOffset = voltageMv - lower.voltageMv;

 if (voltageRange == 0) {
 return lower.percent;
 }

 uint32_t interpolatedPercent = lower.percent +
 (percentRange * voltageOffset) / voltageRange;

 if (interpolatedPercent > 100) {
 interpolatedPercent = 100;
 }

 return (uint8_t)interpolatedPercent;
 }
 }

 return 0;
}

bool PowerManager::checkChargingStatus {
 return digitalRead(Pins::CHARGING) == LOW;
}

bool PowerManager::isBatteryLow const {
 return _batteryPercent <= BatteryConfig::LOW_BATTERY_PERCENT && !_isCharging;
}

bool PowerManager::isBatteryCritical const {
 if (_batteryPercent == 0) {
 return false;
 }
 
 return _batteryPercent <= BatteryConfig::CRITICAL_BATTERY_PERCENT && !_isCharging;
}


void PowerManager::requestSleep {
 if (_currentState != PowerState::SLEEP_PREP) {
 transitionTo(PowerState::SLEEP_PREP);
 }
}

void PowerManager::cancelSleep {
 if (_currentState == PowerState::SLEEP_PREP) {
 _lastSleepCancellationTime = millis;
 
 ESP_LOGI(TAG, "Sleep cancelled (cancel count: %u/%u)", 
 _sleepCancelCount, Timing::MAX_SLEEP_CANCEL_COUNT);
 
 if (_bleManager && _bleManager->isConnected) {
 transitionTo(PowerState::ACTIVE);
 } else {
 transitionTo(PowerState::WAKE_MODE);
 }
 }
}

void PowerManager::deferSleep {
 _sleepDeferred = true;
 ESP_LOGD(TAG, "Sleep deferred (pending operations)");
}

unsigned long PowerManager::getSleepPrepRemaining const {
 if (_currentState != PowerState::SLEEP_PREP) {
 return 0;
 }
 
 unsigned long elapsed = millis - _sleepPrepStartTime;
 if (elapsed >= Timing::SLEEP_PREP_TIME_MS) {
 return 0;
 }
 
 return Timing::SLEEP_PREP_TIME_MS - elapsed;
}


bool PowerManager::shouldForceSleep const {
 if (_sleepCancelCount >= Timing::MAX_SLEEP_CANCEL_COUNT) {
 ESP_LOGW(TAG, "Force sleep: max cancel count reached (%u)", _sleepCancelCount);
 return true;
 }
 
 if (_currentState == PowerState::WAKE_MODE || _currentState == PowerState::SLEEP_PREP) {
 if (_bleManager != nullptr && !_bleManager->isConnected) {
 unsigned long disconnectedTime = getTimeInState;
 if (disconnectedTime >= Timing::FORCE_SLEEP_TIMEOUT_MS) {
 ESP_LOGW(TAG, "Force sleep: disconnected %lu ms", disconnectedTime);
 return true;
 }
 }
 }
 
 return false;
}

bool PowerManager::incrementSleepCancelCount {
 _sleepCancelCount++;
 ESP_LOGI(TAG, "Sleep cancelled by activity (%u/%u)", 
 _sleepCancelCount, Timing::MAX_SLEEP_CANCEL_COUNT);
 
 return _sleepCancelCount >= Timing::MAX_SLEEP_CANCEL_COUNT;
}


void PowerManager::updateRollingProxAverage(uint16_t proximity) {
 const uint16_t INITIAL_SAMPLE_COUNT = 10;
 const float ALPHA = 0.1f;
 
 if (_rollingProxSampleCount < INITIAL_SAMPLE_COUNT) {
 _rollingProxAverage = (_rollingProxAverage * _rollingProxSampleCount + proximity) 
 / (_rollingProxSampleCount + 1);
 _rollingProxSampleCount++;
 
 ESP_LOGV(TAG, "Rolling prox: initializing %u/%u, avg=%.1f", 
 _rollingProxSampleCount, INITIAL_SAMPLE_COUNT, _rollingProxAverage);
 } else {
 _rollingProxAverage = (ALPHA * proximity) + ((1.0f - ALPHA) * _rollingProxAverage);
 
 ESP_LOGV(TAG, "Rolling prox: avg=%.1f, current=%u", _rollingProxAverage, proximity);
 }
}

uint16_t PowerManager::getDynamicSleepCancelThreshold const {
 uint16_t dynamicThreshold = (uint16_t)(_rollingProxAverage * 
 ProximityConfig::SLEEP_CANCEL_THRESHOLD_MULTIPLIER);
 
 uint16_t floorThreshold = (uint16_t)(ProximityConfig::DETECTION_THRESHOLD_DEFAULT * 
 ProximityConfig::SLEEP_CANCEL_THRESHOLD_MULTIPLIER);
 
 if (dynamicThreshold < floorThreshold) {
 dynamicThreshold = floorThreshold;
 }
 
 ESP_LOGV(TAG, "Dynamic sleep cancel threshold: %u (avg=%.1f, floor=%u)", 
 dynamicThreshold, _rollingProxAverage, floorThreshold);
 
 return dynamicThreshold;
}

void PowerManager::resetRollingProxAverage {
 _rollingProxAverage = 0.0f;
 _rollingProxSampleCount = 0;
 ESP_LOGD(TAG, "Rolling proximity average reset");
}

void PowerManager::clearSensorWakeFlag {
 if (_wokeFromSensor) {
 _wokeFromSensor = false;
 ESP_LOGD(TAG, "Sensor wake flag cleared (external call)");
 }
}


void PowerManager::checkIdleTimeout {
 if (_currentState != PowerState::WAKE_MODE) {
 return;
 }
 
 if (_bleManager && _bleManager->isConnected) {
 return;
 }
 
 unsigned long now = millis;
 
 if (now - _lastSleepCancellationTime < ProximityConfig::SLEEP_REREQUEST_DEBOUNCE_MS) {
 ESP_LOGD(TAG, "Sleep re-request debounced (%lu ms since cancel)",
 now - _lastSleepCancellationTime);
 return;
 }
 
 unsigned long idleTime = now - _lastActivityTime;
 
 unsigned long timeout = Timing::DISCONNECTED_SLEEP_TIMEOUT_MS;
 
 if (_wokeFromSensor) {
 int bondCount = _bleManager ? _bleManager->getBondCount : 0;
 
 if (bondCount > 0) {
 timeout = Timing::SENSOR_WAKE_BONDED_TIMEOUT_MS;
 ESP_LOGD(TAG, "Sensor wake with %d bonds - timeout %lu ms",
 bondCount, timeout);
 } else {
 timeout = Timing::SENSOR_WAKE_QUICK_SLEEP_TIMEOUT_MS;
 ESP_LOGD(TAG, "Sensor wake unbonded - timeout %lu ms",
 timeout);
 }
 }
 
 if (idleTime >= timeout) {
 if (_wokeFromSensor) {
 ESP_LOGI(TAG, "Idle after sensor wake");
 ESP_LOGI(TAG, " Idle time: %lu ms (threshold: %lu ms)", idleTime, timeout);
 ESP_LOGI(TAG, " No valid detection or connection confirmed");
 ESP_LOGI(TAG, " Action: Returning to sleep to conserve battery");
 } else {
 ESP_LOGI(TAG, "Disconnected idle timeout: %lu ms (threshold: %lu ms) - sleeping",
 idleTime, timeout);
 }
 
 requestSleep;
 }
}


void PowerManager::notifyActivity {
 _lastActivityTime = millis;
 ESP_LOGD(TAG, "Activity detected - idle timer reset");
 
 if (_currentState == PowerState::SLEEP_PREP) {
 ESP_LOGW(TAG, "Activity during Sleep Prep - Aborting Sleep!");
 cancelSleep;
 }
}

unsigned long PowerManager::getTimeSinceActivity const {
 if (_lastActivityTime == 0) {
 return 0;
 }
 return millis - _lastActivityTime;
}

void PowerManager::configureWakeSources {
 
 uint64_t wakeupMask = 0;

 if (rtc_gpio_is_valid_gpio(Pins::BUTTON_1)) {
 rtc_gpio_pullup_en(Pins::BUTTON_1);
 rtc_gpio_pulldown_dis(Pins::BUTTON_1);
 wakeupMask |= (1ULL << Pins::BUTTON_1);
 }

 if (rtc_gpio_is_valid_gpio(Pins::PROXIMITY_INT)) {
 rtc_gpio_pullup_en(Pins::PROXIMITY_INT);
 rtc_gpio_pulldown_dis(Pins::PROXIMITY_INT);
 wakeupMask |= (1ULL << Pins::PROXIMITY_INT);
 }

 if (rtc_gpio_is_valid_gpio(Pins::I2C_SDA)) {
 rtc_gpio_pullup_en(Pins::I2C_SDA);
 rtc_gpio_pulldown_dis(Pins::I2C_SDA);
 }
 if (rtc_gpio_is_valid_gpio(Pins::I2C_SCL)) {
 rtc_gpio_pullup_en(Pins::I2C_SCL);
 rtc_gpio_pulldown_dis(Pins::I2C_SCL);
 }

 if (wakeupMask > 0) {
 esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_LOW);
 }
}

bool PowerManager::canSafelyEnterSleep const {
 uint8_t blockReasons = 0;
 
 if (_protocolHandler != nullptr) {
 uint8_t pendingCount = _protocolHandler->getPendingCount;
 if (pendingCount > 0) {
 ESP_LOGD(TAG, "Sleep blocked: %u pending ACKs", pendingCount);
 blockReasons |= 0x01;
 }
 }
 
 if (_otaManager != nullptr && _otaManager->isInProgress) {
 ESP_LOGW(TAG, "Sleep blocked - OTA in progress (BRICK PREVENTION)");
 blockReasons |= 0x02;
 }
 
 if (_bleManager != nullptr) {
 if (_bleManager->isConnected) {
 if (_bleManager->isNotifySubscribed) {
 ESP_LOGD(TAG, "Sleep guard: BLE subscribed - defer for TX complete");
 }
 }
 }
 
 if (_historyService != nullptr && _historyService->hasPendingData) {
 ESP_LOGD(TAG, "Sleep guard: History has %u pending events - will flush", 
 _historyService->getPendingCount);
 }
 
 if (isBatteryCritical) {
 ESP_LOGW(TAG, "Sleep guard: Battery critical - NVS flush may be incomplete");
 }
 
 if (blockReasons != 0) {
 ESP_LOGD(TAG, "Sleep blocked (reasons=0x%02X)", blockReasons);
 return false;
 }
 
 ESP_LOGD(TAG, "Sleep guard: All checks passed - safe to enter deep sleep");
 return true;
}

void PowerManager::enterDeepSleep {
 ESP_LOGI(TAG, "ENTERING DEEP SLEEP");
 
 if (_bleManager != nullptr) {
 _bleManager->incrementGhostBondCounter;
 }

 if (onBeforeSleep) {
 ESP_LOGI(TAG, "Calling onBeforeSleep callback...");
 onBeforeSleep;
 }

 if (_protocolHandler && _bleManager && _bleManager->isConnected) {
 ESP_LOGI(TAG, "Sending Sleep Notification to App...");
 _protocolHandler->sendSleepNotification;
 
 vTaskDelay(pdMS_TO_TICKS(150));
 ESP_LOGD(TAG, "Sleep notification sent, proceeding with shutdown");
 }
 
 ConfigService* configService = ConfigService::getInstance;
 if (configService && configService->isDirty) {
 ESP_LOGI(TAG, "Flushing ConfigService...");
 configService->forceFlush;
 }
 
 if (_historyService) {
 ESP_LOGI(TAG, "Stopping Storage Worker...");
 _historyService->stopStorageWorker;
 }
 
 if (_historyService) {
 if (_historyService->hasPendingData) {
 ESP_LOGI(TAG, "Final NVS sync...");
 _historyService->forceNvsSync;
 }
 }
 
 if (_bleManager) {
 if (_bleManager->isConnected) {
 ESP_LOGI(TAG, "Disconnecting BLE...");
 _bleManager->disconnect;
 delay(200);
 }
 _bleManager->stopAdvertising;
 }
 
 LED_HAL* led = LED_HAL::getInstance;
 if (led) {
 led->setPattern(LEDConfig::COLOR_SLEEP_PREP, BlinkPattern::FAST_BLINK);
 delay(500);
 
 led->prepareForSleep;
 }
 
 Sensor_HAL* sensor = Sensor_HAL::getInstance;
 bool sensorReady = false;
 if (sensor) {
 sensorReady = sensor->configureForDeepSleep;
 }
 
 configureWakeSources;
 
 esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

 
 if (sensorReady) {
 esp_err_t err = esp_sleep_enable_timer_wakeup(Timing::FAILSAFE_WAKE_INTERVAL_US);
 if (err != ESP_OK) {
 ESP_LOGW(TAG, "Failed to enable failsafe timer (err=%d)", err);
 }
 ESP_LOGI(TAG, "Deep Sleep Armed (STANDARD MODE)");
 ESP_LOGI(TAG, " Primary Wake: Sensor (EXT1) + Button");
 ESP_LOGI(TAG, " Failsafe Wake: Timer (2 hours)");
 ESP_LOGI(TAG, " Sensor Status: VERIFIED OK");
 } else {
 esp_err_t err = esp_sleep_enable_timer_wakeup(Timing::EMERGENCY_WAKE_INTERVAL_US);
 if (err != ESP_OK) {
 ESP_LOGW(TAG, "Failed to enable emergency timer (err=%d)", err);
 }
 
 ESP_LOGE(TAG, "Deep Sleep Armed (EMERGENCY MODE)");
 ESP_LOGE(TAG, " CRITICAL: Sensor configuration FAILED!");
 ESP_LOGE(TAG, " Primary Wake: Timer (30 seconds) - NOT sensor!");
 ESP_LOGE(TAG, " Backup Wake: Button (EXT1)");
 ESP_LOGE(TAG, " Action: Device will reboot in 30s to retry init");
 
 if (led) {
 for (int i = 0; i < 3; i++) {
 led->setColor(LEDConfig::COLOR_ERROR);
 delay(100);
 led->setColor(LEDConfig::COLOR_OFF);
 delay(100);
 }
 led->prepareForSleep;
 }
 }
 
 updateBatteryStatus;
 ESP_LOGI(TAG, "Power Status at Sleep Entry");
 ESP_LOGI(TAG, " Battery: %u%%", _batteryPercent);
 ESP_LOGI(TAG, " Charging: %s", _isCharging ? "YES" : "NO");
 ESP_LOGI(TAG, " Voltage: %u mV", _lastAdcReading);
 if (_lastAdcReading < BatteryConfig::EXTERNAL_POWER_THRESHOLD_MV) {
 ESP_LOGI(TAG, " Power Source: USB/External (no battery detected)");
 } else {
 ESP_LOGI(TAG, " Power Source: Battery");
 if (_batteryPercent < 30 && !_isCharging) {
 ESP_LOGW(TAG, " WARNING: Low battery may affect wake reliability!");
 ESP_LOGW(TAG, " The sensor uses 120mA LED + 8T integration for robust wake");
 ESP_LOGW(TAG, " even at low voltage, but charging is recommended.");
 }
 }
 
 ESP_LOGI(TAG, "Going to sleep now...");
 Serial.flush;
 delay(100);
 
 if (sensor) {
 uint16_t deepSleepThreshold = 0; // [REMOVED] Deep sleep threshold formula
 if (deepSleepThreshold < ProximityConfig::MIN_DEEP_SLEEP_THRESHOLD) {
 deepSleepThreshold = ProximityConfig::MIN_DEEP_SLEEP_THRESHOLD;
 } else if (deepSleepThreshold > ProximityConfig::MAX_DEEP_SLEEP_THRESHOLD) {
 deepSleepThreshold = ProximityConfig::MAX_DEEP_SLEEP_THRESHOLD;
 }
 
 if (!sensor->isProximityBelowThreshold(deepSleepThreshold)) {
 ESP_LOGW(TAG, "Proximity elevated!");
 ESP_LOGW(TAG, " Proximity >= threshold %u", deepSleepThreshold);
 ESP_LOGW(TAG, " Edge-triggered interrupt would NOT fire");
 ESP_LOGW(TAG, " Action: ABORTING sleep, returning to WAKE_MODE");
 ESP_LOGW(TAG, " Next: Will retry sleep when proximity drops");
 
 esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
 
 transitionTo(PowerState::WAKE_MODE);
 
 _lastActivityTime = millis;
 
 return;
 }
 
 ESP_LOGI(TAG, "Proximity check passed - safe to sleep");
 }
 
 if (digitalRead(Pins::PROXIMITY_INT) == LOW) {
 ESP_LOGW(TAG, "Sensor active during shutdown!");
 ESP_LOGW(TAG, " GPIO %d reads LOW (sensor triggered)", Pins::PROXIMITY_INT);
 ESP_LOGW(TAG, " Action: ABORTING sleep, returning to DETECTION_ACTIVE");
 
 esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
 
 transitionTo(PowerState::DETECTION_ACTIVE);
 
 _lastActivityTime = millis;
 
 return;
 }
 
 ESP_LOGI(TAG, "Sensor GPIO check passed - proceeding to sleep");
 
 esp_deep_sleep_start;
}


void PowerManager::update {
 if (!_initialized) return;
 
 unsigned long now = millis;
 
 if (now - _lastBatteryCheckTime >= Timing::BATTERY_CHECK_INTERVAL_MS) {
 updateBatteryStatus;
 }
 
 switch (_currentState) {
 case PowerState::WAKE_MODE: {
 LED_HAL* led = LED_HAL::getInstance;
 if (led) {
 
 bool advertising = _bleManager != nullptr && _bleManager->isAdvertising;
 bool pairingMode = _bleManager != nullptr && _bleManager->isPairingModeEnabled;
 
 BlinkPattern pattern = led->getPattern;
 uint32_t color = led->getColor;

 if (advertising && pairingMode) {
 if (pattern != BlinkPattern::SLOW_BLINK || color != LEDConfig::COLOR_PAIRING) {
 led->setPattern(LEDConfig::COLOR_PAIRING, BlinkPattern::SLOW_BLINK);
 }
 } else {
 if (pattern != BlinkPattern::SOLID || color != LEDConfig::COLOR_PAIRING) {
 led->setColor(LEDConfig::COLOR_PAIRING);
 }
 }

 led->update;
 }
 
 checkIdleTimeout;
 break;
 }
 
 case PowerState::ACTIVE: {
 if (_otaManager != nullptr && _otaManager->isInProgress) {
 _lastActivityTime = now;
 ESP_LOGD(TAG, "Process-Lock active (OTA) - idle timer reset");
 break;
 }
 
 if (_historyService != nullptr && _historyService->isFlushing) {
 _lastActivityTime = now;
 ESP_LOGD(TAG, "Process-Lock active (NVS Flush) - idle timer reset");
 break;
 }
 
 if (_lastActivityTime > 0) {
 unsigned long idleTime = now - _lastActivityTime;
 
 if (idleTime >= Timing::CONNECTED_IDLE_TIMEOUT_MS) {
 ESP_LOGI(TAG, "Connected Idle Timeout");
 ESP_LOGI(TAG, " Idle time: %lu ms (threshold: %lu ms)",
 idleTime, Timing::CONNECTED_IDLE_TIMEOUT_MS);
 ESP_LOGI(TAG, " Action: Initiating graceful sleep");
 
 requestSleep;
 }
 }
 break;
 }
 
 case PowerState::SLEEP_PREP: {
 LED_HAL* led = LED_HAL::getInstance;
 if (led) led->update;
 
 bool sleepPrepElapsed = (getSleepPrepRemaining == 0);
 bool forcesSleepReady = shouldForceSleep;
 
 if (sleepPrepElapsed || forcesSleepReady) {
 if (forcesSleepReady) {
 ESP_LOGW(TAG, "FORCE SLEEP triggered - bypassing normal checks");
 }
 
 bool canSleepNow = canSafelyEnterSleep;
 if (canSleepNow || forcesSleepReady) {
 if (forcesSleepReady && !canSleepNow) {
 ESP_LOGW(TAG, "Force sleep with pending operations - data may be lost");
 }
 
 transitionTo(PowerState::DEEP_SLEEP);
 enterDeepSleep;
 } else {
 if (!_sleepDeferred) {
 ESP_LOGI(TAG, "Sleep deferred: pending operations");
 }
 _sleepDeferred = true;
 _sleepPrepStartTime = millis;
 }
 }
 break;
 }
 
 default:
 break;
 }
}

bool PowerManager::checkBrownout {
 if (_brownoutDetected) {
 ESP_LOGW(TAG, "Brownout reset detected - battery may be critically low");
 return true;
 }
 
 if (isBatteryCritical) {
 ESP_LOGW(TAG, "Critical battery level: %u%%", _batteryPercent);
 return true;
 }
 
 return false;
}


void PowerManager::getDiagnostics(char* buffer, size_t bufferSize) const {
 snprintf(buffer, bufferSize,
 "Power: state=%s, bat=%u%%, chrg=%s, wake=%s, inState=%lums",
 getStateName,
 _batteryPercent,
 _isCharging ? "Y" : "N",
 getWakeReasonName,
 getTimeInState);
}
