
# Task Model - ESP32-S3 Firmware

## 1. Overview
This document details the concurrency architecture of theESP32-S3 firmware, built upon a dual-core FreeRTOS foundation. It explains the purpose, responsibilities, and interactions of each FreeRTOS task, highlighting how this multi-tasking design ensures real-time sensor processing, maintains critical BLE connectivity, and guarantees data integrity by strategically isolating blocking I/O and CPU-intensive operations across distinct application tasks. Readers will understand the core architectural decisions, inter-task communication mechanisms, and the reliability guarantees provided by this model.

## 2. Design Goals
The firmware's concurrency model is driven by several critical design goals to ensure robustness and responsiveness in a resource-constrained embedded environment:
* **Protect the Real-Time Sensing Path:** Guarantee precise 20Hz sensor sampling and immediate event detection without interruption.
* **Prevent BLE Starvation:** Isolate long-running or blocking I/O operations (e.g., flash writes) from the BLE stack to prevent connection dropouts and maintain responsive communication.
* **Isolate Deferred Work:** Offload persistence, configuration updates, debugging logs, and machine learning processing to dedicated background tasks.
* **Maintain Safe Cross-Core Concurrency:** Ensure data consistency and prevent race conditions when tasks on different CPU cores access shared mutable state.
* **Preserve Responsiveness:** Ensure the device responds to user input and BLE commands even during heavy background processing or network activity.
* **Ensure Data Integrity & System Recoverability:** Implement mechanisms to protect critical data (RTC, NVS) and allow the system to self-heal from unexpected failures.

## 3. Runtime Architecture at a Glance
The the firmware employs a core-pinned FreeRTOS task architecture, partitioning responsibilities across the ESP32-S3's two Xtensa LX7 CPU cores. This separation is fundamental to achieving the design goals, allowing latency-critical operations to run unimpeded by deferred work.

**the Firmware Task Topology & Data Flow**
```
 +-----------------------+
 | ESP32-S3 SoC |
 +-----------------------+
 │
+------------------------------------------------+------------------------------------------------+
| Core 0 (System / BLE Focus) | Core 1 (Application Focus) |
+------------------------------------------------+------------------------------------------------+
| NimBLE Stack & Controller (Internal FreeRTOS Tasks) | |
| (Manages BLE Radio, Link Layer, Host) | |
| | |
| +---------------------------+ | +---------------------------+ |
| | ConfigWorker (P1) | | | SensorTask (P10) | <--------+ Sensor_HAL
| | (Async NVS: UserConfig) | <-------+ | (20Hz Polling, Hit Detect)| ---------> DetectionState (atomic)
| | | Timer | | | |
| +------------^--------------+ Notify | +------------^--------------+ |
| │ | │ |
| +---------------------------+ | │ (DetectionEvent) |
| |PersonalizationWorker (P1) | <-------+ +---------------------------+ |
| | (Async NVS: ML Adaptaion) | Timer | | HistoryService (RAM Buffer)| ---------> _storageQueue
| | | Notify | | | |
| +---------------------------+ | +------------v--------------+ |
| | │ |
+----------------------------------------+----------------------------------------+
 │ +---------------------------+ |
 | |ProtocolTask (main loop, P1)| ---------> BLEManager
 | | (BLE Dispatch, Retries, PM)| |
 | +------------^--------------+ |
 | │ |
 | +---------------------------+ |
 | | StorageWorker (P2) | <---------+ _storageQueue
 | | (Async NVS: Hit History) | |
 | +---------------------------+ |
 | |
 | +---------------------------+ |
 | | DataLoggerService | ---------> _loggerQueue
 | | (Raw ML Samples, RingBuf) | |
 | +------------^--------------+ |
 | │ |
 | +---------------------------+ |
 | | LoggerTask (P1) | <---------+ _loggerQueue
 | | (Async SPIFFS: ML Data) | |
 | +------------^--------------+ |
 | │ |
 | +---------------------------+ Task |
 | | InferenceTask (P3) | Notify <---+ DataLoggerService
 | | (ML Gesture Classification)| |
 | +---------------------------+ |
 | |
 +------------------------------------------+
 Legend:
 P# = Priority
 Notify = Task Notification

```
**Critical Path vs. Deferred Work:**
* **Real-time Path (High Priority):** Primarily managed by `SensorTask` on Core 1, responsible for precise 20Hz sensor polling and initial event detection. It avoids any blocking operations.
* **Connection-Critical Path (Medium Priority):** The main application loop (conceptual `ProtocolTask`) runs on Core 1, handling BLE event dispatch, protocol message processing, and power management. It aims to remain non-blocking and yields regularly.
* **Deferred Persistence & Background Work (Low Priority):** Tasks like `StorageWorker`, `ConfigWorker`, `LoggerTask`, `InferenceTask`, and `PersonalizationWorker` handle disk I/O (NVS, SPIFFS) or CPU-intensive ML computations. These are designed to be preempted by higher-priority tasks and only run when system resources are otherwise idle.

## 4. Task Inventory
This section provides a detailed breakdown of each significant FreeRTOS task within thefirmware.

| Task Name | Main Function(s) | Core | Priority (Effective) | Criticality | Primary Responsibilities | Blocking Risk | Owned Resources / Key IPC |
| :----------------------- | :------------------------------------------------- | :--- | :------------------- | :------------ | :--------------------------------------------------------------- | :------------ | :----------------------------------------------------------------------- |
| **SensorTask** | `main.cpp:sensorTask` | 1 | 10 | **Real-time** | 20Hz sensor polling, raw sample capture, multi-level event detection, runtime flags update, rolling average update | Low | `Sensor_HAL` (read), `DetectionState` (atomic writes), `DataLoggerService::appendSample`, `PowerManager::updateRollingProxAverage` |
| **ProtocolTask** (main) | `main.cpp:loop` | 1 | 1 (Default) | **Medium** | BLE event handling, message dispatch, retry management, power state transitions, LED patterns, button input | Low | `BLEManager`, `ProtocolHandler`, `PowerManager`, `LED_HAL`, `OTAManager`, `ConfigService`, `WifiService`, `TimeService`, `DetectionDeliveryPump`, `DataLoggerService`, `InferenceService`, `PersonalizationService` |
| **StorageWorker** | `HistoryService.cpp:storageWorker` | 1 | 2 | **Deferred** | Asynchronous NVS writes for `HistoryService` (detection event backup) | High (NVS I/O) | `Preferences` API, `HistoryService`'s internal data. Input: `HistoryService::_storageQueue` |
| **ConfigWorker** | `ConfigService.cpp:configWorkerTask` | 0 | 1 | **Deferred** | Asynchronous NVS writes for `ConfigService` (user settings) | High (NVS I/O) | `Preferences` API, `ConfigService`'s internal data. Input: Task notification |
| **LoggerTask** | `DataLoggerService.cpp:loggerTask` | 1 | 1 | **Background**| Asynchronous SPIFFS writes for `DataLoggerService` (ML data capture) | Medium (SPIFFS I/O) | `SPIFFS` filesystem. Input: `DataLoggerService::_loggerQueue` |
| **InferenceTask** | `InferenceService.cpp:inferenceTask` | 1 | 3 | **Background**| On-device ML inference for gesture classification | Low (CPU-bound) | `DataLoggerService` (read window), `PersonalizationService` (read model). Input: Task notification |
| **PersonalizationWorker** | `PersonalizationService.cpp:workerTask` | 1 | 1 | **Deferred** | Asynchronous NVS writes for `PersonalizationService` (ML adaptation) | High (NVS I/O) | `Preferences` API, `PersonalizationService`'s internal data. Input: Task notification |

* **Design Note: ProtocolTask Priority Discrepancy**
 The `platformio.ini` `FreeRTOSConfig` section comments suggest `PROTOCOL_TASK_PRIORITY=5`. However, the main application loop in `main.cpp` (which conceptually acts as the `ProtocolTask`) runs at Arduino's default priority (typically 1). This means its effective priority is lower than `InferenceTask` (3) and `StorageWorker` (2). While `ProtocolTask` is designed to be non-blocking and `BLEManager` offloads some critical BLE operations to Core 0, this discrepancy could lead to unexpected latency or preemption if not carefully managed.

* **Design Note: ConfigWorker on Core 0**
 The `ConfigWorker` is explicitly pinned to Core 0 with the lowest priority (1), sharing the core with the high-priority NimBLE host task. While `ConfigWorker` uses a mutex for NVS access, its low priority and contention on Core 0 introduce a potential risk of starvation or subtle interference with BLE operations, especially if the NimBLE stack is under heavy load. This choice aims to offload NVS operations from Core 1's general application workload, but careful monitoring is warranted.

## 5. Scheduling and Core Assignment
The scheduling strategy explicitly partitions responsibilities between the ESP32-S3's two cores, prioritizing real-time responsiveness and isolating blocking operations.

* **Core 0 (System / BLE Focus):**
 * **Primary Role:** Dedicated to the NimBLE stack (BLE Controller, NimBLE Host Task). This ensures the BLE radio can service its timing-critical Link Layer operations without interruption.
 * **ConfigWorker:** Unusually, the low-priority `ConfigWorker` is also pinned here. This might be to offload its (blocking) NVS writes from Core 1, but it means its execution is directly contended with the BLE stack. Its very low priority (1) generally ensures it yields to NimBLE.

* **Core 1 (Application Focus):**
 * **Primary Role:** Hosts all other application tasks, balancing real-time needs with deferred processing.
 * **SensorTask (Priority 10):** Highest priority. Ensures deterministic 20Hz sensor sampling. Uses `vTaskDelayUntil` for precise periodic execution, crucial for accurate data acquisition.
 * **InferenceTask (Priority 3):** Runs above general background tasks. Provides responsive ML classification.
 * **StorageWorker (Priority 2):** Runs when `SensorTask` or `InferenceTask` yield. Decouples persistent storage writes from higher-priority work.
 * **ProtocolTask (main loop, Priority 1):** Runs at the default Arduino priority. Handles user-facing logic, BLE message dispatch, and orchestrates other services. It uses `vTaskDelay` to yield regularly, ensuring lower-priority workers get CPU time.
 * **LoggerTask (Priority 1):** Lowest priority. Executes SPIFFS writes for ML data when other tasks are idle.
 * **PersonalizationWorker (Priority 1):** Lowest priority. Persists ML model adaptation data when other tasks are idle.

* **Intentional Serialization & Deferral:**
 * Blocking operations like NVS and SPIFFS writes are *intentionally* serialized within their respective worker tasks and run at low priorities. This is a core design decision to prevent them from interfering with BLE timing or sensor acquisition.
 * `vTaskDelay` and `xTaskNotifyWait` with timeouts are used extensively to allow tasks to yield the CPU, preventing busy-waiting and ensuring fair scheduling.

## 6. Inter-Task Communication Model
Tasks communicate and coordinate using a combination of FreeRTOS primitives, each chosen for its specific properties regarding data transfer, signaling, and synchronization.

* **Queues:**
 * **Purpose:** Decouple producers from consumers, allowing asynchronous data transfer and buffering. They act as bounded buffers, applying backpressure if the consumer cannot keep up (e.g., if flash I/O is slow).
 * **Usage:**
 * `HistoryService` to `StorageWorker`: `StorageMessage` structs (containing commands and event IDs) are placed in `_storageQueue`. `StorageWorker` blocks on this queue.
 * `DataLoggerService` to `LoggerTask`: `SampleWindow` structs (raw ML data) are placed in `_loggerQueue`. `LoggerTask` blocks on this queue.
 * **Backpressure:** If a queue is full, the producer (e.g., `HistoryService::logDetection` or `DataLoggerService::appendSample`) will typically attempt a non-blocking `xQueueSend` and, if it fails, either drop the data (e.g., `_dropCount` in `DataLoggerService`) or defer the operation (e.g., `_flushPending` in `HistoryService`).

* **Task Notifications:**
 * **Purpose:** Lightweight, highly efficient signaling mechanism for one-to-one communication, often used to unblock a waiting task without the overhead of queues or semaphores for simple events.
 * **Usage:**
 * `ConfigService` timer callback to `ConfigWorker`: Signals that a `UserConfig` needs to be saved.
 * `PersonalizationService` timer callback to `PersonalizationWorker`: Signals that `MLCalibrationData` needs to be saved.
 * `DataLoggerService::appendSample` to `InferenceTask`: Signals that a new `SampleWindow` is ready for ML inference (`xTaskNotifyGive`).

* **Mutexes (Binary Semaphores):**
 * **Purpose:** Protect shared mutable data structures from concurrent access, ensuring only one task modifies the data at a time. This prevents race conditions and maintains data integrity.
 * **Usage:**
 * `_halMux` (global): Guards critical sections in HAL drivers (`HAL_ENTER_CRITICAL`), ensuring ISR-safe access to shared hardware state.
 * `HistoryService::_stateMutex`: Protects the RTC ring buffer (`RTCState`) when `SensorTask` is writing `DetectionEvent`s and `ProtocolTask` (or `StorageWorker`) is reading.
 * `ProtocolHandler::_mutex`: Protects the BLE TX frame buffer (`_txFrameBuffer`) and the `_pendingMessages` map when `ProtocolTask` is sending/retrying messages and `BLEManager` (via callback) is processing ACKs.
 * `ConfigService::_mutex`: Protects the `UserConfig` struct during reads/writes from `ProtocolTask` or `ConfigWorker`.
 * `DataLoggerService::_inferenceMutex`: Protects the `_inferenceWindow` when `SensorTask` is copying new samples and `InferenceTask` is reading for inference.
 * `PersonalizationService::_mutex`: Protects the `MLCalibrationData` struct during reads/writes from `ProtocolTask` or `PersonalizationWorker`.

* **Atomics (`std::atomic<T>`):**
 * **Purpose:** Provide lock-free, thread-safe access for single variables of fundamental types. They guarantee atomicity and proper memory ordering (visibility across cores) without the overhead of mutexes, especially useful for flags and counters.
 * **Usage:**
 * `Sensor_HAL` (`_interruptTriggered`, `_lastReading`, `_detectionThreshold`, `_criticalDetectionThreshold`, `_userSensitivity`, `_environmentBackoff`): Flags and values shared between ISRs and `SensorTask`, or between `SensorTask` and `ProtocolTask`.
 * `main.cpp` (`g_isWarmupPeriod`, `DetectionState` members like `isDetecting`, `isConfirmedDetection`, `requestBackoff`): Flags controlling high-level behavior, updated by `SensorTask` and read by `ProtocolTask`.
 * `DataLoggerService` (`_loggerTaskRunning`, `_initialized`, `_dropCount`, `_totalWindowsWritten`, `_cachedFlags`): Internal flags and counters updated across tasks.

## 7. Resource Ownership and Synchronization
Clear resource ownership and stringent synchronization rules are foundational to the firmware's stability, especially given the dual-core architecture and shared peripherals.

* **BLE Radio & Stack:**
 * **Ownership:** The NimBLE stack (running on Core 0) has exclusive ownership of the BLE radio hardware and manages the Link Layer timing.
 * **Synchronization:** `BLEManager` acts as the primary interface to NimBLE. `ProtocolHandler` interacts with `BLEManager` via a `BLESendFunction` and expects `BLEManager` to handle internal synchronization for radio access. Application tasks on Core 1 must *never* directly access or block the BLE radio.

* **Flash Memory (NVS & SPIFFS):**
 * **Ownership:** Write access to NVS is strictly owned by `StorageWorker`, `ConfigWorker`, and `PersonalizationWorker`. Write access to SPIFFS is strictly owned by `LoggerTask`. Each worker operates on its own dedicated NVS namespace or SPIFFS directory.
 * **Synchronization:** All write operations are deferred to these low-priority tasks, running outside of critical BLE or sensor paths. These tasks explicitly feed the watchdog and yield before/after blocking flash I/O to minimize interference. Read operations from NVS or SPIFFS by other tasks typically happen via copying, either from RAM-cached versions (protected by mutexes) or by reading snapshots.

* **I2C Bus:**
 * **Ownership:** `Sensor_HAL` owns direct access to the I2C bus for the proximity sensor.
 * **Synchronization:** `Sensor_HAL`'s methods encapsulate I2C transactions. Critical sections within `Sensor_HAL` are protected by `_halMux`. The `recoverI2CBus` mechanism ensures the bus is in a clean state before use.

* **RTC Memory (`RTCState`):**
 * **Ownership:** `HistoryService` is the sole owner of the `RTCState` struct, which persists across deep sleep.
 * **Synchronization:** Access to `RTCState` is protected by `HistoryService::_stateMutex`. Any task reading or writing to this state must acquire the mutex.

* **Sensor Configuration (`UserConfig`, `MLCalibrationData`):**
 * **Ownership:** `ConfigService` owns `UserConfig`, and `PersonalizationService` owns `MLCalibrationData`.
 * **Synchronization:** Access to these structs is protected by their respective mutexes (`ConfigService::_mutex`, `PersonalizationService::_mutex`). `Sensor_HAL` reads values (e.g., sensitivity, thresholds) via `std::atomic` getters from these services, ensuring lock-free access to the current effective values.

* **General Shared Mutable State:**
 * **Rule:** Any variable or data structure that can be modified by one task and read by another, or modified by an ISR and read by a task, *must* be explicitly protected. This is achieved using `std::atomic` for simple types (e.g., `Sensor_HAL` flags, `DetectionState` in `main.cpp`) or FreeRTOS `SemaphoreHandle_t` (mutexes) for complex data structures.
 * **Prohibited Operations:** Latency-sensitive tasks (`SensorTask`, `ProtocolTask`) are strictly forbidden from performing any blocking I/O (e.g., direct NVS/SPIFFS reads/writes, extensive `delay` calls, complex `Serial` operations) or CPU-intensive computations that could exceed their allocated time slice. Such operations *must* be offloaded to dedicated worker tasks or performed asynchronously via callbacks.

## 8. Critical Execution Paths
Tracing the flow of work through the task model illustrates how concurrency ensures both real-time responsiveness and robust background processing.

* **Sensor Sampling to Event Detection (Real-Time Critical Path):**
 1. **SensorTask (Core 1, P10):** Wakes precisely every 50ms (20Hz) via `vTaskDelayUntil`.
 2. Reads raw proximity from `Sensor_HAL` (I2C via `Wire`, non-blocking).
 3. Appends `SampleRecord` to `DataLoggerService`'s internal ring buffer (`appendSample`, non-blocking).
 4. Updates `PowerManager`'s rolling proximity average for dynamic sleep thresholds.
 5. Performs multi-level event detection logic (`handleDetection`) using current thresholds from `Sensor_HAL`.
 6. Atomically updates `DetectionState` flags (`isDetecting`, `isConfirmedDetection`).
 7. If a validated detection occurs, calls `HistoryService::logDetection`.

* **Detection Event to Protocol Delivery (Connection-Critical Path):**
 1. **HistoryService::logDetection (from SensorTask, P10):**
 * Acquires `_stateMutex`.
 * Adds `DetectionEvent` to `RTCState` ring buffer.
 * Updates `RTCState.crc32`.
 * Releases `_stateMutex`. (Total time < few microseconds).
 * Checks `_eventsSinceFlush` and `_bleConnected` state.
 * If necessary, non-blocking `queueStorageCommand(SAVE_HIT)` to `StorageWorker`.
 * Dispatches `onDetectionEvent` callback (async, to `BluetoothService`).
 2. **ProtocolTask (`main.cpp:loop`, Core 1, P1):** Continuously runs, calling `BluetoothHandler.getInstance.getEventSyncService.onProcessedDetectionEvent` via callback after DB persistence.
 3. **EventSyncService (from `BluetoothService` callback):** Converts device timestamps to absolute time.
 4. **EdgeDeviceProtocolService (from `EventSyncService`):**
 * Builds `MSG_DETECTION_EVENT` frame.
 * `sendWithAck`: Takes `_mutex` (ProtocolHandler), stores `PendingMessage`, releases `_mutex`.
 * Sends frame via `BLESendFunction` (to `BLEManager`, which uses NimBLE on Core 0).
 5. **BLEManager (Core 0):** Transmits `MSG_DETECTION_EVENT` via BLE notification.
 6. **App:** Receives event, sends `MSG_ACK`.
 7. **EdgeDeviceProtocolService (via `BLEManager` callback):** Receives `MSG_ACK`, finds `PendingMessage`, resolves Promise.

* **Deferred Persistence Path (NVS Write):**
 1. **HistoryService::logDetection (from SensorTask):** If `needsNvsFlush` and conditions allow (e.g., BLE not critically busy), calls `queueStorageCommand(SAVE_HIT)`.
 2. **StorageWorker (Core 1, P2):** Blocks on `_storageQueue`.
 3. Receives `StorageMessage(SAVE_HIT)`.
 4. **`HistoryService::flushToNvs` (called by StorageWorker):**
 * Acquires `_stateMutex`.
 * Copies `RTCState` to local stack buffer.
 * Releases `_stateMutex`.
 * `esp_task_wdt_reset`.
 * Performs blocking `Preferences.putBytes` (NVS write, 10-100ms).
 * `esp_task_wdt_reset`.
 5. `StorageWorker` yields (`vTaskDelay`).

* **ML Data Logging / Inference Path:**
 1. **SensorTask (Core 1, P10):** Calls `DataLoggerService::appendSample` for each raw proximity reading.
 2. **DataLoggerService::appendSample (non-blocking):**
 * Appends `SampleRecord` to internal `_sampleBuf` (local RAM ring buffer).
 * If `_sampleBuf` is full (window complete):
 * Acquires `_inferenceMutex`.
 * Copies completed window to `_inferenceWindow`.
 * Releases `_inferenceMutex`.
 * `xTaskNotifyGive(_inferenceTaskHandle)` (unblocks `InferenceTask`).
 * If `_state == RECORDING`, non-blocking `xQueueSend` `SampleWindow` to `_loggerQueue`.
 3. **InferenceTask (Core 1, P3):** Blocks on `ulTaskNotifyTake`.
 4. Receives notification. Acquires `_inferenceMutex`, reads `_inferenceWindow`, releases `_inferenceMutex`.
 5. Performs feature extraction and ML prediction (`predictHybrid`).
 6. Posts `InferenceResult` to an internal queue (`_resultQueue`) for `ProtocolTask` access.
 7. **LoggerTask (Core 1, P1):** Blocks on `_loggerQueue`.
 8. Receives `SampleWindow`. Performs blocking `SPIFFS.write` to session file.

 **Sleep Entry / Wake Path:**
 1. **ProtocolTask (`loop`, Core 1, P1):** `PowerManager::update` detects idle timeout or `MSG_SLEEP` command.
 2. **PowerManager (state machine):** Transitions to `SLEEP_PREP`.
 3. `PowerManager::checkIdleTimeout`/`canSafelyEnterSleep` checks conditions (pending ACKs, OTA in progress, battery level, `HistoryService` pending data).
 4. If safe: `PowerManager::enterDeepSleep`:
 * Signals `onBeforeSleep` callback (shuts down `InferenceTask`, `LoggerTask`).
 * `ConfigService::forceFlush` (sync NVS write).
 * `HistoryService::stopStorageWorker` (processes remaining queue, deletes task).
 * `BLEManager::disconnect`/`stopAdvertising`.
 * `LED_HAL::prepareForSleep` (turns off LED, holds GPIO state).
 * `Sensor_HAL::configureForDeepSleep` (configures low-power interrupt mode with *verification*).
 * `PowerManager::configureWakeSources` (enables `EXT1` for sensor/button, `EXT0` for button as backup).
 * `esp_sleep_enable_timer_wakeup` (failsafe timer, either 2hr or 30s if sensor is bad).
 * `Sensor_HAL::isProximityBelowThreshold` (: final check for elevated proximity before sleep).
 * `digitalRead(Pins::PROXIMITY_INT)` (: last-second GPIO check for active touch).
 * `esp_deep_sleep_start` (device enters deep sleep).

## 9. Failure Containment and Reliability Rationale
The multi-tasking model is not merely an organizational choice; it is a fundamental reliability boundary designed to prevent critical system failures and ensure a robust user experience.

* **CPU Starvation Prevention:** By offloading all blocking I/O (NVS flash, SPIFFS) and potentially long CPU-bound operations (ML inference training) to dedicated low-priority worker tasks (`StorageWorker`, `ConfigWorker`, `LoggerTask`, `PersonalizationWorker`), the latency-sensitive `SensorTask` (20Hz loop) and `ProtocolTask` (BLE communications) are guaranteed CPU cycles. This prevents the primary data acquisition and connectivity functions from being starved.

* **Flash-Write Interference Avoidance :** Flash memory writes on ESP32 temporarily halt the CPU (10-100ms) and disable caches. This is a critical period for BLE, where missed timing anchors can cause supervision timeouts and disconnects. The task model mitigates this by:
 * **Deferral:** Non-critical NVS writes (e.g., `HistoryService` auto-flush) are deferred when BLE is connected. They only occur on disconnect or when the buffer is critically full.
 * **Yielding:** Worker tasks explicitly call `esp_task_wdt_reset` and `vTaskDelay` before and after blocking flash operations. This provides "breathing room" for the NimBLE stack (on Core 0) to process pending radio events and prevents watchdog timeouts.

 **BLE Stability Protections:**
 * **Asynchronous Processing:** All BLE event handling, data processing, and protocol operations are designed to be asynchronous. `ProtocolHandler`'s `sendWithAck` uses Promises, ensuring that heavy application logic triggered by callbacks does not block the native BLE stack from sending ACKs or subsequent packets.
 * **Security-Aware Handling:** The system handles common BLE security race conditions (e.g., encryption not ready during handshake, `CBError.peerRemovedPairingInformation` from abrupt device sleep) by whitelisting essential handshake messages (firmware) or by classifying false-positive errors (`BluetoothHandler` in app) to prevent incorrect auto-forgetting of devices.

 **Crash / Recovery Considerations :**
 * **NVS Corruption:** The `main.cpp` boot sequence includes `performCrashRecoveryCheck` which uses an `RTC_NOINIT_ATTR` crash counter. After `CRASH_RECOVERY_THRESHOLD` (3) consecutive watchdog resets, it performs an emergency `nvs_flash_erase` *before* attempting NVS initialization. This breaks boot loops caused by corrupted NVS partitions.
 * **Permanent Sleep:** `PowerManager` ensures sensor configuration for deep sleep is verified. If the sensor is unreliable or fails, a failsafe timer (30 seconds for emergency, 2 hours for backup) guarantees the device will wake up, re-initialize, and attempt recovery.
 * **RTC Integrity:** `HistoryService`'s `RTCState` is protected by a CRC32 checksum and `magicNumber` for cold boot detection. If corrupted, it attempts to restore from NVS backup.

 **Concurrency Hazard Mitigation:**
 * **Mutexes & Atomics:** Systematically eliminate race conditions on shared mutable state. By using mutexes (for complex structures) and atomics (for simple variables) in `HistoryService`, `ProtocolHandler`, `ConfigService`, `PersonalizationService`, and `Sensor_HAL`, the system guarantees data consistency across tasks and cores.
 * **Single-Writer Principle:** Key data paths, such as NVS and SPIFFS writes, enforce a single-writer pattern (each worker task is the sole writer to its owned data store), simplifying concurrency management for writes.

## 10. Operational Invariants
These are non-negotiable rules for maintaining the integrity and reliability of the firmware's concurrency model.

* **Non-Blocking Sensing Path:** The `SensorTask` (P10, Core 1) must *never* execute blocking I/O, perform indefinite computations, or acquire locks that would delay its 20Hz execution cycle.
* **Deferred Persistence Rule:** All blocking data persistence operations (NVS, SPIFFS writes) must be strictly offloaded to dedicated low-priority worker tasks and protected by appropriate synchronization.
* **Explicit Ownership Rule:** Every shared mutable resource must have a clearly defined owner responsible for write access. All non-owner access must use documented synchronization primitives (`Semaphore`, `std::atomic`) or operate on immutable copies/snapshots.
* **Synchronization Rule:** Any access to shared mutable state across different tasks or cores must *always* be protected by appropriate FreeRTOS primitives (mutexes) or `std::atomic` variables.
* **Queue Boundary Rule:** Queues define clear boundaries for work ownership transfer. Data passed through queues should be copied, not passed by reference, to avoid dangling pointers or unexpected modifications.
* **Idempotency Rule:** Any operation that may be retried (e.g., NVS writes, message sends) must be idempotent to prevent corrupted state or duplicate side effects if executed multiple times.

## 11. Related Documents
* `architecture.md`: Broader system architecture, including firmware/app interaction.
* `ble-protocol.md`: Detailed specification of the BLE application-layer protocol.
* `power-state-machine.md`: Comprehensive overview of power states and transitions.
* `ota-recovery.md`: Details the OTA update process and recovery mechanisms.
* `failure-modes.md`: Catalog of known failure modes and their mitigation strategies.
