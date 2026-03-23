# Power State Machine

**Purpose:** This document defines the firmware's explicit power state machine that governs active operation, safe sleep entry, deep sleep wake-up, battery-aware transition policy, and anti-bricking recovery behavior. It serves as the authoritative explanation of the system's power behavior, safety rules, and recovery design, grounded in the device's unattended, battery-powered operation profile.

---

## 1. Design Goals

The power management subsystem is engineered to meet stringent requirements for a battery-powered, unattended wearable device. Its core design goals are:

* **Battery Longevity:** Achieve months-long deep sleep operation while remaining responsive to user interaction.
* **Deterministic Transitions:** Ensure predictable and explicit transitions between all power states.
* **Safe Sleep Entry:** Guarantee all critical data is persisted and peripherals are gracefully shut down before entering deep sleep.
* **Reliable Wake-Up:** Provide robust and recoverable mechanisms for waking from deep sleep, even under adverse conditions.
* **Anti-Bricking Recoverability:** Implement failsafe measures to prevent the device from entering a permanent, unrecoverable sleep state.
* **Graceful Connected Idle Behavior:** Manage power consumption effectively when connected but idle, using device-initiated sleep notifications to the app.
* **Environmental Adaptation:** Dynamically adjust sleep cancellation thresholds based on ambient sensor readings to prevent false wake-ups.
* **Brownout Tolerance:** Handle unexpected power loss (brownouts) gracefully, prioritizing data integrity and immediate sleep.

## 2. Scope and Non-Goals

This document focuses on the architectural and operational aspects of the power state machine.

**Covered Areas:**
* Power state definitions and transitions.
* The ordered sequence of operations for deep sleep entry and wake-up.
* Mechanisms for battery monitoring, low-power gating, and brownout handling.
* Integration with sensor hardware for wake detection and low-power configuration.
* Coordination with other firmware services for safe shutdown and data integrity.
* App-side interaction for graceful connection management during power state changes.
* Mitigation strategies for identified critical power-related failure modes.

**Excluded Areas (Non-Goals):**
* Detailed battery chemistry modeling or charger IC specifications.
* Comprehensive sensor algorithm tuning beyond its role in wake detection and activity monitoring.
* The full BLE stack lifecycle beyond connection/disconnection management relevant to power states.
 (Refer to `ble-protocol.md` for full BLE details).
* Detailed Wi-Fi provisioning setup beyond its policy-driven activation for time synchronization.
* Low-level hardware register maps or specific, volatile numerical tuning constants (which are deferred to `Config.h` or inline code comments).

---

## 3. System Context

The `PowerManager` service acts as the central orchestrator of the device's power state. It is initialized early in the boot sequence (`main.cpp`) and continuously monitors various inputs and system states to make autonomous power decisions.

**Key Dependencies and Interactions:**
* **`Sensor_HAL`:** Provides raw proximity readings for activity detection and is configured by `PowerManager` for low-power, interrupt-driven wake-up.
* **`LED_HAL`:** Receives commands from `PowerManager` to provide visual feedback for different power states and handles critical GPIO isolation (`prepareForSleep`) before deep sleep.
* **`HistoryService` & `ConfigService`:** These services are queried by `PowerManager` (`isFlushing`, `isDirty`) to ensure all critical data (event logs, user settings) is persisted to non-volatile storage before deep sleep. `PowerManager` triggers their `forceFlush` methods during shutdown.
* **`BLEManager`:** Informs `PowerManager` about BLE connection status (`isConnected`, `getBondCount`) and `GhostBond` failures. `PowerManager` may command `BLEManager` to disconnect.
* **`ProtocolHandler`:** Receives an `onActivityDetected` callback from `PowerManager` for meaningful app interactions and is commanded by `PowerManager` to send `MSG_SLEEP` notifications to the app before deep sleep.
* **`OTAManager`:** `PowerManager` queries `OTAManager::isInProgress` to implement a "process-lock" that prevents deep sleep during critical firmware update operations.
* **`TimeService` / `WifiService`:** `PowerManager` integrates with the "One-Shot" Wi-Fi policy, ensuring Wi-Fi is only activated for NTP time sync when appropriate (e.g., cold boot) and shut down afterward.
* **`main.cpp`:** Orchestrates the initial `PowerManager` setup, handles boot-time crash recovery (`g_crash_counter`) and Flasher Mode entry (`g_stability_counter`), and contains the `sensorTask` which directly updates `PowerManager`'s state based on sensor events.

---

## 4. Power State Model

The device operates within a well-defined power state machine to manage energy consumption while ensuring responsiveness and reliability. The core states, defined by the `PowerState` enum in `PowerManager.h`, govern the device's behavior.

**States Definition:**

* **`BOOT`**: The initial state upon power-on or reset. Hardware initialization, crash recovery checks (`main.cpp::performCrashRecoveryCheck`), and boot mode decisions (e.g., Flasher Mode via `g_boot_mode`) occur here.
* **`WAKE_MODE`**: The device is fully awake but not actively connected to a mobile app. It's typically advertising for connections (if `BLEManager::isAdvertising` is true or `BLEManager::isPairingModeEnabled` is true), or waiting for user interaction. Hit detection is active for offline logging. Power consumption is moderate.
* **`ACTIVE`**: The device is connected to a mobile app via BLE and is actively monitoring for detections. Communication is bidirectional. Power consumption is higher than `WAKE_MODE` due to active BLE connection, but is reduced by automatic light sleep (modem sleep) via `esp_pm_configure`.
* **`DETECTION_ACTIVE`**: A user interaction is currently being detected. The LED illuminates green. All idle timers are suspended. This is a transient, high-activity state.
* **`SLEEP_PREP`**: The device has become idle (either connected or disconnected) and is preparing to enter deep sleep. It performs final checks (`PowerManager::canSafelyEnterSleep`), flushes data, and signals its intent to the app. The LED blinks red. This is a crucial transitional state with multiple guards and cancellation points.
* **`DEEP_SLEEP`**: The device is in its lowest power state. The CPU and most peripherals are powered off. It waits for a configured wake-up source (sensor, button, or failsafe timer) to return to `WAKE_MODE`.
* **`ERROR` (Implicit):** A fatal, non-recoverable error has occurred (e.g., `NVS` initialization failure, partition error). The system halts execution via `systemPanic` (`main.cpp`), providing visual LED feedback, and typically requires a reflash or manual intervention.
* **`FLASHER_MODE_ACTIVE` (Implicit):** A special boot mode initiated by app command (`MSG_ENTER_OTA_MODE`) or power-cycle recovery (`g_stability_counter`). The device runs an isolated HTTP server over Wi-Fi (`FlasherService::run`), bypassing the main application, solely for firmware updates. It reboots on completion or timeout.

---

## 5. Transition Rules

Movement between power states is governed by explicit triggers and critical safety guards, ensuring deterministic behavior and preventing unsafe conditions. The primary logic resides within `PowerManager::update` and `PowerManager::transitionTo`, with sensor-driven events handled in `main.cpp`'s `sensorTask`.

| From State | Trigger | Guard / Condition | Side Effects | Next State | Rationale / ode |
| :-------------- | :-------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `BOOT` | `main.cpp` `setup` completes initialization | `NVS` initialized, `HAL`s ready, `OTA` firmware validated | `LED` to `WAKE_MODE` color (magenta). `_lastActivityTime` reset. `_sleepCancelCount = 0`. `_wokeFromSensor` flag set if `WakeReason::SENSOR`. | `WAKE_MODE` | Initial operational state. |
| `WAKE_MODE` | `BLEManager::isConnected` becomes true | - | `LED` to `ACTIVE` color (blue). `_lastActivityTime` reset. `_sleepCancelCount = 0`. `_wokeFromSensor = false`. | `ACTIVE` | User connected; transition to higher-power, responsive state. |
| `WAKE_MODE` | `PowerManager::checkIdleTimeout` fires (`PowerManager::update`) | `Device not connected` AND `Idle Time >= configured timeout` (dynamic, `SENSOR_WAKE_QUICK_SLEEP_TIMEOUT_MS` or `SENSOR_WAKE_BONDED_TIMEOUT_MS`). | `LED` to `SLEEP_PREP` color (red blink). `_sleepPrepStartTime` reset. | `SLEEP_PREP` |  Device idle after wake. Prevents battery drain from phantom wakes or user picking up/putting down device without connecting.  Centralized sleep decision. |
| `ACTIVE` | `BLEManager::isConnected` becomes false | - | `LED` to `WAKE_MODE` color (magenta, blink if advertising). `_lastActivityTime` reset. `_sleepCancelCount = 0`. `ProtocolHandler::clearAllPending`. `HistoryService::setBleConnected(false)`. | `WAKE_MODE` | Connection lost (range, app killed, firmware error). Return to discovery state. |
| `ACTIVE` | `PowerManager::checkIdleTimeout` (internal to `PowerManager::update`) | `Device connected` AND `TimeSinceActivity >= CONNECTED_IDLE_TIMEOUT_MS` AND `!OTAManager::isInProgress` AND `!HistoryService::isFlushing` | `LED` to `SLEEP_PREP` color (red blink). `_sleepPrepStartTime` reset. `ProtocolHandler::sendSleepNotification` (graceful disconnect handshake). | `SLEEP_PREP` |  Connected but idle. Saves battery during long periods of inactivity.  Process-lock by OTA/NVS. |
| `ACTIVE` | `main.cpp::handleDetection` detects hit_start | `Proximity > currentDetectionThreshold` AND `!detectionState.isDetecting` | `LED` to `DETECTION_ACTIVE` color (green). `detectionState.isDetecting = true`. `PowerManager::notifyActivity` (`_lastActivityTime` reset). `_wokeFromSensor = false`. Increment `_consecutiveDetections`. | `DETECTION_ACTIVE` | User interaction detected. |
| `DETECTION_ACTIVE` | `main.cpp::handleDetection` detects detection_end (`Proximity < releaseThreshold`) | `detectionState.isDetecting` | `detectionState.isDetecting = false`. `LED` to `ACTIVE` (if connected) or `WAKE_MODE` (if disconnected). `PowerManager::notifyActivity` (`_lastActivityTime` reset). Validate and log detection. | `ACTIVE` or `WAKE_MODE` | Detection finished. Return to monitoring state. |
| `DETECTION_ACTIVE` | `main.cpp::sensorTask` detects `MAX_DETECTION_DURATION_MS` timeout OR `` (Gray Zone Watchdog) OR `` (60s watchdog) | `detectionState.isDetecting` AND (`millis - detectionStartTime > MAX_DETECTION_DURATION_MS` OR `detectionState.requestBackoff.load` from Gray Zone Watchdog) | `LED` to `OFF` then `WAKE_MODE/ACTIVE` color. `detectionState.isDetecting = false`. `Sensor_HAL::applyEnvironmentBackoff` / `recalibrate`. `PowerManager::notifyActivity` (`_lastActivityTime` reset). Reset `_consecutiveDetections`. | `WAKE_MODE` or `ACTIVE` |  Device signal stuck (e.g., glass reflection). Forces recalibration/backoff to self-heal. |
| `SLEEP_PREP` | `millis - _sleepPrepStartTime >= SLEEP_PREP_TIME_MS` OR `PowerManager::shouldForceSleep` is true | `PowerManager::canSafelyEnterSleep` (all services quiescent) | `PowerManager::enterDeepSleep` initiates hardware shutdown. | `DEEP_SLEEP` | Graceful transition to low-power state. `shouldForceSleep` handles `` (max cancel count, disconnected timeout). `canSafelyEnterSleep` handles `` (sleep guard). |
| `SLEEP_PREP` | `main.cpp::sensorTask` detects activity | `Proximity > PowerManager::getDynamicSleepCancelThreshold` AND `(millis - _lastSleepCancellationTime > SLEEP_REREQUEST_DEBOUNCE_MS)` AND `!PowerManager::shouldForceSleep` | Increment `_sleepCancelCount`. Reset `_lastActivityTime`. `LED` to `ACTIVE` (if connected) or `WAKE_MODE` (if disconnected). | `ACTIVE` or `WAKE_MODE` |  User activity or genuine sensor event cancels sleep. `getDynamicSleepCancelThreshold` adapts to ambient. `_lastSleepCancellationTime` debounces re-request. |
| `SLEEP_PREP` | `PowerManager::enterDeepSleep` `Sensor_HAL::isProximityBelowThreshold` check fails | `Current proximity >= deep sleep threshold` | `LED` to `WAKE_MODE` color. `PowerManager::notifyActivity` (`_lastActivityTime` reset). Disable timer wakeup. | `WAKE_MODE` |  Prevents sleeping with elevated proximity, which would prevent edge-triggered sensor wake. |
| `SLEEP_PREP` | `PowerManager::enterDeepSleep` final `digitalRead(Pins::PROXIMITY_INT)` fails | `Sensor GPIO reads LOW` (active interaction) | `LED` to `DETECTION_ACTIVE` color. `PowerManager::notifyActivity` (`_lastActivityTime` reset). Disable timer wakeup. | `DETECTION_ACTIVE` |  Last-second user interaction during shutdown. Aborts sleep to handle the interaction. |
| `DEEP_SLEEP` | `ESP_SLEEP_WAKEUP_EXT1` (Sensor or Button) | - | `main.cpp` detects `WakeReason::SENSOR` or `WakeReason::BUTTON`. `_wokeFromSensor = true` if sensor. `LED` to `WAKE_MODE` color (magenta). | `WAKE_MODE` | User interaction or sensor activity triggers wake. |
| `DEEP_SLEEP` | `ESP_SLEEP_WAKEUP_TIMER` | - | `main.cpp` detects `WakeReason::TIMER`. `_wokeFromSensor = false`. `LED` to `WAKE_MODE` color (magenta). | `WAKE_MODE` |  Failsafe timer wake (2hr/30s). Prevents permanent sleep if sensor or button wake fails. |
| `ANY` | `main.cpp` detects `ESP_RST_BROWNOUT` | - | `main.cpp::handleBrownoutRecovery` attempts NVS flush, enters deep sleep. | `DEEP_SLEEP` |  Graceful shutdown on critical power loss. |
| `ANY` | `main.cpp` detects `g_crash_counter >= CRASH_RECOVERY_THRESHOLD` | - | `main.cpp::performCrashRecoveryCheck` triggers emergency NVS erase, then reboots. | `BOOT` |  Breaks NVS corruption boot loops. |

---

## 6. Sleep Entry Sequence

The transition from an active state to deep sleep is a carefully orchestrated, multi-stage shutdown process managed by `PowerManager::enterDeepSleep`. This sequence prioritizes data integrity, graceful communication with the mobile app, and the establishment of robust wake-up paths.

```svg
sequenceDiagram
 participant P as PowerManager
 participant App as Mobile App
 participant H as HistoryService
 participant C as ConfigService
 participant B as BLEManager
 participant L as LED_HAL
 participant S as Sensor_HAL
 participant ML as ML Services

 P->>P: requestSleep<br/>(transitionTo(SLEEP_PREP))
 loop Monitoring SLEEP_PREP
 P->>P: checkSleepPrepRemaining
 alt Sleep Prep Timeout / Force Sleep Triggered
 P->>P: canSafelyEnterSleep
 alt Sleep Guard Pass OR Force Sleep
 P->>P: transitionTo(DEEP_SLEEP)
 P->>P: enterDeepSleep
 P->>ML: onBeforeSleep callback<br/>(shutdown ML tasks, unmount SPIFFS)
 P->>App: sendSleepNotification<br/>(MSG_SLEEP via ProtocolHandler)
 App->>P: (ACK implicit/ignored)
 P->>P: delay(150ms) (allow ACK TX)
 P->>C: forceFlush<br/>(persist pending settings to NVS)
 P->>H: stopStorageWorker<br/>(drain NVS queue, terminate task)
 P->>H: forceNvsSync<br/>(final NVS flush of history)
 P->>B: disconnect<br/>(graceful BLE disconnect)
 P->>B: stopAdvertising
 P->>L: prepareForSleep<br/>(send black, GPIO hold)
 P->>S: configureForDeepSleep<br/>(low-power sensor, verify setup)
 alt Sensor Config OK
 P->>P: Configure EXT1 (Sensor+Button)<br/>Enable Failsafe Timer (2 hours)
 else Sensor Config FAILED
 P->>P: Configure EXT1 (Button only)<br/>Enable Emergency Timer (30 seconds)
 L->>L: Flash RED (hardware issue)
 end
 P->>S: isProximityBelowThreshold<br/>(final check before sleep)
 alt Proximity Elevated
 P->>P: Abort Sleep (return to WAKE_MODE)
 else Proximity OK
 P->>S: digitalRead(PROXIMITY_INT)<br/>(last-second tap check)
 alt Sensor GPIO active
 P->>P: Abort Sleep (return to DETECTION_ACTIVE)
 else Sensor GPIO idle
 P->>P: esp_deep_sleep_start
 P--x App: (Device powers off)
 end
 end
 else Sleep Guard Fail
 P->>P: deferSleep<br/>(reset _sleepPrepStartTime)
 end
 else Activity Detected
 P->>P: cancelSleep<br/>(return to ACTIVE/WAKE_MODE)
 end
 end
```

**Ordered Shutdown Sequence (`PowerManager::enterDeepSleep`):**

1. **Notify App (`MSG_SLEEP`):** `PowerManager` instructs `ProtocolHandler` to send `MSG_SLEEP` to the connected mobile app. This is a graceful handshake, informing the app that the upcoming BLE disconnect is intentional due to idle timeout. This also sets `BluetoothHandler::isSleepDisconnect` (app-side JS) and `DeviceBLECore::deviceSignaledSleep` (native iOS) to prepare for graceful handling of the disconnect.
2. **External Services Shutdown:** The `onBeforeSleep` callback (`PowerManager::onBeforeSleep`) is invoked, allowing other services (e.g., ML InferenceTask, DataLoggerService) to gracefully shut down their FreeRTOS tasks and unmount filesystems (SPIFFS).
3. **Config & History Flush:** `ConfigService::forceFlush` is called to ensure any pending user settings are written to NVS. `HistoryService::stopStorageWorker` is called to drain any queued NVS write operations and terminate the storage task, followed by a final `HistoryService::forceNvsSync` for critical data integrity.
4. **BLE Disconnect:** `BLEManager::disconnect` is called to gracefully terminate the BLE connection, and `stopAdvertising` is called to cease broadcasting.
5. **LED Prepare & Hold:** `LED_HAL::prepareForSleep` is executed. For the WS2812B NeoPixel, this sends a "black" (off) command and enables GPIO hold (`gpio_hold_en`) on the LED pin, preventing unintended illumination during deep sleep.
6. **Low-Power Sensor Configuration & Verification:** `Sensor_HAL::configureForDeepSleep` is called to switch the proximity sensor to a low-power, interrupt-driven mode. This function includes internal verification steps to confirm the sensor is correctly configured for wake-up.
7. **Pre-sleep Proximity Check:** A final check (`Sensor_HAL::isProximityBelowThreshold`) is performed using the newly configured deep sleep threshold. If the current proximity reading is elevated, sleep is aborted to prevent the sensor from failing to trigger a wake-up.
8. **Configure Wake Sources:** `PowerManager::configureWakeSources` sets up `ESP_SLEEP_WAKEUP_EXT1` to monitor both the proximity sensor (GPIO2) and the user button (GPIO0) for physical wake-up events. RTC pull-ups are enabled for I2C pins (GPIO5, GPIO6) to prevent floating during deep sleep.
9. **Arm Failsafe Timer:** A crucial `esp_sleep_enable_timer_wakeup` is called. If `Sensor_HAL::configureForDeepSleep` *verified* the sensor, a 2-hour failsafe timer (`FAILSAFE_WAKE_INTERVAL_US`) is set. If the sensor *failed* verification, a 30-second emergency timer (`EMERGENCY_WAKE_INTERVAL_US`) is set, ensuring recovery.
10. **Last-Second GPIO Check:** Just before `esp_deep_sleep_start`, the `PROXIMITY_INT` GPIO pin is checked (`digitalRead(Pins::PROXIMITY_INT)`). If found to be active (LOW), sleep is aborted to handle a rapid user interaction.
11. **Enter Deep Sleep:** `esp_deep_sleep_start` is called, halting CPU execution and entering the lowest power state. The device will remain in this state until a configured wake-up source triggers.

---

## 7. Wake-Source Policy

The device implements a robust multi-layered wake-up policy to ensure reliable recovery from deep sleep, preventing permanent "bricking" scenarios.

| Wake Source | Trigger Condition | Enabled In | Guards / Rationale |
| :---------------------- | :----------------------------------------------------------- | :----------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Proximity Sensor** | `Proximity > configured_deep_sleep_threshold` (falling edge) | `DEEP_SLEEP` (Primary, if verified) | - Configured by `Sensor_HAL::configureForDeepSleep` with robust settings (120mA LED, 8T integration) and verified. |
| `(EXT1 - GPIO2)` | | | - `Sensor_HAL::isProximityBelowThreshold` check *before* sleep prevents wake failure if proximity is already elevated. |
| | | | - `digitalRead(Pins::PROXIMITY_INT)` check *before* `esp_deep_sleep_start` prevents ignoring last-second user taps. |
| **User Button** | `GPIO_NUM_0` goes LOW (falling edge) | `DEEP_SLEEP` (Primary / Backup) | - Always enabled via `ESP_SLEEP_WAKEUP_EXT1` for user-initiated wake. |
| `(EXT1 - GPIO0)` | | | - Acts as a crucial backup even when the sensor is the primary wake source. |
| **Failsafe Timer** | `Timer interval expires` | `DEEP_SLEEP` (Backup / Emergency) | - **CRITICAL:** `` mitigation. Prevents permanent sleep if sensor/button fails or if `Sensor_HAL::configureForDeepSleep` fails verification. |
| `(ESP32 Internal RTC)` | | - **STANDARD:** 2-hour interval (`FAILSAFE_WAKE_INTERVAL_US`) if sensor config `OK`. | - Device wakes, re-initializes all hardware (including potentially reset sensor), and attempts to return to sleep cleanly if no activity. This provides a self-healing opportunity. |
| | | - **EMERGENCY:** 30-second interval (`EMERGENCY_WAKE_INTERVAL_US`) if sensor config `FAILED`. | - Alerts user to a hardware issue via LED flashes and ensures rapid re-initialization attempts. |

---

## 8. Battery and Safety Policy

Power management decisions are deeply intertwined with battery health and system safety. The firmware implements calibrated battery monitoring and integrates battery status into critical operational guards.

* **Calibrated Battery Monitoring (`PowerManager::updateBatteryStatus`):**
 * **Accurate Voltage Reading:** Uses `analogReadMilliVolts(Pins::BAT_ADC)` with 16x oversampling and 2:1 voltage divider compensation for precise millivolt readings.
 * **OCV-SoC Lookup:** Converts calibrated voltage to percentage using a Li-ion Open Circuit Voltage (OCV) to State of Charge (SoC) lookup table, providing a non-linear, accurate discharge curve.
 * **Smoothing & Hysteresis:** Applies Exponential Moving Average (EMA) smoothing and hysteresis (`SMOOTHING_ALPHA`, `HYSTERESIS_PERCENT`) to prevent UI jitter and oscillation at percentage boundaries.
 * **External Power Heuristic:** Detects if the device is running on USB/external power (voltage < `EXTERNAL_POWER_THRESHOLD_MV`), overriding charging status and forcing 100% battery display.
* **Charging Detection:** The `Pins::CHARGING` GPIO (GPIO3) is actively monitored after `gpio_reset_pin` to correctly reflect the charging IC's status (Active LOW = charging).
* **Low/Critical Battery Gating:**
 * **NVS Write Protection:** `HistoryService::flushToNvs` (and implicitly `ConfigService::forceFlush`) will **skip NVS writes** if `PowerManager::isBatteryCritical` is true and not charging. Data remains in RTC memory, prioritizing system stability over permanent persistence at risk of brownout.
 * **OTA Power Gating:** `FlasherService` implements a critical 2-stage power-on self-test before starting OTA firmware updates:
 1. **Idle Check:** Verifies battery voltage is above `MIN_VOLTAGE_IDLE_MV` *before* Wi-Fi activation.
 2. **Load Check & Sag Detection:** Verifies battery voltage remains above `MIN_VOLTAGE_LOAD_MV` *after* Wi-Fi activates (high current draw), and that voltage sag is below `MAX_VOLTAGE_SAG_MV`. If checks fail, OTA is aborted, and the device reboots.
* **Brownout Recovery (`main.cpp::handleBrownoutRecovery`):**
 * On `ESP_RST_BROWNOUT`, the device performs a minimal initialization sequence: flashes red LED, then immediately attempts a final NVS flush (`HistoryService::forceNvsSync`) to preserve data, and enters deep sleep with only button wake enabled. This prevents re-entering a brownout boot loop (``, ``).
* **Wi-Fi Policy & Power Savings (`TimeService::evaluateBootSyncPolicy`):**
 * **"One-Shot" Wi-Fi:** Wi-Fi is actively disabled by `TimeService` (via `WifiService::shutdown`) if not needed for NTP sync (e.g., on Deep Sleep wake or brownout) or after sync completes. This policy significantly reduces battery drain by keeping the Wi-Fi radio off.

---

## 9. Failure Modes and Recovery

The power management subsystem's design has been heavily influenced by numerous identified failure modes, each addressed with specific mitigation strategies to enhance system reliability and user experience.

| Failure Mode | Problem / Risk | Mitigation Strategy | Key Implementation Anchor |
| :----------------------------------------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Permanent Sleep (Brick)** | Primary wake source (sensor) fails or is misconfigured before deep sleep, leaving device stuck indefinitely without recovery. | **Failsafe Timer Wakeup:** Always enables `ESP_SLEEP_WAKEUP_TIMER`. Sets to 2 hours (`FAILSAFE_WAKE_INTERVAL_US`) if `Sensor_HAL::configureForDeepSleep` succeeds, or 30 seconds (`EMERGENCY_WAKE_INTERVAL_US`) if sensor config fails. <br>**Sensor Config Verification:** `Sensor_HAL::configureForDeepSleep` verifies settings by reading back registers. | `PowerManager::enterDeepSleep` <br>`Sensor_HAL::configureForDeepSleep` <br>`FAILSAFE_WAKE_INTERVAL_US`, `EMERGENCY_WAKE_INTERVAL_US` |
| **Phantom Wakes** | Sensor noise, fabric movement, or EMI trigger false wake-ups, leading to unnecessary battery drain. | **Quick Sleep Policy:** If wake is from sensor (`_wokeFromSensor = true`) and no valid detection or BLE connection within `SENSOR_WAKE_QUICK_SLEEP_TIMEOUT_MS` (30s unbonded, 45s bonded), the device returns to deep sleep. <br>**Dynamic Thresholding:** `PowerManager::updateRollingProxAverage` adapts sleep cancellation threshold based on ambient readings. | `PowerManager::checkIdleTimeout` <br>`PowerManager::updateRollingProxAverage` <br>`SENSOR_WAKE_QUICK_SLEEP_TIMEOUT_MS`, `SENSOR_WAKE_BONDED_TIMEOUT_MS` |
| **Elevated Proximity Blocking Sleep** | Device attempts to sleep while proximity is already above threshold, preventing the edge-triggered sensor interrupt from firing upon re-entry. | **Pre-sleep Proximity Check:** `PowerManager::enterDeepSleep` calls `Sensor_HAL::isProximityBelowThreshold` *before* `esp_deep_sleep_start`. If proximity is elevated, sleep is aborted, and the device returns to `WAKE_MODE`. | `PowerManager::enterDeepSleep` <br>`Sensor_HAL::isProximityBelowThreshold` <br>`MIN/MAX_DEEP_SLEEP_THRESHOLD` |
| **Sleep Loop (Activity Cancelling Sleep)** | Persistent sensor noise or environmental reflections repeatedly cancel sleep preparation, keeping the device awake indefinitely and draining battery. | **Dynamic Sleep Cancel Threshold:** `PowerManager::getDynamicSleepCancelThreshold` adapts dynamically to the current ambient level (`_rollingProxAverage`). <br>**Sleep Re-request Debounce:** `_lastSleepCancellationTime` debounces successive sleep re-requests. <br>**Force Sleep Mechanism:** `PowerManager::shouldForceSleep` (`_sleepCancelCount >= MAX_SLEEP_CANCEL_COUNT` OR `disconnected > FORCE_SLEEP_TIMEOUT_MS`) eventually forces deep sleep. | `main.cpp::sensorTask` <br>`PowerManager::getDynamicSleepCancelThreshold` <br>`PowerManager::checkIdleTimeout` <br>`MAX_SLEEP_CANCEL_COUNT`, `FORCE_SLEEP_TIMEOUT_MS`, `SLEEP_REREQUEST_DEBOUNCE_MS` |
| **Rapid Interaction During Shutdown** | User taps the sensor during the final milliseconds of `SLEEP_PREP`, causing the interrupt to be ignored or a sleep-wake-sleep glitch. | **Last-Second GPIO Check:** `PowerManager::enterDeepSleep` performs a final `digitalRead(Pins::PROXIMITY_INT)` check just before `esp_deep_sleep_start`. If the sensor GPIO is active (LOW), sleep is aborted, and the device returns to `DETECTION_ACTIVE`. | `PowerManager::enterDeepSleep` <br>`digitalRead(Pins::PROXIMITY_INT)` |
| **Low Battery NVS Corruption Loop** | `NVS` flush operations, requiring current spikes, occur at critically low battery levels, causing a brownout, leading to a reboot loop and potential data corruption. | **NVS Write Gating:** `HistoryService::flushToNvs` (and `ConfigService::forceFlush`) skips `NVS` writes if `PowerManager::isBatteryCritical` and not charging. Data remains in RTC RAM. | `HistoryService::flushToNvs` <br>`PowerManager::isBatteryCritical` <br>`CRITICAL_BATTERY_PERCENT` |
| **Brownout Recovery** | The device experiences an unexpected power loss or reset due to critically low battery voltage. | **Graceful Brownout Handling:** `main.cpp::handleBrownoutRecovery` detects `ESP_RST_BROWNOUT`, performs minimal initialization, attempts a final `NVS` flush, and enters deep sleep with only button wake enabled. | `main.cpp::handleBrownoutRecovery` |
| **Orphaned BLE Bonds** | The mobile app "forgets" the device, but the ESP32 retains a stale bond record. Subsequent connection attempts fail due to key mismatch, making the device appear unresponsive or unpairable. | **Consecutive Bond Failure Counter:** `BLEManager::incrementGhostBondCounter` tracks consecutive unencrypted connection attempts (`_deviceConnected` = true but no auth complete) when bonds exist. After 5 failures, the *next boot* clears bonds and enables emergency pairing mode. | `BLEManager::incrementGhostBondCounter` <br>`main.cpp::setup` (checks counter) |
| **False Bonding Error During Sleep Disconnect (Cross-System)** | When ESP32 enters deep sleep, iOS may report `CBError.peerRemovedPairingInformation` (a false positive bonding error) due to the abrupt BLE radio power-off. | **Sleep Signal Priority:** Firmware sends `MSG_SLEEP` (`ProtocolHandler::sendSleepNotification`). App-side `BluetoothHandler::isSleepDisconnect` and native `DeviceBLECore::deviceSignaledSleep` flags prioritize this signal over `CBError` codes, classifying as `.deviceSleep` for graceful handling. | `ProtocolHandler::sendSleepNotification` <br>`BluetoothHandler::setDeviceSleepFlag` (app-side) <br>`DeviceBLECore::classifyDisconnectReason` (native iOS) <br>`BluetoothHandler::handleBondingError` (guard) |
| **OTA Process-Lock (Sleep during OTA)** | The device attempts to enter deep sleep during an active OTA firmware update, risking firmware corruption and device bricking. | **Process-Lock Guard:** `PowerManager::canSafelyEnterSleep` checks `OTAManager::isInProgress` and `HistoryService::isFlushing`. If an OTA update or `NVS` flush is active, deep sleep is blocked/deferred. | `PowerManager::canSafelyEnterSleep` <br>`OTAManager::isInProgress` <br>`HistoryService::isFlushing` |
| **Deep Sleep Wi-Fi Prevention** | The Wi-Fi radio remains active during deep sleep or after a failed sync attempt, significantly draining the battery. | **"One-Shot" Wi-Fi Policy:** `TimeService::evaluateBootSyncPolicy` enables Wi-Fi *only* on cold boot if needed for NTP sync. `TimeService::handleSyncSuccess`/`handleSyncTimeout` *shuts down* the Wi-Fi radio immediately after sync (or timeout). | `TimeService::evaluateBootSyncPolicy` <br>`TimeService::handleSyncSuccess`/`handleSyncTimeout` <br>`WifiService::shutdown` |
| **NVS Corruption Boot Loop** | Power loss during an `NVS` write corrupts the partition, causing `nvs_flash_init` to hang, triggering an Interrupt Watchdog (IWDT) reboot loop. | **RTC Crash Counter & Preemptive Erase:** `main.cpp::performCrashRecoveryCheck` tracks consecutive `WDT` resets (`g_crash_counter`). If the count is `>= CRASH_RECOVERY_THRESHOLD`, `NVS` is *erased* before `nvs_flash_init` is attempted. | `main.cpp::performCrashRecoveryCheck` <br>`g_crash_counter` (RTC_NOINIT_ATTR) <br>`nvs_flash_erase` |

---

## 10. Cross-Subsystem Interactions

The power management subsystem is not an isolated component but integrates tightly with various other services and HAL layers, forming a cohesive and robust system.

* **`Sensor_HAL`:**
 * `PowerManager` configures `Sensor_HAL` for low-power, interrupt-driven wake-up via `configureForDeepSleep`.
 * `Sensor_HAL` provides raw proximity data to `main.cpp::sensorTask`, which updates `PowerManager`'s state (`DETECTION_ACTIVE`) and `updateRollingProxAverage` for adaptive sleep cancellation.
 * `Sensor_HAL` performs the `isProximityBelowThreshold` check before `PowerManager` commits to deep sleep.
 * The return value of `Sensor_HAL::configureForDeepSleep` is critical for `PowerManager`'s decision to arm the appropriate failsafe timer.
* **`LED_HAL`:**
 * `PowerManager` dictates the LED's visual feedback (`setColor`, `setPattern`) for each power state (e.g., magenta for `WAKE_MODE`, blue for `ACTIVE`, red for `SLEEP_PREP`).
 * `PowerManager` triggers `LED_HAL::prepareForSleep` for the critical GPIO hold operation before deep sleep, ensuring the LED is off and its pin is stable.
* **`HistoryService` & `ConfigService`:**
 * `PowerManager` queries `HistoryService::isFlushing` and `ConfigService::isDirty` as part of `canSafelyEnterSleep` to defer deep sleep if pending NVS writes are active.
 * `PowerManager` explicitly calls `HistoryService::stopStorageWorker` and `forceNvsSync` (and `ConfigService::forceFlush`) during sleep entry to ensure all pending data is safely persisted.
 * `HistoryService::flushToNvs` contains a guard to skip NVS writes if `PowerManager::isBatteryCritical`.
* **`BLEManager`:**
 * `PowerManager` relies on `BLEManager::isConnected`, `isAdvertising`, `getBondCount`, and `isPairingModeEnabled` to inform state transitions and idle timeout calculations.
 * `PowerManager` calls `BLEManager::disconnect` and `stopAdvertising` during deep sleep entry.
 * `PowerManager` triggers `BLEManager::incrementGhostBondCounter` before deep sleep if a bonded device failed to establish an encrypted connection.
* **`ProtocolHandler`:**
 * `ProtocolHandler` uses its `onActivityDetected` callback to notify `PowerManager` of meaningful app interactions (e.g., config changes), resetting the idle timers.
 * `PowerManager` instructs `ProtocolHandler` to send `MSG_SLEEP` notification (`sendSleepNotification`) to the app before deep sleep.
 * `PowerManager` queries `ProtocolHandler::isAwaitingAck` as a sleep guard (`canSafelyEnterSleep`).
* **`OTAManager`:**
 * `PowerManager` queries `OTAManager::isInProgress` as a "process-lock" in `canSafelyEnterSleep` to prevent deep sleep during critical OTA updates.
* **`TimeService` / `WifiService` (the firmware, the firmware):**
 * `PowerManager`'s sleep policy (`TimeService::evaluateBootSyncPolicy`) coordinates with `TimeService` to decide when to activate Wi-Fi for NTP sync (only on cold boot, not deep sleep wake). This enables the "One-Shot" Wi-Fi policy for power saving.
* **Mobile App (`BluetoothHandler`/`DeviceBLECore` - the app, the app):**
 * **Graceful Disconnect Handshake:** The firmware sends `MSG_SLEEP` (`ProtocolHandler::sendSleepNotification`). On the app side, this sets `BluetoothHandler::isSleepDisconnect` (JS) and `DeviceBLECore::deviceSignaledSleep` (native iOS). This hint allows `DeviceBLECore::classifyDisconnectReason` to correctly identify the disconnect as `.deviceSleep` (overriding potential false `CBError` bonding codes) and `BluetoothHandler` to avoid error alerts and immediately attempt auto-reconnection.

---

## 11. Invariants

The following invariants represent critical safety and reliability guarantees maintained by the power management subsystem:

1. **Guaranteed Wake Path:** The device must never enter `DEEP_SLEEP` without at least one *verified and recoverable* wake source actively configured (sensor, button, or failsafe timer).
2. **Low-Power State Achieved:** All major peripherals (BLE radio, sensors, LEDs) must be configured to their lowest power state or powered off before entering `DEEP_SLEEP`.
3. **GPIO State Hold:** Critical GPIO pins (LED data line, I2C pull-ups, wake pins) must be explicitly held in a stable, low-power state during `DEEP_SLEEP`.
4. **Data Integrity on Sleep:** All critical user and system data (event history, user configuration) must be either safely in non-volatile storage (NVS) or RTC memory *before* entering `DEEP_SLEEP`.
5. **Monotonic Time Across Sleep:** System uptime must maintain a monotonically increasing timestamp that persists across `DEEP_SLEEP` cycles, enabling accurate offline event timestamping.
6. **Idle vs. Fault Distinction:** The system must be able to reliably distinguish between intentional idle-induced sleep, disconnect-induced sleep, and unexpected fault recovery (e.g., brownout, crash).
7. **OTA Protection:** The device must never enter `DEEP_SLEEP` while an OTA firmware update or critical NVS operation is in progress.
8. **Graceful App Disconnect:** For device-initiated sleep, the system must signal its intent to the connected app (`MSG_SLEEP`) to enable graceful app-side disconnect handling and auto-reconnection.
9. **Battery-Aware Operations:** High-draw operations (e.g., NVS writes, Wi-Fi activation, OTA update) must be guarded or deferred if the battery is critically low or shows excessive voltage sag.

---

## 12. Related Documents

* `architecture.md`: Overall firmware architecture and design principles.
* `failure-modes.md`: Detailed catalog of edge cases and mitigation strategies.
* `storage-integrity.md`: NVS and RTC memory usage, data persistence, and integrity checks.
* `ota-recovery.md`: Over-The-Air (OTA) update mechanism and recovery.
* `ble-protocol.md`: Bluetooth Low Energy (BLE) communication protocol and app interaction.
