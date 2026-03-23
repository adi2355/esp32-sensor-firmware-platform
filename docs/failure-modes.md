# Failure Modes and Recovery Design

> How the firmware and mobile application detect, contain, and recover from real-world failures across connectivity, storage, power, sensing, protocol, and update paths.

## 1. Purpose
This document systematically details the critical failure modes identified and mitigated within thefirmware and mobile application. It aims to demonstrate the robustness, resilience, and deliberate reliability engineering principles guiding the system's design, particularly for unattended, battery-powered, and long-running operation in adverse conditions. It serves as a comprehensive map for technical reviewers, future maintainers, and product stakeholders.

## 2. Reliability Philosophy
The the system is designed with an inherent understanding that failures are not exceptional events but expected inputs. Our core philosophy is grounded in:
-   **Fail-Safe over Silent Corruption:** Prioritizing system cessation or explicit error states over silently operating with corrupted data.
-   **Bounded Retries over Infinite Loops:** All retry mechanisms are bounded by counts or timeouts to prevent resource exhaustion and deadlocks.
-   **Data Integrity over Temporary Availability:** Critical data persistence is layered (RTC for deep sleep, NVS for power loss) and protected by checksums, ensuring data survival even if temporary service disruption occurs.
-   **Isolation of Blocking Work:** Time-critical, real-time operations (BLE, sensor polling) are strictly isolated from blocking I/O (flash writes, complex computations) using FreeRTOS task separation and asynchronous queues.
-   **Recoverable Rollback over Risky Forward Progress:** For system-critical operations like OTA updates, a robust rollback mechanism is prioritized over attempting forward progress with potentially corrupted state.
-   **Graceful Degradation:** When full recovery is impossible due to hardware or unrecoverable external conditions, the system degrades to a safe, observable state (e.g., emergency sleep mode, explicit error flags).

## 3. How to Read This Document
This document is structured to provide both a high-level overview and detailed technical insights.
-   **Entry Format:** Each failure mode follows a consistent template: Domain, Failure Mode, Trigger, Impact, Detection, Mitigation, Recovery Path, Residual Risk/Limits, and Related Docs.
-   **Domain Grouping:** Failures are grouped by major architectural domains for logical coherence.
-   **Cross-References:** Links to other documentation (`architecture.md`, `ble-protocol.md`, etc.) provide deeper context.

## 4. Failure Taxonomy Overview
| Domain                        | Representative Failures                                   | Primary Consequence          | Typical Response Pattern                                  |
| :---------------------------- | :-------------------------------------------------------- | :--------------------------- | :-------------------------------------------------------- |
| **Concurrency & RTOS**        | BLE starvation during flash writes, Task lifecycle races  | Unresponsiveness, Crashes    | Task isolation, Mutexes/Queues, Watchdogs                 |
| **BLE & iOS Compatibility**   | Bond mismatch, RPA issues, Connection timeouts           | Disconnects, Pairing failures| Deferred operations, Explicit fault reasons, Auto-reconnect |
| **Protocol & Delivery**       | CRC mismatch, ACK timeout, Channel saturation             | Data loss, Unresponsiveness  | Checksums, Retries, Panic Governor                        |
| **Storage & Data Integrity**  | NVS corruption, RTC corruption, Event ID rollback       | Data loss, Boot loops        | Checksums, Backup, Preemptive erase, Fast-forward         |
| **Power & Lifecycle**         | Permanent sleep, Brownout, Phantom wake, Idle-sleep races | Battery drain, Bricked device| Failsafe timers, Sleep guards, Calibrated sensing         |
| **Sensor & Signal Quality**   | Glass pseudo-detections, Stuck-detection, Calibration drift           | False positives, Missed detections | Multi-level thresholds, Dynamic adaptation, Auto-recal    |
| **OTA & Firmware Update**     | Corrupt image, Incomplete transfer, Brick prevention      | Bricked device, Data loss    | Rollback, Self-test, Flasher mode, Multipart protocol     |
| **Time & Offline Continuity** | Timestamp discontinuity, Offline backlog coherence       | Inaccurate analytics         | Persistent uptime, Epoch backfill, Forward-chaining       |
| **ML & Personalization**      | Invalid sample windows, Inference resource contention     | Inaccurate classification    | Bounded queues, Task isolation, NVS-backed prototypes     |
| **App-Specific Persistence**  | Reinstall detection, Keychain wipe failures              | Stale data, Security risks   | Sentinel files, Reset guards, Secure storage levels       |

## 5. Cross-Cutting Safeguards
Many failure modes are mitigated by architectural patterns and reusable mechanisms designed into thesystem:

-   **Task Isolation & Non-Blocking I/O (Firmware):** Critical real-time tasks (`SensorTask`, `ProtocolTask`) operate at higher priorities with minimal blocking. Slow I/O operations (`NVS` writes, `SPIFFS` writes) are offloaded to dedicated low-priority worker tasks (`StorageWorker`, `ConfigWorker`, `LoggerTask`) that communicate via FreeRTOS queues. This prevents long-latency operations from starving the BLE stack or missing sensor data.
-   **Checksums, ACK, Retry, & Sequencing (Firmware & App):** The binary protocol incorporates CRC16 for frame integrity, sequence numbers for ordering, and an ACK/NACK mechanism with exponential backoff for guaranteed delivery. This guards against data corruption, loss, and reordering.
-   **Tiered Persistence & Integrity Checks (Firmware):** Data critical for deep sleep survival (e.g., `DetectionEvent` ring buffer, sensor calibration) resides in `RTC_DATA_ATTR` memory, protected by CRC32. For power-loss resilience, this is backed up to NVS flash. Both layers are validated on boot.
-   **Watchdogs, Panic, & Boot Recovery (Firmware):** The `ESP32-S3` Task Watchdog Timer (TWDT) is actively managed. Critical failures trigger `systemPanic()` (halting the device with visual error feedback and continuous watchdog feeding). `performCrashRecoveryCheck()` uses an `RTC_NOINIT_ATTR` crash counter to detect NVS corruption boot loops and *preemptively erase NVS* after a threshold of consecutive crashes.
-   **Sleep Failsafes & Wake Guarantees (Firmware):** Deep sleep is protected by comprehensive checks (`canSafelyEnterSleep()`, `isProximityBelowThreshold()`, `digitalRead(Pins::PROXIMITY_INT)`). A failsafe timer wakeup (`ESP_SLEEP_WAKEUP_TIMER`) is *always* enabled as a backup to primary GPIO/sensor wake sources, preventing "permanent sleep" even if the sensor fails. GPIO states are explicitly held low (`gpio_hold_en`) to prevent spurious LED activation during sleep.
-   **Rollback & Self-Test Strategy (Firmware):** After an OTA update, the new firmware enters a `PENDING_VERIFY` state. A `performSelfTest()` routine runs to check hardware components, heap, NVS, and partitions. If tests fail, `esp_ota_mark_app_invalid_rollback_and_reboot()` automatically reverts to the previous stable firmware.
-   **Native App Lifecycle & State Restoration (App):** iOS's `CBCentralManagerOptionRestoreIdentifierKey` and `AppDelegate.mm`'s early `DeviceBLEInitializer.initializeBLECore()` ensure `willRestoreState` is handled, allowing the app to seamlessly restore BLE connections in the background. `AppState` listeners reconcile connection state on foreground.
-   **Bounded Event Buffering & Overflows (App):** The native BLE module buffers events (`EventBuffer`) when JavaScript listeners are not active. A fixed maximum size prevents unbounded memory growth. On overflow, the oldest event is dropped, and an `onBufferOverflow` fault event is emitted (and buffered if no listeners) to notify JS about potential data loss, enabling recovery.
-   **Secure Storage Accessibility (App):** `SecureStorageService` explicitly uses `SecureStore.AFTER_FIRST_UNLOCK` for `PUBLIC` and `PRIVATE` data, ensuring Keychain items are accessible even when the screen is locked or the app is backgrounded, crucial for BLE background operation.

## 6. Detailed Failure Modes

### 6.1 Concurrency and RTOS Failures

#### BLE Starvation During Flash Writes
-   **Domain:** Concurrency & RTOS, BLE & iOS Compatibility
-   **Failure Mode:** BLE connection drops due to `supervision timeout` (`Error 201`/`CBError.connectionTimeout`) during flash write operations.
-   **Trigger/Conditions:**
    1.  Firmware performs a blocking flash write (e.g., NVS save, SPIFFS write, OTA chunk).
    2.  Flash write halts the CPU cache and disables interrupts for `10-100ms`.
    3.  During this period, the `NimBLE` stack (running on Core 0) cannot service radio interrupts or send `BLE Link Layer` keep-alives.
    4.  The connected phone (iOS/Android) does not receive packets within the `supervision timeout` interval and terminates the connection.
-   **Impact:** Frequent, unpredictable BLE disconnects, leading to poor user experience, data sync failures, and potential auto-reconnect loops.
-   **Detection:**
    *   **Firmware:** `ESP_LOGE(TAG, "NVS write failed")` after retries. Logs of `IWDT` (Interrupt Watchdog) panics if critical sections are held during flash.
    *   **App:** `BleError.DeviceDisconnected`, `BleErrorCode.ConnectionTimeout`, or `Error 201` during active connection/handshake.
-   **Mitigation:**
    1.  **Task Decoupling (Firmware):** All blocking NVS/SPIFFS write requests are sent to dedicated low-priority worker tasks (`StorageWorker`, `ConfigWorker`, `LoggerTask`) via FreeRTOS queues. The `SensorTask` and `ProtocolTask` are never blocked by flash I/O.
    2.  **Tiered Flush Strategy (Firmware):** `HistoryService` defers non-critical NVS flushes when `BLE` is connected. Flushes occur only:
        *   Immediately if the event buffer is critically full (e.g., `90%`).
        *   On `BLE` disconnect (when the radio is idle).
        *   Before deep sleep (ensured by `PowerManager`).
    3.  **Connection-Aware Yielding (Firmware):** The `StorageWorker` task introduces tiered delays (`vTaskDelay`) before and after flash operations. Longer delays (`100-150ms`) are used during the initial 4-second `BLE` handshake window to prioritize critical `HELLO/SYNC` packets, reducing disconnects.
    4.  **MTU Optimization (Firmware & App):** Firmware requests `MTU=247`, app configures `MTU_REQUEST_SIZE=247`. Larger `MTU` reduces the number of packets for large transfers, minimizing flash write frequency.
-   **Recovery Path:**
    *   **Firmware:** Continues operating; the event is preserved in `RTC` memory and will be flushed to `NVS` later.
    *   **App:** `BluetoothHandler` detects disconnect, clears state, and initiates `attemptReconnectionWithBackoff()` (which uses native `autoConnect`).
-   **Residual Risk/Limits:** While mitigated, very short `BLE` connection intervals combined with extremely long flash operations (not typical for `NVS`) could still induce minor jitter. Heavy write operations (e.g., very rapid ML data logging to `SPIFFS`) can still cause queue backpressure if not handled correctly.
-   **Related Docs:** `architecture.md` (Service-Oriented Embedded Architecture, FreeRTOS Multi-Task), `storage-integrity.md`, `ble-protocol.md`.

#### FreeRTOS Task Deletion Race Condition
-   **Domain:** Concurrency & RTOS
-   **Failure Mode:** `Guru Meditation Error: StoreProhibited` at `vListInsert` (FreeRTOS list corruption) when entering deep sleep.
-   **Trigger/Conditions:** A FreeRTOS task attempts to self-delete (`vTaskDelete(nullptr)`) while another part of the system attempts to delete the same task via its handle (`vTaskDelete(handle)`).
-   **Impact:** Application crashes and reboots. Could lead to a boot loop if this is a recurring state.
-   **Detection:** `Guru Meditation Error: Core 1 panic'ed (StoreProhibited)` in serial logs, with `PC` pointing to FreeRTOS list manipulation functions.
-   **Mitigation:** **Single Point of Deletion Pattern.** For critical worker tasks (e.g., `StorageWorker`), the task function (`storageWorker()`) enters an infinite loop. When signaled to stop (`_storageWorkerRunning=false`), it enters an idle state (feeding watchdog, delaying) but *never* calls `vTaskDelete(nullptr)` on itself.
-   **Recovery Path:** The managing service (`HistoryService::stopStorageWorker()`) is the *only* entity that calls `vTaskDelete(_storageTaskHandle)`. This ensures that the task is deleted precisely once and only when its state is idle.
-   **Residual Risk/Limits:** This pattern is robust but requires careful adherence by all task managers. If a task accidentally self-deletes, it can still lead to memory corruption.
-   **Related Docs:** `architecture.md` (FreeRTOS Multi-Task Architecture).

#### BLE Stack Overflow
-   **Domain:** Concurrency & RTOS, BLE & iOS Compatibility
-   **Failure Mode:** `Guru Meditation Error: Core 1 panic'ed (StoreProhibited)` during NVS operations within the `NimBLE` host task callback or main loop.
-   **Trigger/Conditions:**
    1.  Large payloads are being processed (e.g., OTA chunks, sync events).
    2.  `NimBLE` host task stack size is insufficient for internal operations + application callbacks.
    3.  Blocking `NVS` operations (which use internal buffers) are called from a task with limited stack.
-   **Impact:** Application crashes and reboots. Can lead to unstable `BLE` connections.
-   **Detection:** `Guru Meditation Error: Core 1 panic'ed (StoreProhibited)` with `PC` pointing to `nvs_flash_` functions or `vListInsert`.
-   **Mitigation:**
    1.  **Increased NimBLE Host Stack (Firmware):** `platformio.ini` sets `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192`.
    2.  **Increased Application Task Stacks (Firmware):** `Config.h` sets `PROTOCOL_TASK_STACK_SIZE=16384` and `STORAGE_TASK_STACK_SIZE=16384`. These account for `NVS` overhead within worker tasks and the main `Arduino loopTask`.
    3.  **Shared TX Buffer (Firmware):** `ProtocolHandler` uses a single `_txFrameBuffer` for building outgoing frames, avoiding large stack allocations in `NimBLE` callbacks.
-   **Recovery Path:** Device reboots via watchdog, potentially recovers (if NVS not corrupt).
-   **Residual Risk/Limits:** Future features with large stack requirements could reintroduce this. Careful stack usage profiling (`uxTaskGetStackHighWaterMark`) is required during development.
-   **Related Docs:** `architecture.md` (FreeRTOS Multi-Task Architecture).

### 6.2 BLE and Bonding Failures

#### Bond Mismatch / Ghost Bond
-   **Domain:** BLE & iOS Compatibility, Storage & Data Integrity
-   **Failure Mode:**
    1.  **Ghost Bond:** App "forgets" device (iOS Settings → Forget), but ESP32 retains bond record. On next connection, iOS uses a new `RPA`, ESP32 can't resolve it or authenticate the link.
    2.  **Security Standoff:** ESP32 has bonds, iOS presents `RPA`. ESP32 waits for iOS to initiate security (via `Write Rejected`). iOS expects `ESP32` to initiate `RPA` resolution/encryption. Neither initiates, connection remains unencrypted.
-   **Trigger/Conditions:**
    1.  User "forgets" device in iOS/Android settings without sending a `CLEAR_BONDS` command to the ESP32.
    2.  ESP32 is reset/reflashed (clearing NVS `ble_sec` partition), but the app retains the old bonding keys.
    3.  iOS `RPA` rotation combined with `NimBLE`'s late `IRK` resolution.
-   **Impact:**
    *   Connection remains unencrypted (security vulnerability).
    *   App disconnects (Error 201) due to security failures, leading to reconnect loops.
    *   Device fails to accept new pairings because it thinks it's already "owned" (soft-locked).
    *   `False positive` bonding errors (`CBError.peerRemovedPairingInformation`) for device sleep.
-   **Detection:**
    1.  **Firmware:** `g_consecutive_bond_failures` (RTC counter) tracks consecutive wake cycles with existing bonds but no successful encrypted connection. Triggers emergency pairing.
    2.  **Firmware:** `BLEManager::onAuthenticationComplete()` logs "Authentication failed - Bonded: No, Encrypted: No". `BLEManager::onConnectInternal()` logs `sec_state.bonded` (often `false` for `RPA`).
    3.  **App:** `BluetoothContext.tsx` detects `shortConnectionBondFailureCount` (disconnects <4s after READY), `consecutiveHandshakeFailures` (disconnects during handshake), and specific `BleError` codes (`peerRemovedPairingInformation`, `encryptionTimedOut`).
-   **Mitigation:**
    1.  **Bilateral Bond Clearing Protocol (App & Firmware):** App sends `MSG_CLEAR_BONDS` to the ESP32 before forgetting locally. ESP32 calls `NimBLEDevice::deleteAllBonds()` to clear its NVS bond storage.
    2.  **Orphaned Bond Detection (Firmware):** `BLEManager::init()` checks `g_consecutive_bond_failures`. If threshold (5) reached, `deleteAllBonds()` + `enablePairingMode()` are called automatically on boot.
    3.  **Proactive Security Initiation (Firmware):** `BLEManager::onConnectInternal()` proactively calls `NimBLEDevice::startSecurity()` if *no bonds exist* (fast first-time pairing). If bonds *do* exist, it *defers* security to iOS (allows `RPA` resolution). `BLEManager::update()` has a `Security Standoff Watchdog` (2.5s unencrypted) to force initiation if iOS doesn't.
    4.  **Security-Aware Whitelist (Firmware):** `MSG_HELLO`, `MSG_ACK`, `MSG_HEARTBEAT` bypass encryption check (`BLEManager::onWrite`) to allow handshake to progress in parallel with encryption.
    5.  **Post-Handshake Delay (App):** `EventSyncService` waits `6000ms` after `HELLO_ACK` before sending `MSG_GET_CONFIG`, giving iOS sufficient time for encryption to complete.
    6.  **"Ghost Bond" Disconnect Detection (App):** `BluetoothContext.tsx` tracks `shortConnectionBondFailureCount`. If `MAX_SHORT_CONNECTION_BOND_FAILURES` (2) reached, `handleBondingError()` is triggered to clear local state and guide user.
    7.  **Sleep Disconnect Override (Firmware & App):** `PowerManager::enterDeepSleep()` sets `isSleepDisconnect` flag. `BLEManager::classifyDisconnectReason()` prioritizes `deviceSignaledSleep` over `CBError.peerRemovedPairingInformation` to prevent false bonding errors for sleep.
-   **Recovery Path:**
    *   **Firmware:** Auto-clears stale bonds, enables pairing mode.
    *   **App:** `handleBondingError()` disconnects, removes device from local storage, clears failure counters, and prompts user to "Forget This Device" in iOS Bluetooth settings.
-   **Residual Risk/Limits:** User must still manually "Forget This Device" in iOS settings. Without this, iOS may indefinitely retain cached keys, causing future issues.
#### Reconnection Death Loop (Race Condition)
-   **Domain:** BLE & iOS Compatibility, Concurrency & RTOS
-   **Failure Mode:** Device repeatedly connects and disconnects in a rapid loop, unable to stabilize.
-   **Trigger/Conditions:** A race condition where a "ghost" cleanup from a previous failed connection attempt interferes with a newer, in-progress connection attempt.
    1.  Connection A drops (e.g., during heavy processing in JS main thread).
    2.  `BluetoothHandler::handleSilentDisconnection()` (for Connection A) gets delayed.
    3.  Meanwhile, the app's auto-reconnect logic initiates Connection B.
    4.  Connection B begins establishing (`ConnectionState.CONNECTING`).
    5.  The delayed `handleSilentDisconnection()` for Connection A eventually fires.
    6.  It attempts to `cancelDeviceConnection()` for the currently active Connection B, killing it.
    7.  This triggers another disconnect, leading to Connection C, and the loop continues.
-   **Impact:** Device remains perpetually disconnected, consuming significant battery (from constant radio activity), and preventing any user interaction or data sync.
-   **Detection:**
    *   Rapid succession of connect/disconnect events in logs.
    *   `BluetoothContext.tsx` logs `Stale connection attempt detected` or `Silent disconnection ignored - connection attempt in progress`.
-   **Mitigation:**
    1.  **Connection Attempt ID (App):** `BluetoothHandler` assigns a unique `currentConnectionAttemptId` to each `connectToDevice()` call.
    2.  **Race Condition Guards (App):** `BluetoothHandler::handleSilentDisconnection()` and `connectToDevice()` include checks:
        *   If `ConnectionState === ConnectionState.CONNECTING` or `HANDSHAKING`, `handleSilentDisconnection()` aborts early, not interfering with the new attempt.
        *   During `connectToDevice()`, the `currentConnectionAttemptId` is verified repeatedly throughout the async connection process (after `connectToDevice()`, after `discoverAllServicesAndCharacteristics()`, before final state update). If the ID mismatch, the current attempt is aborted.
-   **Recovery Path:** The explicit ID tracking prevents the loop. Once a connection attempt fails or succeeds, its ID is recorded, preventing stale cleanup calls from impacting future attempts. The system's general auto-reconnection logic then attempts to establish a stable connection.
-   **Residual Risk/Limits:** Extremely rare edge cases of CPU starvation or OS scheduling delays could still theoretically create brief windows for interference, but the ID-based guards make this highly improbable.
-   **Related Docs:** `ble-protocol.md`, `architecture.md`.

### 6.3 Protocol and Delivery Failures

#### Protocol Desynchronization (Phantom ACKs)
-   **Domain:** Protocol & Delivery Integrity
-   **Failure Mode:** The device receives an `ACK` message that does not correspond to any currently pending outgoing message.
-   **Trigger/Conditions:**
    1.  **Delayed ACK:** The app sends an `ACK` for a message, but the device times out, retransmits, and then eventually receives the `ACK` (after the message was already cleared from `pendingMessages`).
    2.  **Stale ACK after Reconnect:** The app has `ACKs` queued from a previous session. After a disconnect/reconnect, these stale `ACKs` are sent by the app but don't match any `pendingMessages` in the new session.
    3.  **Malicious Injection (low probability):** An attacker injects a fake `ACK` with a random sequence number.
-   **Impact:**
    *   **Firmware:** If `_invalidAckCount` exceeds `INVALID_ACK_THRESHOLD` (10), `handleSecurityThresholdExceeded()` is called. In old firmware, this called `clearAllPending()`, destroying valid sync data and causing `sync failures`.
    *   **App:** Protocol service might log warnings or, if not handled, incorrectly mark messages as acknowledged.
-   **Detection:** `ProtocolHandler::validateAckSecurity()` checks `_pendingMessages`. If no match, increments `_invalidAckCount`.
-   **Mitigation:**
    1.  **Increased ACK Timeout (Firmware & App):** `PROTOCOL_ACK_TIMEOUT_MS` increased to `6000ms` (from `3000ms`) to provide ample time for `iOS` background processing. This reduces legitimate delayed `ACKs`.
    2.  **ACK Debounce (App):** `EdgeDeviceProtocolService::sendAck()` debounces duplicate ACKs for the same sequence number within a `500ms` window, preventing firmware security violations.
    3.  **Phantom ACK Policy (Firmware):** `ProtocolHandler::handleSecurityThresholdExceeded()` *no longer calls `clearAllPending()`*. It now only resets `_invalidAckCount` and logs a warning. This prevents destruction of valid `pendingMessages`.
    4.  **Session Reset (Firmware):** `ProtocolHandler::resetForNewConnection()` (called on `BLEManager::onBLEConnect`) clears `_invalidAckCount` and `_lastSecurityEvent` on new connections, preventing accumulation of stale ACKs across sessions.
-   **Recovery Path:** The system gracefully ignores phantom `ACKs` or, for legitimate delayed `ACKs`, the retry mechanism ensures delivery. Sync data remains intact.
-   **Residual Risk/Limits:** While benign, frequent phantom `ACKs` can generate log spam. Malicious `ACK` injection is highly improbable on a bonded `BLE` link.
-   **Related Docs:** `ble-protocol.md`.

#### Channel Saturation / Panic Governor
-   **Domain:** Protocol & Delivery Integrity, Sensor & Signal Quality
-   **Failure Mode:** `BLE` channel becomes saturated with excessive outgoing messages, starving incoming `ACKs` and `MSG_SET_CONFIG` commands.
-   **Trigger/Conditions:**
    1.  User sets sensor sensitivity too high, causing constant false positive detections.
    2.  Device is placed on a vibrating/noisy surface, triggering rapid `DETECTION_EVENT` bursts.
    3.  A software bug causes rapid `DETECTION_EVENT` generation.
-   **Impact:**
    *   `BLE` radio is overwhelmed, cannot receive `ACKs` for outgoing messages.
    *   App cannot send `MSG_SET_CONFIG` (to correct sensitivity) or `MSG_ACK`.
    *   Connection drops (supervision timeout).
    *   Device becomes unresponsive (locked out until physical reset).
-   **Detection:** `ProtocolHandler::_detectionEventCount` and `_detectionWindowStart` track the number of `DETECTION_EVENTs` within a sliding time window.
-   **Mitigation:**
    1.  **Panic Governor (Firmware):** `ProtocolHandler::sendDetectionEvent()` implements a rate limit: `MAX_DETECTIONS_PER_WINDOW` (5) per `DETECTION_RATE_WINDOW_MS` (1000ms). Excess `DETECTION_EVENTs` are *dropped* (not queued) to prioritize `BLE RX` bandwidth.
    2.  **Sensor Suppression (Firmware):** If `Panic Governor` triggers, `Sensor_HAL::suppressDetections(1000)` is called for 1 second. This reduces CPU load and prevents sensor from hammering the protocol stack.
    3.  **Governor Bypass for Sync (Firmware):** `ProtocolHandler::sendDetectionEvent(..., bypassGovernor=true)` for `MSG_SYNC_REQUEST` responses (historical data replay), preventing data loss during sync bursts.
-   **Recovery Path:** `Panic Governor` temporarily limits throughput, allowing `BLE RX` to clear and eventually receive `MSG_SET_CONFIG` or `ACKs`. `Sensor` suppression further aids recovery.
-   **Residual Risk/Limits:** Dropped `DETECTION_EVENTs` are preserved in `HistoryService` and will be synchronized later. The user might perceive a delay in "live" detection updates if the governor is active.
-   **Related Docs:** `ble-protocol.md`, `sensor-signal-quality.md`.

### 6.4 Storage and Data Integrity Failures

#### RTC Memory Corruption
-   **Domain:** Storage & Data Integrity, Power & Lifecycle
-   **Failure Mode:** Data stored in `RTC_DATA_ATTR` memory (e.g., `HistoryService` ring buffer, `rtcCalibration`) becomes corrupted.
-   **Trigger/Conditions:**
    1.  Extreme power fluctuations (e.g., rapid brownouts, voltage spikes).
    2.  Rare hardware faults affecting `RTC` memory.
    3.  Software bugs overwriting `RTC` memory (less likely with `RTC_DATA_ATTR`).
-   **Impact:** Loss of ephemeral session data (e.g., unsynced detections). `CalibrationData` can become invalid, leading to incorrect sensor thresholds and preventing wake from sleep.
-   **Detection:** `HistoryService::init()` and `main.cpp::loadCalibrationFromRTC()` perform `CRC32` checksum validation on `RTCState` and `CalibrationData` structs respectively. They also check a magic number (`HistoryConfig::RTC_MAGIC_NUMBER`) to distinguish corruption from a clean cold boot.
-   **Mitigation:**
    1.  **CRC32 Validation:** All `RTC` data blocks (events, calibration) include a `CRC32` checksum. Data is considered invalid if `CRC` mismatch.
    2.  **NVS Backup:** `HistoryService` periodically flushes its `RTC` ring buffer to `NVS` (Tier 2 persistence). `CalibrationData` is written to `NVS` implicitly via `ConfigService`.
    3.  **Sanity Checks:** `main.cpp::isThresholdSane()` validates `RTC` calibration values (wake, detection, critical thresholds) to prevent loading logically `insane` data (e.g., `wakeThreshold=5000` from bad calibration).
    4.  **Battery Guard:** `HistoryService::flushToNvs()` (firmware) skips NVS writes if `isBatteryCritical()` to prevent brownout during flush.
-   **Recovery Path:**
    1.  If `RTC` data is corrupt, `HistoryService` attempts to `restoreFromNvs()`.
    2.  If `NVS` restore fails (or no backup), `RTC` data is reset to factory defaults.
    3.  For `CalibrationData`, if corrupt or insane, defaults are applied.
-   **Residual Risk/Limits:** If both `RTC` memory *and* the `NVS` backup are simultaneously corrupted or inaccessible, data is permanently lost. This is a rare, severe hardware-level failure.
-   **Related Docs:** `storage-integrity.md`, `power-state-machine.md`.

#### App Reinstallation (Stale Data)
-   **Domain:** App-Specific Persistence, Storage & Data Integrity
-   **Failure Mode:** After an app reinstall, stale persistent data (Keychain, old SQLite DB files, AsyncStorage) from the previous installation leads to app crashes or incorrect behavior.
-   **Trigger/Conditions:**
    1.  User installs the app.
    2.  User uses the app, generating Keychain entries (e.g., auth tokens), SQLite DBs, AsyncStorage.
    3.  User uninstalls the app. iOS deletes the app sandbox (Documents, Library) but **retains Keychain data**.
    4.  User reinstalls the app. The new sandbox is clean, but Keychain still contains old data.
-   **Impact:**
    *   App crashes immediately on startup when encountering stale authentication tokens in Keychain.
    *   Incorrect application state due to loading old data.
    *   Security vulnerabilities if stale authentication tokens are re-used.
    *   Bluetooth connection failures in background if `SecureStore` cannot access keys.
-   **Detection:** `AppDelegate.mm` calls `ReinstallDetector.checkForReinstall()` at launch.
    *   **Keychain Marker:** A specific entry in Keychain (`keychainService`, `keychainAccount`) persists across uninstalls.
    *   **Sandbox Sentinel File:** A hidden sentinel file in the app's `Documents` directory is deleted on uninstall.
    *   **Reinstall Condition:** Keychain marker exists `AND` sentinel file is missing.
-   **Mitigation:**
    1.  **Factory Reset Guard:** `FactoryResetGuard.swift` limits factory reset attempts (`maxAttempts=3`) per app build using `UserDefaults` to prevent infinite reset loops if wiping fails partially.
    2.  **Full Data Wipe:** On `reinstall` detection, `AppDelegate.mm` orchestrates a comprehensive wipe:
        *   `KeychainWipeModule.wipeAllKeychainItems()`: Deletes ALL Keychain items (GenericPassword, InternetPassword, etc.) for the app.
        *   `FactoryResetModule.wipeSQLiteDatabase()`: Deletes the entire `Documents/SQLite` directory (including WAL/SHM files) and legacy DB files.
        *   `FactoryResetModule.wipeAsyncStorage()`: Deletes `RCTAsyncLocalStorage` directories.
    3.  **Partial Failure Handling:** If the wipe fails partially (e.g., `Keychain` wipe succeeds but `SQLite` fails), `FactoryResetGuard` *re-seeds the Keychain marker* while leaving the sentinel absent. This ensures the reinstall is detected again on the next launch, allowing retry (up to `maxAttempts`).
    4.  **Secure Storage Accessibility (iOS):** `SecureStorageService.ts` explicitly sets `keychainAccessible: SecureStore.AFTER_FIRST_UNLOCK` for `PUBLIC` and `PRIVATE` sensitivity levels. This allows `Keychain` access even when the app is backgrounded or the screen is locked, enabling background BLE operations (e.g., retrieving `deviceId` for sync).
-   **Recovery Path:** The app performs an atomic factory reset, returning to a clean "first launch" state. The user must log in again and re-pair devices.
-   **Residual Risk/Limits:** If `Keychain` itself becomes corrupted or inaccessible (rare iOS hardware/OS bug), the detection mechanism might fail. The user must still explicitly "Forget This Device" in iOS Bluetooth settings if a `BLE` bonding mismatch occurs (`handleBondingError()` in `BluetoothContext.tsx`).
### 6.5 Power, Sleep, and Wake Failures

#### Permanent Sleep (Failed Primary Wake)
-   **Domain:** Power & Lifecycle, Sensor & Signal Quality
-   **Failure Mode:** Device enters deep sleep but never wakes up, becoming soft-bricked until physical power cycle.
-   **Trigger/Conditions:**
    1.  The primary wake source (proximity sensor via `EXT1` interrupt) is misconfigured or fails.
    2.  An I2C bus latch-up, sensor hardware fault, or power glitch causes the sensor to stop triggering interrupts.
    3.  No backup wake source is configured.
-   **Impact:** Device is unusable, appears "dead" to the user, consuming minimal power but unresponsive. Requires manual intervention.
-   **Detection:** The lack of any wake event. In "Emergency Mode", the device wakes via timer, detects sensor config failure, and reboots.
-   **Mitigation:**
    1.  **Robust Sensor Configuration (Firmware):** `Sensor_HAL::configureForDeepSleep()` performs a "verify-after-write" pattern with up to 3 retries (with I2C bus recovery) to ensure the sensor is correctly configured for `EXT1` interrupt mode.
    2.  **Failsafe Timer Wakeup (Firmware):** `PowerManager::enterDeepSleep()` *always* enables `ESP_SLEEP_WAKEUP_TIMER` as a secondary wake source:
        *   **Standard Mode:** If sensor configuration `VERIFIED OK`, a `2-hour` timer is set (backup).
        *   **Emergency Mode:** If sensor configuration `FAILED`, a `30-second` timer is set (primary wake source), forcing a quick reboot to retry sensor initialization.
    3.  **Proximity Check Before Sleep (Firmware):** `PowerManager::enterDeepSleep()` calls `Sensor_HAL::isProximityBelowThreshold()` *before* `esp_deep_sleep_start()`. If proximity is *already elevated* (e.g., hand near sensor), sleep is *aborted* to prevent edge-triggered interrupts from never firing.
    4.  **"Last Second" GPIO Check (Firmware):** Just before `esp_deep_sleep_start()`, `PowerManager` checks `digitalRead(Pins::PROXIMITY_INT)`. If the sensor pin is `LOW` (active), sleep is *aborted*.
    5.  **GPIO Hold (Firmware):** `LED_HAL::prepareForSleep()` and `PowerManager::configureWakeSources()` use `gpio_hold_en()` and `rtc_gpio_pullup_en()` to maintain stable GPIO states (e.g., LED OFF, I2C bus pulled HIGH) during deep sleep, preventing unintended wakeups or component confusion.
-   **Recovery Path:**
    *   **Standard Mode:** If sensor fails during sleep, device wakes after 2 hours via timer, re-initializes hardware, and attempts to re-enter sleep.
    *   **Emergency Mode:** Device reboots after 30 seconds via timer, re-initializes hardware, and tries to configure sensor again. Visual feedback (3 red flashes) alerts the user to a hardware issue.
    *   **Proximity/GPIO Abort:** Device returns to `WAKE_MODE` to wait for the user to move their hand away, then retries sleep.
-   **Residual Risk/Limits:** Extremely rare `ESP32` hardware failure preventing any timer or `EXT1` wakeup. The 2-hour failsafe can feel long for the user, but guarantees eventual recovery.
-   **Related Docs:** `power-state-machine.md`, `sensor-signal-quality.md`.

#### Inaccurate/Critical Battery Monitoring
-   **Domain:** Power & Lifecycle
-   **Failure Mode:**
    1.  Inaccurate battery percentage reporting (e.g., "stuck at 100%" then rapid drop).
    2.  "Stuck charging" indicator when not charging.
    3.  Device enters brownout/reboot loop during flash write at low battery.
-   **Trigger/Conditions:**
    1.  Incorrect ADC calibration or voltage division compensation.
    2.  Non-linear LiPo discharge curve not accurately modeled.
    3.  `GPIO` pin conflicts (e.g., `GPIO 3` for charging LED defaults to `JTAG_T0`).
    4.  Low battery voltage combined with high current draw during `NVS` flash write operations.
-   **Impact:**
    *   Confusing user experience (misleading battery levels).
    *   Device appears to be constantly charging.
    *   Device becomes soft-bricked in a brownout reboot loop.
    *   `NVS` corruption or data loss during failed writes.
-   **Detection:**
    *   **Firmware:** `PowerManager::updateBatteryStatus()` reads calibrated voltage and logs status.
    *   **Firmware:** `PowerManager::isBatteryCritical()` checks for low battery state.
-   **Mitigation:**
    1.  **Calibrated ADC (Firmware):** `PowerManager::readBatteryVoltage()` uses `analogReadMilliVolts()` (ESP32's calibrated ADC) and `16x oversampling`. Multiplies by `2` to compensate for the 2:1 voltage divider.
    2.  **OCV→SoC Lookup Table (Firmware):** `PowerManager::voltageToPercent()` uses a non-linear `OCV_SOC_TABLE` (`3300mV`-`4200mV`) with linear interpolation, matching LiPo discharge characteristics.
    3.  **EMA Smoothing & Hysteresis (Firmware):** `PowerManager::updateBatteryStatus()` applies `EMA` smoothing (`SMOOTHING_ALPHA=0.15`) and `2% hysteresis` to reported percentage (`_batteryPercent`), preventing jitter and oscillation.
    4.  **GPIO 3 JTAG Fix (Firmware):** `PowerManager::init()` calls `gpio_reset_pin(Pins::CHARGING)` before `pinMode()`, detaching `GPIO 3` from `JTAG_T0` to correctly read the charging `IC` state.
    5.  **USB Power Heuristic (Firmware Flasher Mode):** `FlasherService::readBatteryVoltageInternal()` assumes `USB` power (returns `4200mV`) if measured voltage is `<1.5V` to prevent false low-battery alerts when no battery is connected.
    6.  **Low Battery NVS Guard (Firmware):** `HistoryService::flushToNvs()` skips `NVS` writes if `isBatteryCritical()` and not `isCharging()`, preventing brownout during flash operations. Data is held in `RTC RAM` until charged.
-   **Recovery Path:** Device stops `NVS` writes, preserving data in `RTC`. User charges device. After recovery, battery status becomes accurate. Brownout loop is prevented.
-   **Residual Risk/Limits:** If `RTC` memory dies before user charges, any pending data is lost. The `USB` power heuristic could misinterpret certain faulty battery conditions.
-   **Related Docs:** `power-state-machine.md`, `storage-integrity.md`.

### 6.6 Sensor and Detection Failures

#### Sensor False Positives / Stuck Detection
-   **Domain:** Sensor & Signal Quality
-   **Failure Mode:**
    1.  **Glass Pseudo-detections:** Device logs false positive detections when placed on glass/reflective surfaces.
    2.  **Jitter:** Rapid detection/release cycles, multiple false positives due to sensor noise near a narrow hysteresis band.
    3.  **Stuck Detection:** Device remains in `DETECTION_ACTIVE` state (green `LED`) indefinitely when signal is stuck in hysteresis band.
-   **Trigger/Conditions:**
    1.  `IR` reflections from glass/reflective surfaces create weak, persistent proximity signals (`~100-150` units).
    2.  `Smart Noise Floor Clamp` compresses the `hysteresis gap` to almost zero, making sensor noise cross the threshold repeatedly.
    3.  High ambient baselines (e.g., from glass) combine with a `fixed hysteresis` (e.g., 20%) to push `releaseThreshold` *below* the actual ambient level.
-   **Impact:** Inaccurate detection logging, confusing `LED` behavior (`blue/green` flickering), battery drain (device stays awake), degraded user experience.
-   **Detection:**
    1.  **Multi-Level Thresholding (Firmware):** Calibration defines `WAKE_THRESHOLD_LOW`, `DETECTION_THRESHOLD_LOW` (3-sigma, starts detection timer, `LED` green), and `CRITICAL_DETECTION_THRESHOLD` (5-sigma, *confirms* detection). A detection is logged only if `duration >= MIN_DETECTION_DURATION_MS` AND `criticalThreshold` was reached.
    2.  **Gray Zone Watchdog (Firmware):** `handleDetection()` tracks if `proximityValue` is stuck in the hysteresis band (`[releaseThreshold, detectionThreshold)`) for `>5` seconds.
    3.  **Hysteresis Gap Monitoring (Firmware):** `Sensor_HAL::recalculateThresholdsFromSensitivity()` dynamically checks the `effective hysteresis gap`.
-   **Mitigation:**
    1.  **Multi-Level Thresholding (Firmware):** `MIN_DETECTION_DURATION_MS=1000ms`, `CRITICAL_Z_SCORE=5.0`. Requires both `duration` AND `strength` for logging, filtering weak glass reflections.
    2.  **Smart Noise Floor Clamp (Firmware):** `handleDetection()` ensures `releaseThreshold` never drops below `baseDetectionThreshold` when operating at `Normal/Low sensitivity`. If `DetectionThreshold >= BaseDetectionThreshold`, `releaseThreshold = MAX(rawRelease, BaseDetectionThreshold)`. This prevents "stuck detection" by ensuring release is always reachable.
    3.  **Bounded Hysteresis Gap (Firmware):** `Sensor_HAL::recalculateThresholdsFromSensitivity()` caps the `Schmitt Trigger` hysteresis gap at `MAX_HYSTERESIS_GAP=200` units. This prevents excessively large dead bands at high `detectionThresholds`.
    4.  **Proportional Critical Gap (Firmware):** `Sensor_HAL::recalculateThresholdsFromSensitivity()` uses a `PROPORTIONAL_GAP` (5% of `Hit` threshold, min `30` units) between `Hit` and `Critical` thresholds. This ensures a meaningful "Gray Zone" at all sensitivity levels.
    5.  **Minimum Hysteresis Gap Enforcement (Firmware):** If `effective gap < MINIMUM_HYSTERESIS_GAP` (50 units), `Sensor_HAL::applyEnvironmentBackoff()` is called immediately to raise thresholds. This is a proactive anti-jitter measure.
    6.  **Gray Zone Watchdog & Self-Healing (Firmware):** If `Gray Zone Watchdog` fires (`>5s` stuck), `sensorTask` triggers immediate `sensitivity backoff (+5 points)` via `Sensor_HAL::applyEnvironmentBackoff()` and forces `recalibration`.
    7.  **Stuck Detection Watchdog (Firmware):** Fallback `60-second` watchdog triggers `sensitivity backoff` and `recalibration` if `DETECTION_ACTIVE` state persists too long.
    8.  **Wide Dynamic Range Sensitivity (Firmware):** `recalculateThresholdsFromSensitivity()` allows `0-100%` sensitivity to map to `0.4x-3.0x` multipliers, providing fine-grained control for extreme environments (glass tables need low sensitivity).
-   **Recovery Path:** `Sensitivity backoff` and `recalibration` adjust thresholds to match the environment, breaking the stuck-detection/jitter loop. Logged detections are filtered by two-factor validation.
-   **Residual Risk/Limits:** Extremely subtle signal patterns or new highly reflective materials could still bypass filtering. Requires ongoing tuning with field data. `ML` classification (`InferenceService`) aims to further improve this by classifying actual gestures.
-   **Related Docs:** `sensor-signal-quality.md`, `architecture.md`.

### 6.7 OTA and Firmware Update Failures

#### Incomplete OTA (Firmware Brick)
-   **Domain:** OTA & Firmware Update
-   **Failure Mode:** Device becomes non-functional or `soft-bricked` after an `OTA` update due to a corrupted firmware image, power loss during write, or an invalid new firmware that crashes on boot.
-   **Trigger/Conditions:**
    1.  `Power loss` during firmware download or flash write.
    2.  `Corrupted firmware image` (e.g., download error, bad build).
    3.  `Incompatible firmware` (e.g., wrong version, wrong board).
    4.  `Buggy new firmware` that crashes immediately on startup.
    5.  `TCP timeout` during `OTA` due to blocking flash erase.
-   **Impact:** Device is unusable, requires manual intervention (reflash or power cycling for recovery). Loss of user trust.
-   **Detection:**
    1.  **`PENDING_VERIFY` State (Firmware):** After `OTA` flash, `esp_ota_set_boot_partition` marks the new partition as `PENDING_VERIFY`.
    2.  **`performSelfTest()` (Firmware):** On boot from a `PENDING_VERIFY` partition, `OTAManager::validateBootedFirmware()` runs a suite of self-tests (`I2C` sensor, heap, NVS, `LED`).
    3.  **`FlasherService` Watchdogs (Firmware):** `IDLE_TIMEOUT_MS` (5 min without activity), `PROGRESS_TIMEOUT_MS` (15s without bytes flowing). `2-stage battery checks`.
    4.  **`Multipart Protocol` (Firmware & App):** Ensures binary data integrity and proper handling by WebServer.
    5.  **`CRC32` & Size Check (Firmware):** `OTAManager` verifies `CRC32` and `totalSize` after all chunks received.
-   **Mitigation:**
    1.  **Dual-Bank OTA (Firmware):** Two `app` partitions (`app0`, `app1`) of equal size. Firmware is written to the *inactive* partition.
    2.  **Automatic Rollback (Firmware):** If `performSelfTest()` fails (a "hard" failure), `esp_ota_mark_app_invalid_rollback_and_reboot()` reverts to the previous known-good firmware.
    3.  **Flasher Mode (Firmware & App):**
        *   **Dedicated Environment:** `FlasherService` runs in an isolated mode (BLE/sensors OFF, Wi-Fi AP ON) for robust upload.
        *   **2-Stage Power Gating:** `FlasherService` performs battery checks at idle and under Wi-Fi load to prevent brownouts during upload.
        *   **Explicit Erase Protocol:** App POSTs to `/start` (triggering deferred flash erase) then polls `/status` for `RECEIVING` phase. Chunks are sent to `/update_chunk`. This decouples the blocking erase from network `TCP` stack.
        *   **Multipart Encoding:** App sends chunks as `multipart/form-data` (`buildMultipartBody()`) to prevent binary corruption on `ESP32 WebServer`.
        *   **Chunked Upload:** Firmware sent in `4KB` chunks (`CHUNK_SIZE`) with `INTER_CHUNK_DELAY_MS` to prevent `TCP` window exhaustion. Max `3` retries per chunk.
    4.  **Power-Cycle Recovery (Firmware):** If device is `bricked` (e.g., bad firmware crashes on boot), user can `power-cycle 3 times` quickly to force entry into `Flasher Mode`.
    5.  **`OTA_PROTECT_NVS` (Firmware):** `main.cpp::performCrashRecoveryCheck()` skips `NVS` erase if `ESP_OTA_IMG_PENDING_VERIFY` to protect bonds/settings during `OTA` rollback.
    6.  **`OTA File Logging` (App):** `OtaService` maintains `ota_debug_log.txt` for offline diagnostics.
    7.  **`Post-Update Verification` (App):** `OtaPostUpdateVerifier` waits for `BLE` reconnect after `OTA`, retrieves `HELLO_ACK` info, and verifies firmware version.
-   **Recovery Path:**
    *   **Auto-Rollback:** If new firmware is buggy, device automatically reboots to previous stable version.
    *   **Flasher Mode:** User enters `Flasher Mode` (manually or via `MSG_ENTER_OTA_MODE`), connects to `Wi-Fi AP`, and re-uploads a known-good firmware via `HTTP`.
-   **Residual Risk/Limits:** Physical damage to flash memory or `Wi-Fi` antenna can still prevent `OTA`. Very rare `ESP32` bootloader corruption would require `JTAG` reflash.
### 6.8 Time and Offline Continuity Failures

#### Timestamp Discontinuity Across Deep Sleep
-   **Domain:** Time & Offline Continuity, Storage & Data Integrity
-   **Failure Mode:** Hits recorded offline across multiple deep sleep cycles have incorrect relative timestamps or appear out of chronological order.
-   **Trigger/Conditions:**
    1.  `millis()` is used for timestamps, but `millis()` resets to `0` on each wake from deep sleep.
    2.  `TIME_SYNC` is never received (device stays offline).
    3.  Device powers off completely (cold boot) between offline sessions.
-   **Impact:** Inaccurate detection analytics, broken timelines in the app, inability to reconstruct correct activity sequences.
-   **Detection:** App detects `DetectionEvent.bootCount` changes, indicating a new device uptime timeline. `DetectionEvent.timestamp == 0` indicates unknown absolute time.
-   **Mitigation:**
    1.  **RTC-Persistent Uptime (Firmware):** `HAL::getSystemUptime()` reads from the `ESP32`'s `RTC` controller, which continues counting during deep sleep. All `DetectionEvent.timestampMs` use this API.
    2.  **Boot Cycle Counter (Firmware):** `DetectionEvent.bootCount` increments only on `cold boots` (`POWER_ON`/`BROWNOUT`) and remains constant across deep sleep wakes. This indicates timeline continuity to the app.
    3.  **`3-Tiered Time Sync` (Firmware & App):**
        *   **Tier 1 (NTP):** Wi-Fi `NTP` sync (`TimeService`) is attempted on cold boot.
        *   **Tier 2 (BLE):** `App` sends `MSG_TIME_SYNC` (App → Device) to sync time.
        *   **Tier 3 (Fallback):** Device internally uses `HAL::getSystemUptime()` as a relative time base.
    4.  **Retroactive Timestamp Backfill (Firmware):** `HistoryService::applyTimeSync()` (triggered by `MSG_TIME_SYNC` or `NTP` sync) iterates through current boot session events. For `DetectionEvent.timestamp == 0`, it calculates `eventEpoch = currentEpoch - (currentUptime - eventUptime)/1000` and updates `DetectionEvent.timestamp`.
    5.  **Forward-Chaining Time (Firmware):** `UserConfig::lastKnownEpoch` is persisted to `NVS` on `TIME_SYNC`. On a subsequent cold boot without network, `ConfigService` restores the system clock to `lastKnownEpoch`, ensuring `time(NULL)` yields a semi-absolute, forward-chained time. Events are marked `TIME_ESTIMATED`.
    6.  **Boot Count Reset Detection (App):** If a new event from a `LOWER` bootCount has a `HIGHER` eventId, the app deduces a device `NVS` reset (firmware fast-forwarded) and updates its stored bootCount.
-   **Recovery Path:** Offline detections are either backfilled with accurate epoch timestamps when the device syncs, or they use a "compressed timeline" that preserves relative order when `lastKnownEpoch` is used.
-   **Residual Risk/Limits:** If device loses power (cold boot) and has no access to Wi-Fi/BLE, `DetectionEvent.timestamp` will remain `0`. These events will be processed by the app with "unknown time" semantics. The "compressed timeline" cannot account for the actual duration the device was powered off.
-   **Related Docs:** `power-state-machine.md`, `storage-integrity.md`.

## 7. Verification and Validation
The reliability claims are substantiated through a multi-faceted approach:
-   **Targeted Unit/Integration Tests:** Specific failure modes (e.g., CRC calculations, NVS corruption scenarios, protocol retransmissions) are tested in isolated environments.
-   **Runtime Asserts & Panics:** Critical invariants and `systemPanic()` calls in firmware trigger immediate halting on unrecoverable errors.
-   **Extensive Logging:** Structured logging across firmware and app layers provides detailed, context-rich diagnostics for all phases, errors, and recovery attempts.
-   **Field-Derived Hardening:** Many mitigations originate from real-world observations, demonstrating a feedback loop that continually hardens the system.
-   **Bench/Platform Validation:** Low-level behaviors (e.g., flash write durations, BLE timing, I2C bus recovery sequences) are validated on the target ESP32-S3 hardware.
-   **App State/Lifecycle Management:** AppState listeners and native module integrations ensure correct behavior across foreground/background transitions and iOS state restoration.

## 8. Known Limits and Non-Goals
This document outlines the deliberately engineered resilience of thesystem. However, certain aspects are intentionally out of scope, represent inherent limitations, or are areas of future work:
-   **Flash Encryption / Secure Boot:** Not currently enabled in the firmware. These features require eFuse programming at manufacturing time and are beyond the scope of current software-only protections.
-   **Generic Bluetooth Device Support:** The `BLE` protocol is highly tailored to thedevice. General-purpose `BLE` communication is not a goal.
-   **ML Model Reliability:** While `ML` services are integrated, the long-term robustness and accuracy of `ML` models in all environments (e.g., dealing with novel detection patterns, model drift) is an ongoing research and development effort. The `FAILURE-MODES.md` currently focuses on the *integrity of the ML data pipeline*, not the *accuracy of the ML predictions*.
-   **Wi-Fi Provisioning (Firmware):** The firmware's Wi-Fi provisioning path is currently disabled. While the app is prepared to send credentials, the device will not process them.
-   **Android Health Connect Integration:** This is explicitly deferred and not currently implemented in the mobile app, remaining a future platform integration.
-   **Proprietary Thresholds:** Specific numerical constants (e.g., `CIRCUIT_BREAKER_THRESHOLD=10`, `MAX_CONSECUTIVE_HANDSHAKE_FAILURES=3`) are tuned for the system. While these are stated, the document does not extensively justify the exact numerical values, as these are operational tunables.
-   **Exhaustive Hardware Faults:** The system mitigates many hardware-related issues (I2C latch-up, sensor failure), but cannot recover from catastrophic hardware failure (e.g., CPU/RAM failure).

## 9. Related Documents
-   [architecture.md](architecture.md): High-level system design and component overview.
-   [ble-protocol.md](ble-protocol.md): Detailed specification of the binary communication protocol.
-   [power-state-machine.md](power-state-machine.md): Comprehensive documentation of device power states and transitions.
-   [ota-recovery.md](ota-recovery.md): In-depth analysis of the OTA update process and recovery mechanisms.
-   [storage-integrity.md](storage-integrity.md): Details on persistent storage (NVS, RTC) and data integrity measures.
---