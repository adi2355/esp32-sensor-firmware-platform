
#include <Arduino.h>
#include "Config.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>

#include "HAL/HAL_Base.h"
#include "HAL/LED_HAL.h"
#include "HAL/Sensor_HAL.h"
#include "HAL/StorageHAL.h"

#include "Services/HistoryService.h"
#include "Services/BLEManager.h"
#include "Services/ProtocolHandler.h"
#include "Services/PowerManager.h"
#include "Services/OTAManager.h"
#include "Services/ConfigService.h"
#include "Services/WifiService.h"
#include "Services/TimeService.h"
#include "Services/FlasherService.h"
#include "Services/BootRecoveryTracker.h"
#include "Services/SessionEventService.h"
#include "Services/DetectionDeliveryPump.h"
#include "Services/DataLoggerService.h"
#include "Services/InferenceService.h"
#include "Services/PersonalizationService.h"
#include "Domain/DetectionEvent.h"
#include "Domain/ProtocolDefs.h"

static const char* TAG = "MAIN";


RTC_NOINIT_ATTR uint32_t g_crash_counter;

RTC_NOINIT_ATTR uint32_t g_rtc_magic;
RTC_NOINIT_ATTR uint32_t g_boot_mode;
RTC_NOINIT_ATTR uint32_t g_stability_counter;

static constexpr uint32_t CRASH_RECOVERY_THRESHOLD = 3;

void systemPanic(const char* message) {
#ifndef DIST_BUILD
 Serial.begin(115200);
 delay(100);
#endif

 ESP_LOGE(TAG, "SYSTEM PANIC - CRITICAL FAILURE");
 ESP_LOGE(TAG, "%s", message);
 ESP_LOGE(TAG, "SYSTEM HALTED - Reflash required");

#ifndef DIST_BUILD
 Serial.flush;
#endif

 esp_err_t wdtErr = esp_task_wdt_add(NULL);
 if (wdtErr != ESP_OK && wdtErr != ESP_ERR_INVALID_STATE) {
 ESP_LOGW(TAG, "Could not add task to TWDT: %s", esp_err_to_name(wdtErr));
 }

 pinMode(Pins::LED_DATA, OUTPUT);
 LED_HAL* led = LED_HAL::getInstance;
 bool ledAvailable = (led && led->init == HalError::OK);

 if (!ledAvailable) {
 pinMode(21, OUTPUT);
 }

 while (true) {
 esp_task_wdt_reset;

 if (ledAvailable) {
 led->setColor(LEDConfig::COLOR_ERROR);
 delay(100);
 led->off;
 delay(100);
 } else {
 digitalWrite(21, LOW);
 delay(100);
 digitalWrite(21, HIGH);
 delay(100);
 }

 yield;
 }
}


HistoryService historyService;
ProtocolHandler protocolHandler;
SessionEventService* sessionEventService = SessionEventService::getInstance;
PowerManager powerManager;
OTAManager otaManager;

DetectionDeliveryPump hitDeliveryPump;

DataLoggerService dataLoggerService;
InferenceService inferenceService;
PersonalizationService personalizationService;

static BootRecoveryTracker& bootRecovery = BootRecoveryTracker::getInstance;


struct DetectionState {
 std::atomic<bool> isDetecting;
 std::atomic<unsigned long> detectionStartTime;
 std::atomic<uint16_t> lastProximityValue;

 std::atomic<bool> isConfirmedDetection;

 std::atomic<bool> requestBackoff;

 uint16_t wakeThreshold;
 uint16_t detectionThreshold;

 uint16_t criticalDetectionThreshold;

 DetectionState
 : isDetecting(false)
 , detectionStartTime(0)
 , lastProximityValue(0)
 , isConfirmedDetection(false)
 , requestBackoff(false)
 , wakeThreshold(ProximityConfig::WAKE_THRESHOLD_DEFAULT)
 , detectionThreshold(ProximityConfig::DETECTION_THRESHOLD_DEFAULT)
 , criticalDetectionThreshold(ProximityConfig::CRITICAL_DETECTION_THRESHOLD_DEFAULT)
 {}
};

DetectionState detectionState;

struct HitDiagnostics {
 uint32_t totalHitStarts;
 uint32_t validHits;
 uint32_t rejectedTooShort;
 uint32_t rejectedNotConfirmed;
 uint32_t rejectedBoth;
 uint32_t stuckHitHeals;
} hitDiag = {};

TaskHandle_t sensorTaskHandle = nullptr;

static std::atomic<bool> g_isWarmupPeriod{true};
static bool g_stuckHitWatchdogActive = false;
static unsigned long g_stuckHitStartTime = 0;

struct TimingState {
 unsigned long lastSensorSampleTime;
 unsigned long lastKeepaliveTime;
 unsigned long lastHeartbeatReceived;
 unsigned long connectionStartTime;
};

TimingState timing = {0};

RTC_DATA_ATTR CalibrationData rtcCalibration;
RTC_DATA_ATTR TimeSyncData rtcTimeSync;

class MainBLEEventHandler : public BLEEventHandler {
public:
 void onBLEConnect(uint16_t connHandle) override {
 ESP_LOGI(TAG, "BLE Connected (handle=%u)", connHandle);

 timing.connectionStartTime = millis;
 timing.lastHeartbeatReceived = millis;

 protocolHandler.resetForNewConnection;

 powerManager.transitionTo(PowerState::ACTIVE);

 historyService.setBleConnected(true);

 }

 void onBLESubscribe(bool subscribed) override {
 if (subscribed) {
 ESP_LOGI(TAG, "Notification subscription confirmed");
 }
 }

 void onBLEDisconnect(uint16_t connHandle, int reason) override {
 ESP_LOGI(TAG, "BLE Disconnected (reason=0x%02X)", reason);


 protocolHandler.clearAllPending;

 historyService.setBleConnected(false);

 if (powerManager.getState != PowerState::SLEEP_PREP) {
 powerManager.transitionTo(PowerState::WAKE_MODE);
 }

 detectionState.isDetecting.store(false, std::memory_order_release);
 detectionState.detectionStartTime.store(0, std::memory_order_relaxed);
 detectionState.isConfirmedDetection.store(false, std::memory_order_release);

 void onBLEBonded(bool success) override {
 ESP_LOGI(TAG, "Bonding %s", success ? "SUCCESS" : "FAILED");
 }

 void onBLEDataReceived(const uint8_t* data, size_t length) override {
 timing.lastHeartbeatReceived = millis;

 protocolHandler.processReceived(data, length);
 }

 void onBLEAuthenticationComplete(bool authenticated) override {
 ESP_LOGI(TAG, "Authentication %s", authenticated ? "complete" : "failed");
 }
};

MainBLEEventHandler bleEventHandler;

bool isThresholdSane(uint16_t wake, uint16_t detection, uint16_t critical);


void onTimeSyncReceived(uint32_t epochSeconds) {
 ESP_LOGI(TAG, "Time sync: epoch=%lu, millis=%lu", epochSeconds, millis);

 rtcTimeSync.syncedEpoch = epochSeconds;
 rtcTimeSync.syncedMillis = millis;
 rtcTimeSync.isValid = 1;
}

void onSleepCommandReceived {
 ESP_LOGI(TAG, "Sleep command received");
 inferenceService.shutdown;
 personalizationService.shutdown;
 dataLoggerService.shutdown;
 powerManager.requestSleep;
}

void onCalibrateRequested(uint16_t* outWake, uint16_t* outDetection) {
 ESP_LOGI(TAG, "Calibration requested (Multi-Level Thresholding)");

 Sensor_HAL* sensor = Sensor_HAL::getInstance;
 if (sensor) {
 uint16_t criticalThreshold = 0;
 HalError err = sensor->calibrate(3000, outWake, outDetection, &criticalThreshold);

 if (err == HalError::OK) {
 if (!isThresholdSane(*outWake, *outDetection, criticalThreshold)) {
 ESP_LOGE(TAG, "CALIBRATION REJECTED!");
 ESP_LOGE(TAG, " Calculated: wake=%u, detection=%u, critical=%u",
 *outWake, *outDetection, criticalThreshold);

 *outWake = ProximityConfig::WAKE_THRESHOLD_DEFAULT;
 *outDetection = ProximityConfig::DETECTION_THRESHOLD_DEFAULT;
 criticalThreshold = ProximityConfig::CRITICAL_DETECTION_THRESHOLD_DEFAULT;

 detectionState.wakeThreshold = *outWake;
 detectionState.detectionThreshold = *outDetection;
 detectionState.criticalDetectionThreshold = criticalThreshold;

 rtcCalibration.wakeThresholdLow = *outWake;
 rtcCalibration.detectionThresholdLow = *outDetection;
 rtcCalibration.criticalDetectionThreshold = criticalThreshold;
 rtcCalibration.crc32 = calculateCRC32(
 reinterpret_cast<uint8_t*>(&rtcCalibration),
 sizeof(CalibrationData) - sizeof(uint32_t)
 );

 ESP_LOGI(TAG, "Safe defaults applied: wake=%u, detection=%u, critical=%u",
 *outWake, *outDetection, criticalThreshold);
 return;
 }

 detectionState.wakeThreshold = *outWake;
 detectionState.detectionThreshold = *outDetection;
 detectionState.criticalDetectionThreshold = criticalThreshold;
 rtcCalibration.wakeThresholdLow = *outWake;
 rtcCalibration.detectionThresholdLow = *outDetection;
 rtcCalibration.criticalDetectionThreshold = criticalThreshold;
 rtcCalibration.crc32 = calculateCRC32(
 reinterpret_cast<uint8_t*>(&rtcCalibration),
 sizeof(CalibrationData) - sizeof(uint32_t)
 );

 {
 float calMean = sensor->getLastCalibrationMean;
 float calSigma = sensor->getLastCalibrationSigma;
 if (calSigma > 0.0f) {
 float dynRange = calMean + 10.0f * calSigma;
 if (dynRange < 500.0f) dynRange = 500.0f;
 personalizationService.updateSensorBaseline(calMean, calSigma, dynRange);
 }
 }

 ESP_LOGI(TAG, "Calibration saved: wake=%u, detection=%u, critical=%u",
 *outWake, *outDetection, criticalThreshold);
 }
 }
}

void handleButtonInput {
 static int lastButtonState = HIGH;
 static unsigned long lastDebounceTime = 0;
 static unsigned long buttonPressStartTime = 0;
 static bool longPressHandled = false;

 int reading = digitalRead(Pins::BUTTON_1);

 if (reading != lastButtonState) {
 lastDebounceTime = millis;
 }

 if ((millis - lastDebounceTime) > Timing::BUTTON_DEBOUNCE_MS) {
 static int buttonState = HIGH;

 if (reading != buttonState) {
 buttonState = reading;

 if (buttonState == LOW) {
 buttonPressStartTime = millis;
 longPressHandled = false;
 ESP_LOGI(TAG, "Button pressed");
 } else {
 if (!longPressHandled) {
 ESP_LOGI(TAG, "Short press detected - toggling pairing mode");
 BLEManager* ble = BLEManager::getInstance;
 if (ble) {
 if (ble->isPairingModeEnabled) {
 ble->disablePairingMode;
 ESP_LOGI(TAG, "Pairing mode manually DISABLED");
 } else {
 ble->enablePairingMode;
 ESP_LOGI(TAG, "Pairing mode manually ENABLED");
 }
 }
 }
 }
 }

 if (buttonState == LOW && !longPressHandled) {
 if (millis - buttonPressStartTime > 3000) {
 longPressHandled = true;
 ESP_LOGW(TAG, "LONG PRESS DETECTED - CLEARING ALL BONDS!");

 LED_HAL* led = LED_HAL::getInstance;
 if (led) {
 led->flash(LEDConfig::COLOR_ERROR, 200);
 led->flash(LEDConfig::COLOR_ACTIVE, 200);
 led->flash(LEDConfig::COLOR_ERROR, 200);
 led->flash(LEDConfig::COLOR_ACTIVE, 200);
 }

 BLEManager* ble = BLEManager::getInstance;
 if (ble) {
 ble->deleteAllBonds;
 ble->disconnect;

 ble->enablePairingMode;
 }

 ESP_LOGI(TAG, "Bonds cleared. Ready for new pairing.");
 }
 }
 }
 lastButtonState = reading;
}

// ============================================================================
// [PROPRIETARY] Detection threshold validation
// Validates multi-level threshold ordering and bounds.
// ============================================================================
bool isThresholdSane(uint16_t wake, uint16_t detection, uint16_t critical) {
 // [PROPRIETARY LOGIC REMOVED]
 // Validates threshold sanity: ordering constraints,
 // min/max bounds for wake, detection, and critical thresholds.
 return true;
}

bool loadCalibrationFromRTC {
 uint32_t crc = calculateCRC32(
 reinterpret_cast<uint8_t*>(&rtcCalibration),
 sizeof(CalibrationData) - sizeof(uint32_t)
 );

 if (crc == rtcCalibration.crc32) {
 if (!isThresholdSane(rtcCalibration.wakeThresholdLow,
 rtcCalibration.detectionThresholdLow,
 rtcCalibration.criticalDetectionThreshold)) {
 ESP_LOGW(TAG, "BAD CALIBRATION DETECTED!");
 ESP_LOGW(TAG, " CRC is valid, but threshold values are INSANE");
 ESP_LOGW(TAG, " Action: RESETTING to safe default thresholds");

 } else {
 detectionState.wakeThreshold = rtcCalibration.wakeThresholdLow;
 detectionState.detectionThreshold = rtcCalibration.detectionThresholdLow;
 detectionState.criticalDetectionThreshold = rtcCalibration.criticalDetectionThreshold;

 ESP_LOGI(TAG, "RTC Calibration valid and sane. Loaded: wake=%u, detection=%u, critical=%u",
 detectionState.wakeThreshold, detectionState.detectionThreshold, detectionState.criticalDetectionThreshold);
 return true;
 }
 } else {
 ESP_LOGW(TAG, "RTC Calibration invalid (CRC mismatch). Using defaults.");
 }

 detectionState.wakeThreshold = ProximityConfig::WAKE_THRESHOLD_DEFAULT;
 detectionState.detectionThreshold = ProximityConfig::DETECTION_THRESHOLD_DEFAULT;
 detectionState.criticalDetectionThreshold = ProximityConfig::CRITICAL_DETECTION_THRESHOLD_DEFAULT;

 ESP_LOGI(TAG, "Using default thresholds: wake=%u, detection=%u, critical=%u",
 detectionState.wakeThreshold, detectionState.detectionThreshold, detectionState.criticalDetectionThreshold);

 return false;
}

// ============================================================================
// [PROPRIETARY] Core detection algorithm
// Multi-level proximity detection with hysteresis, gray-zone watchdog,
// confirmation logic, duration validation, and event logging.
// ============================================================================
void handleDetection(uint16_t proximityValue) {
 // [PROPRIETARY ALGORITHM REMOVED]
 //
 // This function implements the core detection pipeline:
 //   1. Multi-level threshold comparison (wake / detection / critical)
 //   2. Adaptive hysteresis gap calculation for release detection
 //   3. Gray-zone watchdog for stuck signals
 //   4. Critical-level confirmation to filter false positives
 //   5. Duration-based hit validation
 //   6. Offline-capable event logging with BLE live-send
 //   7. Environmental backoff requests on jitter detection
 //   8. Power state transitions (DETECTION_ACTIVE / ACTIVE / WAKE_MODE)
}

void sendKeepalive {
 BLEManager* ble = BLEManager::getInstance;

 if (!ble || !ble->isConnected || !ble->isNotifySubscribed) {
 return;
 }

 unsigned long now = millis;

 if (now - timing.lastKeepaliveTime >= Timing::KEEPALIVE_INTERVAL_MS) {
 timing.lastKeepaliveTime = now;

 protocolHandler.sendMessage(MessageType::MSG_HEARTBEAT, nullptr, 0, false);

 ESP_LOGD(TAG, "Keepalive sent (connection age: %lu s)",
 (now - timing.connectionStartTime) / 1000);
 }
}

void checkHeartbeatTimeout {
 BLEManager* ble = BLEManager::getInstance;

 if (!ble || !ble->isConnected) {
 return;
 }

 if (detectionState.isDetecting.load(std::memory_order_acquire)) {
 return;
 }

 if (ble->getBondCount > 0) {
 return;
 }

 unsigned long now = millis;

 if (now - timing.lastHeartbeatReceived > Timing::HEARTBEAT_TIMEOUT_MS) {
 ESP_LOGW(TAG, "Heartbeat timeout! Last received %lu ms ago",
 now - timing.lastHeartbeatReceived);
 powerManager.requestSleep;
 }
}


bool handleBrownoutRecovery {
 esp_reset_reason_t resetReason = esp_reset_reason;

 if (resetReason != ESP_RST_BROWNOUT) {
 return false;
 }

 ESP_LOGW(TAG, "BROWNOUT RECOVERY DETECTED!");
 ESP_LOGW(TAG, "Battery critically low. Skipping heavy init.");

 LED_HAL* led = LED_HAL::getInstance;
 if (led->init == HalError::OK) {
 led->flash(LEDConfig::COLOR_ERROR, 500);
 led->flash(LEDConfig::COLOR_ERROR, 500);
 led->flash(LEDConfig::COLOR_ERROR, 500);
 }

 historyService.init;

 if (historyService.hasPendingData) {
 ESP_LOGW(TAG, "Saving pending data to NVS before shutdown...");
 historyService.forceNvsSync;
 }

 ESP_LOGW(TAG, "Critical: Battery depleted. Going to deep sleep.");
 ESP_LOGW(TAG, "Charge device before next use.");

 if (led) {
 led->off;
 }

 esp_sleep_enable_ext0_wakeup(Pins::BUTTON_1, 0);

 ESP_LOGW(TAG, "Entering deep sleep (wake on button)...");
 Serial.flush;

 esp_deep_sleep_start;

 return true;
}

void sensorTask(void* pvParameters) {
 ESP_LOGI(TAG, "SensorTask started on core %d", xPortGetCoreID);

 esp_task_wdt_add(nullptr);

 const TickType_t xFrequency = pdMS_TO_TICKS(Timing::SAMPLING_PERIOD_MS);
 TickType_t xLastWakeTime = xTaskGetTickCount;

 Sensor_HAL* sensor = Sensor_HAL::getInstance;
 BLEManager* ble = BLEManager::getInstance;

 unsigned long lastDebugLog = 0;

 unsigned long taskStartTime = millis;

 while (true) {
 vTaskDelayUntil(&xLastWakeTime, xFrequency);

 esp_task_wdt_reset;

 if (sensor == nullptr) {
 continue;
 }

 uint16_t proximity = sensor->readProximity;

 dataLoggerService.appendSample(proximity, Timing::SAMPLING_PERIOD_MS);

 if (g_isWarmupPeriod.load(std::memory_order_relaxed)) {
 unsigned long now = millis;
 if (now - taskStartTime >= 1000) {
 g_isWarmupPeriod.store(false, std::memory_order_release);
 ESP_LOGI(TAG, "Warmup complete (1s). Hit detection ENABLED.");
 } else {
 powerManager.updateRollingProxAverage(proximity);
 continue;
 }
 }

 unsigned long now = millis;
 if (now - lastDebugLog >= 1000) {
 lastDebugLog = now;

 detectionState.detectionThreshold = sensor->getDetectionThreshold;
 detectionState.criticalDetectionThreshold = sensor->getCriticalDetectionThreshold;

 ESP_LOGI(TAG, "prox=%u, state=%s, conn=%d, wake_th=%u, hit_th=%u, crit_th=%u, envBackoff=%d",
 proximity,
 powerManager.getStateName,
 ble ? ble->isConnected : 0,
 detectionState.wakeThreshold,
 detectionState.detectionThreshold,
 detectionState.criticalDetectionThreshold,
 sensor->getEnvironmentBackoff);

 {
 uint8_t mlFlags = 0;
 if (g_isWarmupPeriod.load(std::memory_order_relaxed))
 mlFlags |= WindowHeader::FLAG_WARMUP_ACTIVE;
 if (ble && ble->isConnected)
 mlFlags |= WindowHeader::FLAG_BLE_CONNECTED;
 #if ARDUINO_USB_CDC_ON_BOOT
 mlFlags |= WindowHeader::FLAG_USB_POWERED;
 #endif
 if (sensor->getWakeThreshold != 0)
 mlFlags |= WindowHeader::FLAG_CALIBRATION_VALID;
 dataLoggerService.setRuntimeFlags(mlFlags);
 }
 }

 PowerState state = powerManager.getState;

 if (state == PowerState::WAKE_MODE && ble && ble->isConnected) {
 ESP_LOGI(TAG, "Device connected - transitioning to ACTIVE");
 powerManager.transitionTo(PowerState::ACTIVE);
 state = PowerState::ACTIVE;
 }

 if (state == PowerState::ACTIVE ||
 state == PowerState::DETECTION_ACTIVE ||
 state == PowerState::WAKE_MODE ||
 state == PowerState::SLEEP_PREP) {

 handleDetection(proximity);
 }

 // ========================================================================
 // [PROPRIETARY] Stuck-hit self-healing watchdog
 // Detects prolonged DETECTION_ACTIVE state and performs automatic
 // sensitivity backoff + recalibration to recover from stuck signals.
 // ========================================================================
 if (state == PowerState::DETECTION_ACTIVE) {
 // [PROPRIETARY SELF-HEALING LOGIC REMOVED]
 //
 // This block implements:
 //   1. Stuck-hit timeout detection (watchdog timer)
 //   2. Preemptive gray-zone backoff trigger
 //   3. Automatic environment backoff adjustment
 //   4. Emergency recalibration with threshold reset
 //   5. Calibration persistence to RTC memory
 //   6. State recovery (ACTIVE or WAKE_MODE)
 //   7. LED status restoration
 } else {
 if (g_stuckHitWatchdogActive) {
 g_stuckHitWatchdogActive = false;
 g_stuckHitStartTime = 0;
 }
 detectionState.requestBackoff.store(false, std::memory_order_relaxed);
 }

 if (state == PowerState::WAKE_MODE || state == PowerState::SLEEP_PREP) {
 powerManager.updateRollingProxAverage(proximity);
 }

 if (state == PowerState::SLEEP_PREP) {
 if (powerManager.shouldForceSleep) {
 }
 else {
 uint16_t dynamicSleepCancelThreshold = powerManager.getDynamicSleepCancelThreshold;

 if (proximity > dynamicSleepCancelThreshold) {
 bool maxCancelsReached = powerManager.incrementSleepCancelCount;

 if (maxCancelsReached) {
 ESP_LOGW(TAG, "Max sleep cancels reached - forcing sleep on next attempt");
 } else {
 ESP_LOGI(TAG, "Significant activity during sleep prep (prox=%u > dynamic_threshold=%u) - cancelling",
 proximity, dynamicSleepCancelThreshold);
 powerManager.cancelSleep;
 }
 }
 }
 }
 }

 esp_task_wdt_delete(nullptr);
 vTaskDelete(nullptr);
}

void performCrashRecoveryCheck {
 const esp_partition_t* running = esp_ota_get_running_partition;
 esp_ota_img_states_t otaState;
 if (running && esp_ota_get_state_partition(running, &otaState) == ESP_OK) {
 if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
 ESP_LOGW(TAG, "Booting NEW OTA image (Pending Verify)");
 ESP_LOGW(TAG, " NVS crash recovery DISABLED to protect bonding keys.");
 ESP_LOGW(TAG, " If boot fails, native rollback will revert to old firmware.");
 return;
 }
 }

 esp_reset_reason_t reason = esp_reset_reason;

 ESP_LOGI(TAG, "Reset reason: %d (%s)",
 static_cast<int>(reason),
 reason == ESP_RST_POWERON ? "POWER_ON" :
 reason == ESP_RST_BROWNOUT ? "BROWNOUT" :
 reason == ESP_RST_PANIC ? "PANIC" :
 reason == ESP_RST_INT_WDT ? "INTERRUPT_WDT" :
 reason == ESP_RST_TASK_WDT ? "TASK_WDT" :
 reason == ESP_RST_WDT ? "OTHER_WDT" :
 reason == ESP_RST_SW ? "SOFTWARE" :
 reason == ESP_RST_DEEPSLEEP ? "DEEP_SLEEP" :
 "OTHER");

 if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
 g_crash_counter = 0;
 ESP_LOGI(TAG, "Cold boot detected. Crash counter initialized to 0.");
 return;
 }


 bool wasCrashReset = (reason == ESP_RST_PANIC ||
 reason == ESP_RST_INT_WDT ||
 reason == ESP_RST_TASK_WDT ||
 reason == ESP_RST_WDT);

 if (wasCrashReset) {
 g_crash_counter++;
 ESP_LOGW(TAG, "CRASH DETECTED! Consecutive crashes: %lu", g_crash_counter);
 } else {
 ESP_LOGI(TAG, "Normal reset. Current crash counter: %lu", g_crash_counter);
 }


 if (g_crash_counter >= CRASH_RECOVERY_THRESHOLD) {
 ESP_LOGE(TAG, "CRITICAL: BOOT LOOP DETECTED (%lu crashes)", g_crash_counter);
 ESP_LOGE(TAG, "Initiating EMERGENCY NVS ERASE...");


 LED_HAL* led = LED_HAL::getInstance;
 if (led != nullptr && led->init == HalError::OK) {
 for (int i = 0; i < 10; i++) {
 led->flash(LEDConfig::COLOR_ERROR, 50);
 delay(50);
 }
 }


 esp_err_t err = nvs_flash_erase;

 if (err == ESP_OK) {
 ESP_LOGI(TAG, "EMERGENCY NVS ERASE SUCCESSFUL");
 ESP_LOGW(TAG, "All bonds, settings, and history have been reset.");
 ESP_LOGI(TAG, "Device will now attempt normal boot...");

 g_crash_counter = 0;

 if (led != nullptr) {
 led->flash(LEDConfig::COLOR_HIT, 500);
 }
 } else {
 ESP_LOGE(TAG, "EMERGENCY NVS ERASE FAILED: %s (0x%x)",
 esp_err_to_name(err), err);
 ESP_LOGE(TAG, "Flash memory may be damaged. Device may continue crashing.");

 }

 Serial.flush;
 }
}

void setup {

#ifndef DIST_BUILD
 Serial.begin(115200);
 Serial.setTxTimeoutMs(0);

 unsigned long serialWaitStart = millis;
 while (!Serial && (millis - serialWaitStart < 3000)) {
 delay(10);
 }

 delay(500);

 Serial.println;
 Serial.println("================================================================");
 Serial.println("=== BOOT LOGS ===");
 Serial.println("================================================================");
 Serial.flush;
#endif

 ESP_LOGI(TAG, "ESP32-S3 FIRMWARE v%d.%d.%d",
 FIRMWARE_VERSION_MAJOR,
 FIRMWARE_VERSION_MINOR,
 FIRMWARE_VERSION_PATCH);
 ESP_LOGI(TAG, "Build Type: %s",
 FIRMWARE_BUILD_TYPE == 0 ? "Release" :
 FIRMWARE_BUILD_TYPE == 1 ? "Beta" : "Debug");

 performCrashRecoveryCheck;

 if (!bootRecovery.init) {
 ESP_LOGW(TAG, "Boot recovery persistence unavailable - power-cycle recovery may be unreliable after full power loss.");
 }

 {
 ESP_LOGI(TAG, "Checking Flasher Mode entry conditions...");

 if (g_rtc_magic != BootConfig::RTC_MAGIC_KEY) {
 ESP_LOGI(TAG, "RTC magic invalid (0x%08lX). Initializing boot state.", g_rtc_magic);
 g_rtc_magic = BootConfig::RTC_MAGIC_KEY;
 g_boot_mode = BootConfig::MODE_NORMAL;
 g_stability_counter = 0;

 if (bootRecovery.isAvailable) {
 g_stability_counter = bootRecovery.getCounter;
 ESP_LOGI(TAG, "Boot recovery counter restored from NVS: %lu", g_stability_counter);
 } else {
 ESP_LOGW(TAG, "Boot recovery counter reset (RTC-only mode)");
 }
 }

 esp_reset_reason_t reason = esp_reset_reason;

 if (reason == ESP_RST_POWERON || reason == ESP_RST_SW) {
 if (bootRecovery.isAvailable) {
 g_stability_counter = bootRecovery.incrementIfEligible(reason);
 } else {
 g_stability_counter++;
 }
 ESP_LOGI(TAG, "Stability counter incremented: %lu (reason=%d)",
 g_stability_counter, static_cast<int>(reason));
 } else if (reason == ESP_RST_BROWNOUT) {
 ESP_LOGW(TAG, "Brownout reset - stability counter unchanged: %lu", g_stability_counter);
 }

 bool enterFlasher = false;
 const char* entryReason = "";

 if (g_boot_mode == BootConfig::MODE_FLASHER) {
 enterFlasher = true;
 entryReason = "App requested (MSG_ENTER_OTA_MODE)";
 g_boot_mode = BootConfig::MODE_NORMAL;
 }

 if (g_stability_counter >= BootConfig::RECOVERY_TRIGGER_COUNT) {
 enterFlasher = true;
 entryReason = "Recovery trigger (power-cycle pattern)";
 g_stability_counter = 0;
 bootRecovery.resetCounter;
 }

 if (enterFlasher) {
 ESP_LOGW(TAG, "ENTERING FLASHER MODE");
 ESP_LOGW(TAG, " Reason: %s", entryReason);
 Serial.flush;

 bootRecovery.resetCounter;

 FlasherService::run;

 ESP.restart;
 }

 ESP_LOGI(TAG, "Normal boot (stability_counter=%lu)", g_stability_counter);
 }

 if (handleBrownoutRecovery) {
 }

 Serial.flush;


 ESP_LOGI(TAG, "Phase 0: Storage Validation (Fail Fast)");
 Serial.flush;

 HAL::StorageHAL* storage = HAL::StorageHAL::getInstance;

 if (!storage->validatePartitions) {
 char errorMsg[128];
 snprintf(errorMsg, sizeof(errorMsg),
 "Partition Error: %s",
 HAL::StorageHAL::getErrorString(storage->getLastError));
 systemPanic(errorMsg);
 }

 HalError nvsErr = storage->initNvs;
 if (nvsErr != HalError::OK) {
 systemPanic("NVS initialization failed - cannot proceed");
 }

 if (storage->wasNvsRecovered) {
 ESP_LOGW(TAG, "WARNING: NVS was corrupted and has been reset.");
 ESP_LOGW(TAG, "All bonds and settings were cleared.");
 }


 ESP_LOGI(TAG, "Phase 1: HAL Initialization");

 pinMode(Pins::BUTTON_1, INPUT_PULLUP);

 LED_HAL* led = LED_HAL::getInstance;
 if (led->init != HalError::OK) {
 ESP_LOGE(TAG, "LED HAL init failed!");
 }
 led->setColor(LEDConfig::COLOR_BOOT);

 Sensor_HAL* sensor = Sensor_HAL::getInstance;
 bool sensorInitOk = (sensor->init == HalError::OK);

 if (!sensorInitOk) {
 ESP_LOGE(TAG, "Sensor HAL init failed - I2C may be unavailable");
 led->setColor(LEDConfig::COLOR_ERROR);
 } else {
 uint16_t sensorId = sensor->getSensorId;
 if ((sensorId & 0x00FF) != 0x80) {
 char errorMsg[64];
 snprintf(errorMsg, sizeof(errorMsg),
 "SENSOR ID MISMATCH: Expected 0x80, Got 0x%02X",
 sensorId & 0xFF);
 systemPanic(errorMsg);
 }
 ESP_LOGI(TAG, "Sensor ID verified: 0x%04X", sensorId);
 }

 ESP_LOGI(TAG, "Phase 1.5: OTA Firmware Validation");
 otaManager.validateBootedFirmware;

 ESP_LOGI(TAG, "Initializing services...");

 historyService.init;

 ESP_LOGI(TAG, "Initializing ConfigService...");
 ConfigService* configService = ConfigService::getInstance;
 if (!configService->init(sensor)) {
 ESP_LOGW(TAG, "ConfigService init failed - using default sensitivity");
 } else {
 ESP_LOGI(TAG, "ConfigService initialized: sensitivity=%u",
 configService->getSensitivity);
 }


 if (loadCalibrationFromRTC) {
 ESP_LOGI(TAG, "FAST BOOT - Valid calibration in RTC");

 sensor->setBaseThresholds(
 rtcCalibration.wakeThresholdLow,
 rtcCalibration.detectionThresholdLow,
 rtcCalibration.criticalDetectionThreshold
 );

 detectionState.wakeThreshold = sensor->getWakeThreshold;
 detectionState.detectionThreshold = sensor->getDetectionThreshold;
 detectionState.criticalDetectionThreshold = sensor->getCriticalDetectionThreshold;

 ESP_LOGI(TAG, " Base: wake=%u, detection=%u, critical=%u",
 rtcCalibration.wakeThresholdLow,
 rtcCalibration.detectionThresholdLow,
 rtcCalibration.criticalDetectionThreshold);
 ESP_LOGI(TAG, " Effective: wake=%u, detection=%u, critical=%u",
 detectionState.wakeThreshold, detectionState.detectionThreshold, detectionState.criticalDetectionThreshold);
 } else {
 // ====================================================================
 // [PROPRIETARY] Dark calibration sequence
 // Performs baseline measurement after power rail stabilization
 // and computes multi-level thresholds using statistical methods.
 // ====================================================================
 ESP_LOGI(TAG, "COLD BOOT - Running Dark Calibration");

 // [PROPRIETARY CALIBRATION SEQUENCE REMOVED]
 //
 // This block performs:
 //   1. LED off + power rail stabilization delay
 //   2. Statistical baseline measurement via sensor->calibrate()
 //   3. Multi-level threshold computation (wake / detection / critical)
 //   4. Threshold persistence to RTC memory with CRC32
 //   5. Personalization baseline update from calibration statistics

 ESP_LOGI(TAG, "Calibration phase complete (details redacted)");

 if (led) {
 led->setColor(LEDConfig::COLOR_BOOT);
 }
 }
 ESP_LOGI(TAG, "Calibration phase complete");

 BLEManager* ble = BLEManager::getInstance;
 if (!ble->init(&historyService)) {
 ESP_LOGE(TAG, "BLE Manager init failed!");
 led->setColor(LEDConfig::COLOR_ERROR);
 while (1) delay(1000);
 }
 ble->setEventHandler(&bleEventHandler);

 if (!protocolHandler.init(ble, &historyService)) {
 ESP_LOGE(TAG, "Protocol Handler init failed!");
 }

 protocolHandler.onTimeSyncReceived = onTimeSyncReceived;
 protocolHandler.onSleepCommandReceived = onSleepCommandReceived;
 protocolHandler.onCalibrateRequested = onCalibrateRequested;

 protocolHandler.onGetBatteryStatus = [](uint8_t& percent, bool& isCharging) {
 percent = powerManager.getBatteryPercent;
 isCharging = powerManager.isCharging;
 };

 protocolHandler.onActivityDetected = [] {
 powerManager.notifyActivity;
 };

 if (!otaManager.init(ble, &protocolHandler)) {
 ESP_LOGE(TAG, "OTA Manager init failed!");
 }

 protocolHandler.setOtaManager(&otaManager);

 if (!hitDeliveryPump.init(ble, &historyService, &protocolHandler)) {
 ESP_LOGW(TAG, "DetectionDeliveryPump init failed - delayed sync fix inactive");
 }

 powerManager.init(ble, &historyService, &protocolHandler);
 sessionEventService->setPowerManager(&powerManager);

 powerManager.setOTAManager(&otaManager);

 powerManager.onBeforeSleep = [] {
 inferenceService.shutdown;
 personalizationService.shutdown;
 dataLoggerService.shutdown;
 };

 ESP_LOGI(TAG, "Initializing Wi-Fi & Time Services (One-Shot Policy)...");

 WifiService* wifi = WifiService::getInstance;
 wifi->init;

 TimeService* timeSvc = TimeService::getInstance;
 timeSvc->init;

 wifi->setConnectionCallback([](bool connected) {
 if (connected) {
 ESP_LOGI(TAG, "[WIFI] Connected - Starting NTP sync...");
 TimeService::getInstance->startSync;
 } else {
 ESP_LOGI(TAG, "[WIFI] Disconnected");
 }
 });

 wifi->setStatusCallback([](uint8_t state, uint8_t reason, int8_t rssi, uint32_t ip) {
 protocolHandler.sendWifiStatus(state, reason, rssi, ip);
 });

 timeSvc->setTimeSyncCallback([](uint32_t epoch) {
 ESP_LOGI(TAG, "[NTP] TIME SYNC COMPLETE - Backfilling timestamps");
 ESP_LOGI(TAG, " Epoch: %lu", epoch);
 ESP_LOGI(TAG, " Source: Tier 1 (Wi-Fi/NTP)");

 extern HistoryService historyService;
 historyService.onSystemTimeSynced(epoch);

 });

 ESP_LOGI(TAG, "Evaluating boot sync policy...");
 timeSvc->evaluateBootSyncPolicy;

 ESP_LOGI(TAG, "Wi-Fi & Time Services initialized");
 ESP_LOGI(TAG, " Wi-Fi State: %s", wifi->getStateName);
 ESP_LOGI(TAG, " Time State: %s", timeSvc->getStateName);
 ESP_LOGI(TAG, " Credentials: %s", wifi->hasCredentials ? "YES" : "NO");
 ESP_LOGI(TAG, " One-Shot Active: %s", timeSvc->isOneShotSyncActive ? "YES" : "NO");

 ESP_LOGI(TAG, "Initializing ML services...");

 if (!dataLoggerService.init) {
 ESP_LOGW(TAG, "DataLoggerService init failed - ML data collection disabled");
 }

 if (!personalizationService.init) {
 ESP_LOGW(TAG, "PersonalizationService init failed - using global model only");
 }

 if (!inferenceService.init(&dataLoggerService, &personalizationService)) {
 ESP_LOGW(TAG, "InferenceService init failed - ML inference disabled");
 }

 dataLoggerService.setBootCount(static_cast<uint8_t>(historyService.getBootCount & 0xFF));

 {
 Sensor_HAL* sensorHal = Sensor_HAL::getInstance;
 if (sensorHal) {
 float calMean = sensorHal->getLastCalibrationMean;
 float calSigma = sensorHal->getLastCalibrationSigma;
 if (calSigma > 0.0f) {
 float dynRange = calMean + 10.0f * calSigma;
 if (dynRange < 500.0f) dynRange = 500.0f;
 personalizationService.updateSensorBaseline(calMean, calSigma, dynRange);
 }
 }
 }

 ESP_LOGI(TAG, "ML services initialized (DataLogger=%s, Inference=%s, Personalization=%s)",
 dataLoggerService.getStateName,
 inferenceService.isRunning ? "RUNNING" : "DISABLED",
 personalizationService.isCalibrated ? "CALIBRATED" : "UNCALIBRATED");

 powerManager.transitionTo(PowerState::WAKE_MODE);

 timing.lastSensorSampleTime = millis;
 timing.lastHeartbeatReceived = millis;

 ESP_LOGI(TAG, "Launching Sensor Task...");

 BaseType_t taskResult = xTaskCreatePinnedToCore(
 sensorTask,
 "SensorTask",
 FreeRTOSConfig::SENSOR_TASK_STACK_SIZE,
 nullptr,
 FreeRTOSConfig::SENSOR_TASK_PRIORITY,
 &sensorTaskHandle,
 FreeRTOSConfig::APP_CORE
 );

 if (taskResult != pdPASS) {
 ESP_LOGE(TAG, "CRITICAL: Failed to create SensorTask! result=%d", taskResult);
 } else {
 ESP_LOGI(TAG, "SensorTask created: handle=%p, priority=%d, core=%d",
 sensorTaskHandle,
 FreeRTOSConfig::SENSOR_TASK_PRIORITY,
 FreeRTOSConfig::APP_CORE);
 }

 char diagBuffer[128];

 storage->getDiagnostics(diagBuffer, sizeof(diagBuffer));
 ESP_LOGI(TAG, "%s", diagBuffer);

 storage->getPartitionInfo(diagBuffer, sizeof(diagBuffer));
 ESP_LOGI(TAG, "%s", diagBuffer);

 historyService.getDiagnostics(diagBuffer, sizeof(diagBuffer));
 ESP_LOGI(TAG, "%s", diagBuffer);

 ble->getDiagnostics(diagBuffer, sizeof(diagBuffer));
 ESP_LOGI(TAG, "%s", diagBuffer);

 powerManager.getDiagnostics(diagBuffer, sizeof(diagBuffer));
 ESP_LOGI(TAG, "%s", diagBuffer);

 g_crash_counter = 0;

 ESP_LOGI(TAG, "System Ready (- One-Shot Wi-Fi Policy)");
 ESP_LOGI(TAG, " Boot Status: SUCCESS (crash counter reset)");
 ESP_LOGI(TAG, " Architecture: FreeRTOS Task Separation");
 ESP_LOGI(TAG, " Core 0: NimBLE/BLE Controller (automatic)");
 ESP_LOGI(TAG, " Core 1: SensorTask (prio 10), ProtocolTask (prio 1)");
 ESP_LOGI(TAG, " Core 1: StorageWorker (prio 2) - async NVS");
 ESP_LOGI(TAG, " Sensor: %s (handle=%p)",
 sensorTaskHandle ? "RUNNING" : "FALLBACK",
 sensorTaskHandle);

#ifndef DIST_BUILD
 Serial.flush;
 Serial.println("[MAIN] === SETUP COMPLETE - ENTERING MAIN LOOP ===");
 Serial.flush;
#endif
}


#ifndef DIST_BUILD
void processSerialMLCommand(const String& line) {
 if (line.startsWith("ML_START ")) {
 String labelStr = line.substring(9);
 labelStr.trim;
 GestureLabel label = stringToGestureLabel(labelStr.c_str);
 if (label == GestureLabel::INVALID) {
 Serial.printf("ERROR: Unknown label '%s'\n", labelStr.c_str);
 return;
 }
 if (dataLoggerService.startSession(label)) {
 Serial.printf("OK: Session started (label=%s, id=%u)\n",
 labelStr.c_str, dataLoggerService.getCurrentSessionId);
 } else {
 Serial.printf("ERROR: Failed to start session (state=%s)\n",
 dataLoggerService.getStateName);
 }
 } else if (line == "ML_STOP") {
 if (dataLoggerService.stopSession) {
 Serial.printf("OK: Session stopped (%u windows written)\n",
 dataLoggerService.getCurrentSessionWindowCount);
 } else {
 Serial.printf("ERROR: No active session\n");
 }
 } else if (line == "ML_STATUS") {
 dataLoggerService.printStatus;
 inferenceService.printStatus;
 Serial.printf("\n=== Hit Diagnostics ===\n");
 Serial.printf(" Starts: %lu\n", hitDiag.totalHitStarts);
 Serial.printf(" Valid: %lu\n", hitDiag.validHits);
 Serial.printf(" Rej Short: %lu\n", hitDiag.rejectedTooShort);
 Serial.printf(" Rej Unconfirmed: %lu\n", hitDiag.rejectedNotConfirmed);
 Serial.printf(" Rej Both: %lu\n", hitDiag.rejectedBoth);
 Serial.printf(" Stuck Heal: %lu\n", hitDiag.stuckHitHeals);
 Serial.printf("=======================\n\n");
 } else if (line == "ML_DELETE") {
 if (dataLoggerService.deleteAll) {
 Serial.printf("OK: All ML data deleted\n");
 } else {
 Serial.printf("ERROR: Delete failed\n");
 }
 }
 else if (line == "ML_PERSONALIZE_START") {
 if (personalizationService.startCalibration) {
 Serial.printf("OK: Personalization capture mode started\n");
 Serial.printf(" Use: ML_PERSONALIZE_CAPTURE <label> to capture windows\n");
 } else {
 Serial.printf("ERROR: Failed to start calibration\n");
 }
 } else if (line.startsWith("ML_PERSONALIZE_CAPTURE ")) {
 String labelStr = line.substring(22);
 labelStr.trim;
 GestureLabel label = stringToGestureLabel(labelStr.c_str);
 if (label == GestureLabel::INVALID) {
 Serial.printf("ERROR: Unknown label '%s'\n", labelStr.c_str);
 } else {
 SampleWindow window;
 if (dataLoggerService.getCurrentWindow(&window)) {
 float features[InferenceService::NUM_FEATURES];
 float mean, sigma, dynRange;
 if (personalizationService.getBaselineParams(mean, sigma, dynRange) && dynRange > 0.0f) {
 InferenceService::extractFeaturesNormalized(
 window.samples, window.header.sampleCount, features, dynRange);
 } else {
 InferenceService::extractFeatures(
 window.samples, window.header.sampleCount, features);
 }
 if (personalizationService.captureWindow(features, label)) {
 Serial.printf("OK: Captured window for %s\n", labelStr.c_str);
 } else {
 Serial.printf("ERROR: Capture failed (not in calibration mode or at max)\n");
 }
 } else {
 Serial.printf("ERROR: No window available (wait for sensor data)\n");
 }
 }
 } else if (line == "ML_PERSONALIZE_COMMIT") {
 if (personalizationService.commitCalibration) {
 Serial.printf("OK: Calibration committed (will persist to NVS)\n");
 } else {
 Serial.printf("ERROR: Commit failed (not enough samples or not calibrating)\n");
 }
 } else if (line == "ML_PERSONALIZE_RESET") {
 if (personalizationService.resetCalibration) {
 Serial.printf("OK: Personalization reset to defaults\n");
 } else {
 Serial.printf("ERROR: Reset failed\n");
 }
 } else if (line == "ML_PERSONALIZE_STATUS") {
 personalizationService.printStatus;
 } else {
 Serial.printf("ERROR: Unknown ML command. Available:\n");
 Serial.printf(" ML_START <label> - Start recording session\n");
 Serial.printf(" ML_STOP - Stop recording session\n");
 Serial.printf(" ML_STATUS - Print diagnostics\n");
 Serial.printf(" ML_DELETE - Delete all ML data\n");
 Serial.printf(" ML_PERSONALIZE_START - Enter calibration mode\n");
 Serial.printf(" ML_PERSONALIZE_CAPTURE <label> - Capture window for class\n");
 Serial.printf(" ML_PERSONALIZE_COMMIT - Finalize calibration\n");
 Serial.printf(" ML_PERSONALIZE_RESET - Reset personalization\n");
 Serial.printf(" ML_PERSONALIZE_STATUS - Show calibration status\n");
 }
}
#endif

void loop {
 unsigned long now = millis;

 if (g_stability_counter > 0 && now > BootConfig::STABILITY_WINDOW_MS) {
 ESP_LOGI(TAG, "System stable for %lums - resetting stability counter",
 BootConfig::STABILITY_WINDOW_MS);
 g_stability_counter = 0;
 bootRecovery.resetCounter;
 }

 BLEManager* ble = BLEManager::getInstance;
 if (ble) {
 ble->update;
 }

 protocolHandler.processRetries;

 hitDeliveryPump.update;

 powerManager.update;
 sessionEventService->update;

 otaManager.update;

 WifiService::getInstance->update;
 TimeService::getInstance->update;

 if (ble && ble->isConnected) {
 sendKeepalive;
 }

 if (powerManager.getState == PowerState::ACTIVE) {
 checkHeartbeatTimeout;
 }


 static unsigned long lastLedUpdate = 0;
 if (now - lastLedUpdate >= 60) {
 lastLedUpdate = now;

 LED_HAL* led = LED_HAL::getInstance;
 if (led) {
 led->update;
 }
 }

 handleButtonInput;

#ifndef DIST_BUILD
 if (Serial.available) {
 static String serialMLBuffer;
 while (Serial.available) {
 char c = (char)Serial.read;
 if (c == '\n' || c == '\r') {
 if (serialMLBuffer.length > 0) {
 if (serialMLBuffer.startsWith("ML_")) {
 processSerialMLCommand(serialMLBuffer);
 }
 serialMLBuffer = "";
 }
 } else if (serialMLBuffer.length < 64) {
 serialMLBuffer += c;
 }
 }
 }
#endif

 vTaskDelay(pdMS_TO_TICKS(FreeRTOSConfig::PROTOCOL_TASK_YIELD_MS));
}
