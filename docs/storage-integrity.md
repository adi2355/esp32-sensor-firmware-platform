
# Storage Integrity

> Design of the firmware persistence model and the mechanisms that preserve data integrity across deep sleep, power interruption, corruption, and recovery on theESP32-S3 device.

## 1. Overview

This document details the architectural design, implementation, and rationale behind thefirmware's state persistence mechanisms. It focuses on how data integrity, durability, and graceful recoverability are ensured across various operational states (active, deep sleep) and failure conditions (power loss, flash corruption, system crashes).

The firmware employs a robust, multi-tiered persistence model, leveraging the unique characteristics of different storage media on the ESP32-S3: **RTC Memory**, **NVS Flash**, and **SPIFFS**. This intentional design prioritizes reliability and data survival, which are critical for a production-grade, low-power, BLE-connected embedded device supporting OTA updates and offline data logging.

**Scope:** This document is strictly confined to the *firmware's* storage model. While the overall system includes mobile app and backend persistence, those layers are outside this scope and are addressed in their respective documentation.

## 2. Design Goals and Guarantees

The firmware's storage subsystem is engineered to uphold the following core principles and guarantees:

* **Event Continuity Across Deep Sleep:** Ensure monotonically increasing detection events (`DetectionEvent`) maintain their relative temporal order and uptime-based timestamps (`HAL::getSystemUptime`) across multiple deep sleep cycles, enabling accurate offline data logging (Offline Detection Logging).
* **Bounded Durability for Critical State:** Guarantee that essential user configuration (`UserConfig`), historical data (`DetectionEvent` ring buffer), ML calibration parameters (`MLCalibrationData`), and BLE bonding keys persist across unexpected power loss and cold boots. This protects against (RTC Memory Loss) and (Bond Lost After OTA).
* **Corruption Detection and Recoverability:** Implement checksums (CRC32) and magic numbers to detect corrupted or uninitialized state in RTC memory and NVS flash. Provide graceful recovery paths, including fallback to NVS backups or controlled resets, rather than crashing indefinitely.
* **BLE-Aware Persistence Operations:** Isolate or defer slow flash write operations (NVS, SPIFFS) to prevent real-time performance degradation, CPU starvation, and subsequent BLE connection drops or supervision timeouts. This addresses (Flash write causes BLE disconnect) and .
* **Self-Healing from Boot Loops:** Automatically detect and recover from persistent boot failures caused by corrupted NVS partitions, including preemptive NVS erasure after a bounded number of crash-resets. This is the mechanism.
* **Data Integrity on OTA Rollback:** Preserve critical NVS data (bonds, settings) during OTA updates, ensuring that if new firmware fails self-test, a rollback to the previous version does not result in data loss.

**Explicit Non-Goals:**
* Guaranteed millisecond-accurate absolute timestamps for offline events *before* initial NTP/App time synchronization (timestamps are relative or estimated until sync).
* Arbitrary data recovery from all possible flash corruption scenarios (recovery is bounded and targets common failure modes).
* Real-time processing of all ML data logs (SPIFFS logging is asynchronous and best-effort).

## 3. Storage Architecture

The firmware employs a three-tiered persistence architecture, each optimized for different durability, speed, and power consumption profiles.

### 3.1. RTC Memory

* **Medium:** Static RAM within the ESP32-S3's Real-Time Clock (RTC) domain. Data allocated with `RTC_DATA_ATTR` or `RTC_NOINIT_ATTR`.
* **Durability:** Data persists across **deep sleep** and software resets, but is **lost on power-off or brownout**.
* **Power Profile:** Extremely low power, consuming microamps during deep sleep.
* **Access Speed:** Fast, direct RAM access (nanoseconds).
* **Typical Contents:**
 * **`DetectionEvent` Ring Buffer (`HistoryService`):** Primary storage for up to 50 recent detections. Ensures continuity across deep sleep. (`HistoryService::_state.buffer`)
 * **`CalibrationData` (`main.cpp`):** Sensor thresholds, preserved to avoid recalibration after deep sleep. (`rtcCalibration`)
 * **`TimeSyncData` (`main.cpp`):** Last known time synchronization anchor for relative time calculations. (`rtcTimeSync`)
 * **Crash Counters (`main.cpp`):** Tracks consecutive WDT resets for boot-loop recovery. (`g_crash_counter`)
 * **Flasher Mode Flags (`main.cpp`):** Controls entry into OTA Flasher Mode on reboot or power-cycle recovery. (`g_boot_mode`, `g_stability_counter`)
* **Rationale:** Ideal for transient state that must survive brief sleep periods but does not require full power-loss durability, prioritizing speed and low power.

### 3.2. NVS Flash

* **Medium:** ESP-IDF Non-Volatile Storage (NVS) on external SPI flash. Utilizes the `Preferences` library for key-value access.
* **Durability:Fully durable** across power-off, brownout, and reboots.
* **Power Profile:** Higher power consumption during write/erase operations (tens to hundreds of milliamps for flash operations).
* **Access Speed:** Slower, block-based flash access (tens to hundreds of milliseconds for writes, microseconds for reads). Writes involve CPU halts.
* **Typical Contents:**
 * **`HistoryService` NVS Backup:** Copies of the RTC `DetectionEvent` ring buffer for power-loss recovery. (`HistoryService::flushToNvs`)
 * **`UserConfig` (`ConfigService`):** User-configurable settings (e.g., sensitivity, LED brightness). (`ConfigService::_config`)
 * **NimBLE Bonding Keys (`BLEManager`):** Critical for iOS RPA (Resolvable Private Address) handling and secure reconnection. Configured via `CONFIG_BT_NIMBLE_NVS_PERSIST=1` (`nimconfig.h`).
 * **ML Calibration Prototypes (`PersonalizationService`):** Per-device adaptation parameters for gesture inference. (`PersonalizationService::_data`)
 * **Boot Recovery Tracker (`BootRecoveryTracker`):** Stores `g_stability_counter` for power-cycle triggered OTA recovery.
* **Rationale:** Essential for critical configuration, security credentials, and data that must persist even after power loss. Optimizes for durability and wear leveling (provided by ESP-IDF NVS driver).

### 3.3. SPIFFS

* **Medium:** ESP-IDF SPI Flash File System (SPIFFS) on external SPI flash.
* **Durability:Fully durable** across power-off, brownout, and reboots.
* **Power Profile:** Similar to NVS during writes.
* **Access Speed:** File-based access, generally slower than NVS for small, structured data, but efficient for larger blobs.
* **Typical Contents:**
 * **ML Raw Data Logs (`DataLoggerService`):** `SampleWindow` records for machine learning model training. Stored in sequential binary files (`/ml_sXXXX.bin`). (TinyML).
* **Rationale:** Best suited for large volumes of unstructured or sequential data that can be accessed as files, such as raw sensor readings for ML datasets.


## 4. Data Placement Model

The selection of storage tier for each data class is deliberate, balancing durability, performance, and power consumption.

### 4.1. Event History (`DetectionEvent`)

* **Primary Tier:** RTC Memory (`HistoryService::_state.buffer`).
 * **Rationale:** Provides (RTC Memory Loss on Deep Sleep) and (Data loss during disconnects) protection. Enables Offline Detection Logging and Session Continuity. Fast writes from `SensorTask` hot path. Preserves relative timestamps (`HAL::getSystemUptime`) across deep sleep.
* **Backup Tier:** NVS Flash (`HistoryService`'s NVS backup).
 * **Rationale:** Protects against (RTC Memory Loss) during power-off or brownout. NVS data is used to restore the RTC buffer on cold boot if available.

### 4.2. User Configuration (`UserConfig`)

* **Primary Tier:** NVS Flash (`ConfigService`'s `_config`).
 * **Rationale:** (ESP32 is source of truth). Must survive power loss.
* **Runtime Cache:** RAM (`ConfigService`'s `_config`).
 * **Rationale:** Provides fast access for application logic. Updates pushed to NVS asynchronously.

### 4.3. BLE Bonding Keys and Identity State

* **Primary Tier:** NVS Flash (managed by NimBLE stack).
 * **Rationale:** (iOS RPA Rotation) and (Bond Lost After OTA). Essential for secure, persistent connections and recognizing iOS devices with rotating MAC addresses. Configured via `CONFIG_BT_NIMBLE_NVS_PERSIST=1` in `nimconfig.h`.

### 4.4. ML Calibration & Prototypes (`MLCalibrationData`)

* **Primary Tier:** NVS Flash (`PersonalizationService`'s `_data`).
 * **Rationale:** Per-device adaptation parameters for TinyML gesture classification. Must persist across reboots and power cycles.
* **Runtime Cache:** RAM (`PersonalizationService`'s `_snapshot`).
 * **Rationale:** Provides a lock-free, immutable snapshot for high-frequency reads by `InferenceService`, ensuring data consistency.

### 4.5. ML Raw Data Logs (`SampleWindow`)

* **Primary Tier:** SPIFFS (`DataLoggerService`).
 * **Rationale:** Large-volume sensor data for offline model training. File-based storage is suitable for sequential records and large datasets. (TinyML).

### 4.6. Time Synchronization Anchors

* **Primary Tier (Boot Anchor):** NVS Flash (`ConfigService::lastKnownEpoch`).
 * **Rationale:** (Forward-Chaining Time). Persists the last known Unix epoch to restore the system clock on boot, enabling "compressed timeline" for offline events.
* **Primary Tier (Live Anchor):** RTC Memory (`rtcTimeSync`).
 * **Rationale:** (Robust Offline Timestamp Support). Stores the `millis` value and `epoch` at the last successful `TIME_SYNC`, used by `HistoryService` to convert `DetectionEvent.timestampMs` to absolute time for events in the current session.

### 4.7. Boot and Recovery Counters

* **Crash Counter:** `RTC_NOINIT_ATTR g_crash_counter` in `main.cpp`.
 * **Rationale:** . Must survive WDT resets (panic) but be cleared on power-on (cold boot) to track *consecutive* crashes.
* **Flasher Stability Counter:** `RTC_NOINIT_ATTR g_stability_counter` in `main.cpp` (and NVS-backed by `BootRecoveryTracker`).
 * **Rationale:** (Safe, Recoverable OTA). Tracks rapid power cycles to trigger Flasher Mode (OTA recovery) without a button. `BootRecoveryTracker` provides NVS persistence.

## 5. Integrity Mechanisms

The firmware employs a suite of mechanisms to ensure the integrity, consistency, and validity of persisted data.

### 5.1. Checksums and Magic Numbers

* **CRC32 Validation:** All structured data stored in RTC memory (`RTCState`, `CalibrationData`) and NVS (`UserConfig`, `MLCalibrationData`) includes a CRC32 checksum. On load, the data's CRC is re-calculated and compared.
 * **Action on Mismatch:** If CRCs do not match, the data is considered corrupted. `HistoryService` attempts restoration from NVS backup; `ConfigService` or `main.cpp` revert to safe default values.
 * **Proof:** `HistoryService::calculateBufferCRC`, `HistoryService::validateState`; `main.cpp` for `rtcCalibration`; `ConfigService::calculateConfigCrc`; `PersonalizationService::calculateCrc`.
* **Magic Numbers:** Specific sentinel values (`RTC_MAGIC_NUMBER`, `RTC_MAGIC_KEY`, `g_ghost_bond_magic`) are used in RTC memory to distinguish between a cold boot (uninitialized/garbage memory) and a warm boot (valid, previously written state).
 * **Action on Mismatch:** On cold boot, magic numbers are re-initialized. For `HistoryService`, this determines whether to attempt NVS restore or a full reset.

### 5.2. Asynchronous and BLE-Aware Persistence

* **Decoupled Worker Pattern:** Slow flash write operations (NVS, SPIFFS) are offloaded to dedicated, low-priority FreeRTOS tasks (`HistoryService::StorageWorker`, `ConfigService::ConfigWorker`, `PersonalizationService::WorkerTask`, `DataLoggerService::loggerTask`). These tasks consume write requests from queues.
 * **Rationale:** Prevents CPU halts (50-200ms) during flash writes from starving the BLE stack (running on Core 0) or the high-priority `SensorTask` (Core 1). This is a critical fix for and .
* **BLE-Aware Flush Scheduling:** NVS flushes are conditionally deferred when the BLE radio is active or in sensitive states (e.g., pairing mode).
 * **Logic:** `HistoryService::flushToNvs` checks `_bleConnected` and `_bleManager->isPairingModeEnabled`. Non-critical flushes are skipped during active BLE connections and triggered on disconnect or before deep sleep.
 * **Rationale:** Mitigates (Flash write causes BLE disconnect) and (NVS write during bonding corrupts keys).

### 5.3. Atomic Operations and Optimistic Locking

* **FreeRTOS Mutexes:** Shared RAM state (e.g., `HistoryService::_state`) is protected by FreeRTOS Mutexes (`xSemaphoreCreateMutex`, `xSemaphoreTake`, `xSemaphoreGive`). This ensures atomic read-modify-write cycles and prevents race conditions or "torn reads" on the dual-core ESP32-S3.
 * **Rationale:** Superior to simple spinlocks (`portENTER_CRITICAL`) as mutexes allow task yielding, preventing CPU starvation and deadlocks.
* **Transactional Integrity (Stack Copy):** Write operations like `HistoryService::flushToNvs` copy critical data to a local stack buffer *inside* a mutex-protected critical section, then perform the blocking flash write *outside* the critical section. This ensures the data snapshot is consistent during the blocking I/O phase.
* **Optimistic Locking (Version Tracking):** Data structures like `UserConfig` and `MLCalibrationData` include a `version` field. In the firmware, this primarily tracks schema changes for migration. For full-stack conflict resolution, a more robust optimistic locking pattern (e.g., compare-and-swap) is applied in the React Native / backend layers.

### 5.4. Versioned Schemas and Migration

* **Versioning:** Critical data structures like `UserConfig` (`VERSION=3`) and `MLCalibrationData` (`CALIBRATION_VERSION=1`) include a `version` field.
 * **Rationale:** Enables . Allows for schema migration on firmware updates without discarding old user data. `ConfigService` handles automatic migration logic (`v1` to `v2` to `v3`).

## 6. Persistence Lifecycles

Understanding the data's journey through the storage tiers is crucial for debugging and maintenance.

### 6.1. Normal Detection Event Persistence

1. **Sensor Read:** `SensorTask` reads proximity every 50ms (`SAMPLING_PERIOD_MS`).
2. **RTC Write:** `HistoryService::logDetection` is called from `main.cpp` (within `handleDetection`). It acquires a mutex, adds a new `DetectionEvent` to the `RTCState` ring buffer, updates `bootCount`, `globalEventId`, `timestampMs` (`HAL::getSystemUptime`), and `durationMs`.
3. **NVS Flush Decision:** `logDetection` checks if `NVS_FLUSH_THRESHOLD` (50 events) is met or if the buffer is critically full (90%).
4. **Async NVS Queue:** If a flush is triggered and BLE is not active, a `SAVE_HIT` command is sent to `HistoryService::StorageWorker` via a FreeRTOS queue.
5. **Storage Worker Execution:** `StorageWorker` task (Core 1, low priority) wakes up, processes the `SAVE_HIT` command.
6. **BLE-Aware NVS Write:** `StorageWorker` calls `HistoryService::flushToNvs`. This function performs battery checks (`PowerManager::isBatteryCritical`) and BLE-state checks (`_bleConnected`, `_bleManager->isPairingModeEnabled`) before proceeding.
7. **Data Copy & NVS Write:** Data from the RTC buffer is copied to a stack-allocated local buffer (under mutex). The mutex is released. The local buffer is then written to NVS using `Preferences::putBytes`.
8. **ACK Flow:** If the event is sent over BLE, the app acknowledges it (`MSG_ACK`). `HistoryService::markAsSynced` sets a flag in the RTC `DetectionEvent` to mark it as synchronized.

```mermaid
graph TD
 subgraph Firmware (ESP32-S3)
 A[SensorTask (20Hz)] -->|1. Detects Hit| B{HistoryService.logDetection(duration)}
 B -->|2. Acquires Mutex| C[RTCState Ring Buffer (RAM)]
 C -->|3. Appends DetectionEvent<br>(id, uptimeMs, bootCount, durationMs, epochMs, flags)| D[Updates RTCState CRC32]
 D -->|4. Releases Mutex| E{Needs NVS Flush? (50 events or Critical Buffer)}
 E -- Yes, & BLE NOT Active --> F[Queue SAVE_HIT Command]
 F --> G[StorageWorker Task (Core 1, Low Prio)]
 G -->|5. Dequeues SAVE_HIT| H{HistoryService.flushToNvs}
 H -->|6. Checks BLE/Battery State<br>| I[Copies RTC Buffer to Stack (under Mutex)]
 I -->|7. Releases Mutex| J[Writes Buffer to NVS Flash (Blocking I/O)]
 J -->|8. NVS Flush Complete| K[Updates _lastNvsFlushId, _eventsSinceFlush]
 B -->|9. Returns Event ID (to ProtocolHandler)| L[ProtocolHandler.sendDetectionEvent]
 L -->|10. Sends MSG_DETECTION_EVENT via BLE| M[BLEManager]
 end

 subgraph Mobile App
 N[App: Receives MSG_DETECTION_EVENT] --> O[Processes DetectionEvent]
 O -->|Sends MSG_ACK| M
 M -->|ACK Received| L
 L -->|Notifies HistoryService| P{HistoryService.markAsSynced(eventId)}
 P -->|Updates RTCState (Sets SYNCED flag)| Q[Updates RTCState CRC32]
 end

 style A fill:#e0f7fa,stroke:#00bcd4,stroke-width:2px
 style C fill:#ffe0b2,stroke:#ff9800,stroke-width:2px
 style D fill:#ffe0b2,stroke:#ff9800,stroke-width:2px
 style B fill:#f0f0f0,stroke:#333
 style E fill:#f0f0f0,stroke:#333
 style F fill:#f0f0f0,stroke:#333
 style G fill:#e0f7fa,stroke:#00bcd4,stroke-width:2px
 style H fill:#f0f0f0,stroke:#333
 style I fill:#ffe0b2,stroke:#ff9800,stroke-width:2px
 style J fill:#c8e6c9,stroke:#4caf50,stroke-width:2px
 style K fill:#ffe0b2,stroke:#ff9800,stroke-width:2px
 style L fill:#f0f0f0,stroke:#333
 style M fill:#f0f0f0,stroke:#333
 style N fill:#e0f7fa,stroke:#00bcd4,stroke-width:2px
 style O fill:#f0f0f0,stroke:#333
 style P fill:#f0f0f0,stroke:#333
 style Q fill:#ffe0b2,stroke:#ff9800,stroke-width:2px
```

### 6.2. User Configuration Persistence

1. **User Input:** App calls `DeviceSettingsService::setSensitivity` (in JavaScript).
2. **Optimistic UI Update:** `DeviceSettingsService` immediately updates local `sensitivity` (RAM) and notifies UI. The old value is cached for rollback (Optimistic UI Rollback).
3. **Debounced Send:** `DeviceSettingsService` debounces the update for 500ms (`DEBOUNCE_DELAY_MS`). On timer expiry, it calls `EdgeDeviceProtocolService::sendSetSensitivity`.
4. **BLE Send & ACK:** `ProtocolHandler` sends `MSG_SET_CONFIG` to the device and waits for an ACK.
5. **Device Update:** `ProtocolHandler::handleSetConfig` on firmware updates the `ConfigService`'s `_config` (RAM) and calls `Sensor_HAL::setUserSensitivity` (immediate physical effect).
6. **Firmware Debounce & NVS Write:** `ConfigService::setSensitivity` (on firmware) debounces the NVS write for 30 seconds (`COMMIT_DELAY_MS`). A `ConfigWorker` task performs the actual NVS `writeToNvs` to persist `UserConfig`.
7. **Confirmation (`CONFIG_DATA`):** On successful NVS write, the device may send `MSG_CONFIG_DATA` to confirm its state, updating the app's cache.
8. **Rollback/Persistence:** If `MSG_SET_CONFIG` fails, `DeviceSettingsService` rolls back UI to `previousSensitivity`. If send fails due to disconnect, `DeviceSettingsService` saves pending config to AsyncStorage for reconnect.

### 6.3. Boot / Wake Restoration

1. **Device Boot:** `main.cpp` execution starts.
2. **Crash Recovery Check:** `performCrashRecoveryCheck` examines `esp_reset_reason` and `g_crash_counter`. If `CRASH_RECOVERY_THRESHOLD` (3) is exceeded, an **emergency NVS erase** is performed.
3. **HAL Init:** `StorageHAL`, `LED_HAL`, `Sensor_HAL` are initialized.
4. **OTA Validation:** `OTAManager::validateBootedFirmware` marks newly flashed firmware as valid or triggers an automatic rollback.
5. **History Service Init:** `HistoryService::init` checks `RTCState.magicNumber` and `RTCState.crc32`.
 * **Cold Boot (Magic Mismatch / Power On):** Attempts `restoreFromNvs` (NVS backup). If not, `resetState`. `bootCount` is incremented.
 * **Warm Boot (Deep Sleep Wake):** If CRC/Magic are valid, `bootCount` is *not* incremented (Session Continuity). `HAL::getSystemUptime` provides consistent time.
6. **Config Service Init:** `ConfigService::init` loads `UserConfig` from NVS, restoring `sensitivity`, `ledBrightness`, `configSignature`, and `lastKnownEpoch`.
 * **Time Forward-Chaining:** If `lastKnownEpoch` is valid, `settimeofday` updates the system clock, ensuring offline events can be timestamped relative to the last known epoch.
7. **`Sensor_HAL` Thresholds:** `Sensor_HAL::setBaseThresholds` restores calibrated base thresholds from `rtcCalibration` (RTC memory), then applies the `UserConfig::sensitivity` multiplier to derive effective runtime thresholds.

### 6.4. ML Data Logging

1. **Sample Acquisition:** `SensorTask` acquires proximity samples at 20Hz (`SAMPLING_PERIOD_MS`).
2. **Window Buffering:** `DataLoggerService::appendSample` adds the samples to a RAM ring buffer.
3. **Window Completion:** When a `SampleWindow` is full, it's copied to an inference buffer (for `InferenceService`) and, if a recording session (`DataLoggerService`'s `_state == LoggerState::RECORDING`) is active, the window is sent to `DataLoggerService::loggerTask` via a FreeRTOS queue.
4. **SPIFFS Write:** `loggerTask` processes the queue, opening the session file (`/ml_sXXXX.bin`) on SPIFFS in `FILE_APPEND` mode, and writing the `SampleWindow` struct. CRC32 validates the `SampleWindow` contents to ensure integrity (`SampleWindow::calculateCRC`).

## 7. Failure Handling and Recovery

The storage subsystem is designed to tolerate and recover from common failure modes with clear, bounded behavior.

### 7.1. NVS Corruption (, )

* **Problem:** Power loss during an NVS write (e.g., saving bonding keys or history backup) can corrupt the NVS partition structure. This can cause `nvs_flash_init` to hang indefinitely or crash the device (`IWDT` panic).
* **Detection:** `main.cpp::performCrashRecoveryCheck` observes consecutive Watchdog Timer (WDT) resets (counted by `RTC_NOINIT_ATTR g_crash_counter`). `StorageHAL::initNvs` detects `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`.
* **Recovery:**
 1. **Preemptive Erase:** If `g_crash_counter` exceeds `CRASH_RECOVERY_THRESHOLD` (3 consecutive WDT resets), `main.cpp` performs an **emergency `nvs_flash_erase`** *before* `nvs_flash_init`. This removes the corrupt data that would cause the hang.
 2. **Graceful Erase:** `StorageHAL::initNvs` attempts `nvs_flash_erase` and re-initialization if `nvs_flash_init` returns a known corruption error.
* **Consequence:** All NVS-persisted data (BLE bonds, user config, history backup) is lost, reverting the device to a factory state. This is a destructive but necessary step to break unrecoverable boot loops. The device is then usable.

### 7.2. RTC Memory Corruption

* **Problem:** Rare hardware faults or voltage glitches can corrupt data stored in RTC memory.
* **Detection:** `HistoryService::init` verifies `RTCState.magicNumber` and `RTCState.crc32`. `main.cpp` checks `rtcCalibration`'s CRC.
* **Recovery:**
 1. **NVS Backup Restore:** `HistoryService` attempts to `restoreFromNvs` (loading the entire `RTCState` from its NVS backup).
 2. **Full Reset:** If NVS backup fails or is unavailable, `HistoryService::resetState` clears the RTC buffer. `main.cpp` resets `rtcCalibration` to defaults.
* **Consequence:** Data in RTC is lost. If NVS backup also fails, all event history (`DetectionEvent` ring buffer) is permanently lost.

### 7.3. Power Loss During Write Operations (, )

* **Problem:** Abrupt power removal (e.g., battery dies) during a flash write (NVS, SPIFFS, or OTA) can lead to data corruption or incomplete writes.
* **Prevention:**
 * **BLE-Aware Flushing:** `HistoryService` and `ConfigService` defer NVS writes if BLE is active.
 * **Battery Guards:** `PowerManager::isBatteryCritical` checks battery level before `HistoryService::flushToNvs`, skipping writes to prevent brownout-induced corruption.
 * **Atomic Transactions:** Write operations (like `HistoryService::flushToNvs`) are designed to be idempotent and minimize the critical window.
 * **OTA Rollback:** `OTAManager` ensures incomplete OTA updates don't brick the device, using dual-bank partitions and self-test/rollback.
* **Recovery:** Subsequent boot detection and recovery mechanisms (NVS/RTC corruption checks) will handle any resulting corrupted state.

### 7.4. Boot Loops from Corrupted Persisted State

* **Problem:** Certain types of NVS corruption (e.g., circular references in page headers) can cause `nvs_flash_init` to hang the CPU, leading to an Interrupt Watchdog Timer (IWDT) panic. The device then reboots and enters a persistent loop.
* **Detection:** `RTC_NOINIT_ATTR g_crash_counter` in `main.cpp` monitors consecutive WDT resets across reboots.
* **Recovery:** If `g_crash_counter` reaches `CRASH_RECOVERY_THRESHOLD` (3), `main.cpp::performCrashRecoveryCheck` performs a **preemptive `nvs_flash_erase`** to remove the offending data, breaking the loop.
* **Consequence:** Device is factory reset but becomes usable.

### 7.5. Stale or Mismatched Device State (, )

* **Problem:** Device resets or reflashes (`globalEventId` resets to 0), while the app retains an older `lastEventId`. New device events are then rejected as "old" by the app. Or, app has old bonding keys while device has no keys (reset).
* **Recovery (Event ID Sync):** `HistoryService::fastForwardEventId` (triggered by `ProtocolHandler::handleHello`) adjusts the device's `globalEventId` to match the app's `lastEventId` after a cold boot, ensuring new events are accepted. This is immediately persisted to NVS.
* **Recovery (Config Signature):** `ConfigService::UserConfig.signature` (random ID on factory reset) is compared with `HelloAckPayload.configSignature`. A mismatch indicates the device was factory reset, prompting the app to discard local "zombie" settings.

## 8. Trade-offs and Design Decisions

The storage architecture reflects deliberate trade-offs to optimize for real-world constraints of an embedded device.

* **Durability vs. Power/Speed:** Using RTC for high-frequency, temporary data (`DetectionEvent`) prioritizes low power and speed for deep sleep continuity, while offloading durable storage (`UserConfig`, `HistoryService` NVS backup) to NVS incurs higher power consumption and latency during writes.
* **BLE Stability vs. Immediate Persistence:** Asynchronous, BLE-aware NVS writes (`HistoryService`, `ConfigService`) prioritize maintaining stable BLE communication over immediate data persistence. This accepts a small risk of data loss (e.g., if power fails immediately after RTC write but before NVS flush) to prevent critical BLE disconnects.
* **Data Loss for Recoverability:** In extreme failure scenarios (e.g., low battery brownout loops, irrecoverable NVS corruption), the system prioritizes making the device usable over preserving all data. This may involve discarding some data (e.g., last few detections, clearing NVS entirely) to enable robust self-healing.
* **Memory Footprint:** Fixed-size buffers and static allocations (`HistoryService::_state.buffer`, `ProtocolHandler::_txFrameBuffer`) are preferred over dynamic memory allocation to prevent fragmentation and ensure deterministic memory usage in a constrained environment.
* **User Experience vs. System Complexity:** Implementing features like Offline Detection Logging and Forward-Chaining Timestamps adds complexity to the persistence model (RTC time, epoch backfill logic) but significantly enhances the user experience by providing seamless data continuity.

## 9. Related Documents

* [architecture.md](architecture.md): Provides an overview of the overall firmware system architecture.
* [power-state-machine.md](power-state-machine.md): Details the device's power management and sleep/wake transition logic.
* [failure-modes.md](failure-modes.md): Catalog of failure modes and mitigation strategies.
* [ota-recovery.md](ota-recovery.md): Explains the detailed process for OTA updates, including rollback and recovery mechanisms.
* [task-model.md](task-model.md): Describes the FreeRTOS task architecture and inter-task communication patterns.

## 11. Appendix

### 11.1. Terminology Glossary

* **Atomicity:** An operation that is guaranteed to either complete entirely or not at all, leaving the system in a consistent state.
* **Bounded Recovery:** A recovery process that is guaranteed to complete within a finite (and usually short) time, preventing indefinite loops or hangs.
* **Continuity:** The preservation of sequential or temporal relationships of data across system state changes (e.g., deep sleep).
* **Corruption:** The state where stored data is invalid, inconsistent, or unreadable due to unexpected events (e.g., power loss, hardware fault).
* **Durability:** The property of data surviving power loss or system resets.
* **Idempotence:** An operation that can be performed multiple times without changing the result beyond the initial application.
* **Integrity:** The property that data is accurate, consistent, and valid.
* **Persistence Tier:** A distinct layer of storage (e.g., RTC, NVS, SPIFFS) with specific characteristics regarding durability, speed, and power.
* **Recoverability:** The ability of the system to return to a functional state after a failure, potentially with some data loss.
* **Wear Leveling:** Techniques to distribute writes evenly across flash memory sectors to prolong lifespan.

### 11.2. Storage Tier Comparison

| Storage Medium | Primary Use Case | Data Durability | Power Profile (Write) | Access Speed | Key Integrity Mechanisms |
| :------------- | :--------------------------- | :----------------------- | :-------------------- | :----------------- | :------------------------------- |
| **RTC Memory** | Live state, deep sleep continuity | Volatile (power-off) | Very Low | Very Fast (RAM) | CRC32, Magic Numbers |
| **NVS Flash** | Critical configuration, backups | Durable (power-off) | High (flash write) | Medium (micro-ms) | CRC32, Async Workers, BLE-Aware |
| **SPIFFS** | Large file data (ML logs) | Durable (power-off) | High (flash write) | Medium (file I/O) | CRC32, Async Workers |

