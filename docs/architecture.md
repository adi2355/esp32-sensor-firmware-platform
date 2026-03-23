# Architecture

## Overview
The system comprises an ESP32-S3 embedded firmware and a companion iOS mobile application, designed for real-time proximity sensing, data logging, and interactive user engagement. This architecture prioritizes reliability, power efficiency, and data integrity across various operational states—connected, offline, and during unattended periods. This document serves as a foundational map, detailing the architectural choices, subsystem boundaries, and operational flows that underpin this robust, connected-device ecosystem.

## Design Goals and Constraints
The architecture of the system is fundamentally shaped by a set of critical hardware and operational constraints, driving specific design goals:

*   **Battery-Powered Operation**: Extended operational life (months) necessitates aggressive power management, including deep sleep modes, low-power sensing, and intelligent control of power-hungry peripherals (BLE, Wi-Fi).
*   **Resource-Constrained MCU**: The ESP32-S3's dual-core architecture, 8MB flash, and 512KB SRAM demand careful memory management, optimized code, and efficient concurrency to prevent resource exhaustion.
*   **Real-Time Sensing**: Consistent 20Hz (50ms interval) sensor data acquisition is critical for accurate event detection, requiring dedicated processing paths that are isolated from blocking I/O.
*   **BLE-First Connectivity (iOS Focus)**: Primary interface for control and data synchronization. Requires robust iOS compatibility (Resolvable Private Addresses, Secure Connections), stable connections, and seamless background operation.
*   **Occasional Wi-Fi Capability**: Utilized sparingly for high-bandwidth tasks like Over-The-Air (OTA) firmware updates and precise NTP time synchronization.
*   **Offline Data Continuity**: Ensure no data loss when the device is disconnected, powered off, or between sleep cycles, enabling a "truly offline" detection logging experience.
*   **Long-Term Unattended Reliability**: The system must operate autonomously for extended periods, incorporating self-healing mechanisms for crashes, recoverable firmware updates, and resilient sensor operation.
*   **User Experience (UX)**: Provide responsive UI feedback, consistent data presentation, and transparent, safe processes (e.g., OTA, factory reset).

## System Architecture at a Glance
The system operates as a unified platform comprising distinct but highly integrated firmware and iOS application layers. Communication between these layers is primarily orchestrated via a custom binary BLE protocol.

```mermaid
graph LR
    subgraph Firmware (ESP32-S3)
        direction LR
        HW[Hardware & Sensors] --- A[HAL]
        A --- B[Services Layer]
        B --- K[FreeRTOS Kernel & Main App]
    end

    subgraph iOS Application
        direction LR
        J[iOS Frameworks & System] --- C[Native Modules]
        C --- D[RN Bridge Modules]
        D --- E[JS Services Layer]
        E --- F[React Native UI]
    end

    subgraph External Systems
        G[HealthKit]
        H[Backend API]
        I[NTP Servers]
    end

    HW -- GPIO, I2C, ADC --> A
    A -- Driver Calls --> B
    B -- Tasking, State --> K
    K -- BLE Host/Controller --> J
    J -- CoreBluetooth --> C
    C -- Native Bridge --> D
    D -- JS Bridge --> E
    E -- UI Calls --> F
    E -- HTTPS --> H
    G -- Health Data --> C
    C -- Keychain --> J
    K -- Wi-Fi --> I
    K -- BLE Peripheral --> J

    style HW fill:#f9f,stroke:#333,stroke-width:2px
    style A fill:#bbf,stroke:#333,stroke-width:2px
    style B fill:#eef,stroke:#333,stroke-width:2px
    style K fill:#ddf,stroke:#333,stroke-width:2px

    style J fill:#fcc,stroke:#333,stroke-width:2px
    style C fill:#f99,stroke:#333,stroke-width:2px
    style D fill:#f66,stroke:#333,stroke-width:2px
    style E fill:#f33,stroke:#333,stroke-width:2px
    style F fill:#f00,stroke:#333,stroke-width:2px

    style G fill:#9cf,stroke:#333,stroke-width:2px
    style H fill:#9c9,stroke:#333,stroke-width:2px
    style I fill:#cc9,stroke:#333,stroke-width:2px
```

The **Firmware Layer** (ESP32-S3) directly interacts with hardware via the **Hardware Abstraction Layer (HAL)**. The **Services Layer** implements most of the device's logic, orchestrated by the **FreeRTOS Kernel and Main Application**. This layer communicates with the **iOS Application** primarily through a custom binary **BLE Peripheral** interface.

The **iOS Application** integrates with **iOS Frameworks and System** components (like CoreBluetooth and HealthKit) via **Native Modules** (Swift/Objective-C). These native capabilities are exposed to the **JavaScript Services Layer** through **React Native Bridge Modules**. The JavaScript services then drive the **React Native UI**.

External integrations include **HealthKit** (for ingesting health data), **Backend API** (for user data, sync, and cloud processing), and **NTP Servers** (for time synchronization).

## Architectural Principles
The architecture is founded on principles that ensure its reliability, efficiency, and extensibility in a demanding embedded environment.

### Event-Driven Multitasking (FreeRTOS)
The firmware leverages FreeRTOS to decouple time-critical operations from blocking I/O. Dedicated tasks, each with specific priorities, handle distinct responsibilities (e.g., `SensorTask` for precise polling, `StorageWorker` for asynchronous flash writes). Communication between tasks primarily uses FreeRTOS queues and task notifications, ensuring non-blocking execution and preventing priority inversion. This model guarantees responsiveness for real-time sensing while allowing long-running operations to proceed in the background without starving critical paths.

### Clear Platform vs. Product Boundaries
The codebase is structured with a distinct separation between platform-specific hardware interactions and higher-level product logic. The Hardware Abstraction Layer (HAL) in the firmware provides a clean, portable interface to the underlying MCU and peripherals, insulating the Services Layer from hardware specifics. Similarly, in the iOS app, native Swift modules encapsulate direct iOS framework interactions, exposing a simplified interface to the React Native JavaScript layer. This promotes maintainability, testability, and potential portability.

### Fail-Safe and Recoverable Design
Reliability is prioritized through defensive programming and active recovery mechanisms. The system is designed to detect and gracefully recover from various failure modes, including unexpected reboots, data corruption, and connection losses, rather than simply crashing or becoming unusable. This includes crash counters, robust OTA rollback, I2C bus recovery, and app-side safeguards for inconsistent states.

### Tiered Persistence for Continuity and Durability
Data is managed across multiple persistence layers, each optimized for different durability, latency, and power requirements.
*   **Firmware**:
    *   **RTC Memory**: For fast, low-power storage that survives deep sleep (`HistoryService` ring buffer, boot flags).
    *   **NVS Flash**: For persistent storage across power loss (`UserConfig`, `HistoryService` backup, `BLEManager` bonds).
    *   **SPIFFS Flash**: For larger, file-based data (`DataLoggerService` ML samples).
*   **App (iOS)**:
    *   **Keychain**: For sensitive user data and unique device identifiers that survive app reinstallation (`SecureStorageService`).
    *   **SQLite**: For structured application data (`HealthIngestSQLite`, Drizzle ORM tables).
    *   **UserDefaults**: For lightweight, non-critical settings (factory reset guards).
    *   **AsyncStorage**: For general-purpose app data caching (legacy, being migrated).

### Power-Aware Operation as a First-Class Concern
Energy consumption is a primary architectural driver. Components are designed to enter low-power states whenever possible, from aggressive deep sleep cycling in the firmware to intelligent Wi-Fi management and optimized BLE connection parameters. The system actively monitors activity levels and power state to dynamically adapt its behavior and minimize battery drain during idle or background operation.

### Bounded Concurrency and Resource Use
To prevent resource exhaustion and ensure predictable real-time performance, the system employs bounded resources. This includes fixed-size message queues, explicit maximums for concurrent operations, watchdog timers, and mutexes with timeouts. Critical operations are designed to be non-blocking where possible, or their execution is carefully managed to avoid indefinite resource contention or system stalls.

### Idempotency and Deduplication
Operations are designed to be safely repeatable without unintended side effects. This is crucial for robust synchronization and recovery. Examples include transactional database writes, event ID-based deduplication for processing incoming data, and OTA chunking that can tolerate retransmission.

## Layering and Ownership
The codebase is structured into distinct layers and subsystems, each with clear responsibilities, promoting modularity and maintainability.

| Subsystem                      | Primary Responsibility (Firmware)                                      | Primary Responsibility (iOS App)                                              | Key Components (Firmware)                                            | Key Components (iOS App)                                                                                                     | Key Interfaces / Protocols                  |
| :----------------------------- | :--------------------------------------------------------------------- | :---------------------------------------------------------------------------- | :------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------ |
| **HAL (Hardware Abstraction)** | Direct hardware interaction (GPIO, I2C, ADC), ISR-safe access, LED control, basic flash management. | -                                                                             | `HAL_Base.h/cpp`, `LED_HAL.h/cpp`, `Sensor_HAL.h/cpp`, `StorageHAL.h/cpp` | -                                                                                                                            | GPIO, I2C Bus, ADC, FastLED                 |
| **BLE Connectivity**           | NimBLE stack management, advertising, bonding, connection parameters, security. | CoreBluetooth central management, background BLE, state restoration, device discovery. | `BLEManager.h/cpp`, `nimconfig.h`                                    | `DeviceBLECore.swift` (Native), `BluetoothHandler.ts` (JS), `DeviceBLEModule.m/swift` (Bridge)                               | CoreBluetooth API, NimBLE API               |
| **Binary Protocol**            | Frame building/parsing, ACK/NACK, retry logic, message dispatch.       | Frame building/parsing, ACK/NACK, retry logic, message dispatch.              | `ProtocolHandler.h/cpp`, `ProtocolDefs.h`                            | `EdgeDeviceProtocolService.ts`, `BinaryProtocol.ts`                                                                            | Custom Binary Protocol (SOF, CRC)           |
| **Sensing & Event Detection**  | 20Hz sensor polling, event detection state machine, threshold management, environmental adaptation. | -                                                                             | `Sensor_HAL.h/cpp`, `main.cpp` (`handleDetection`, `sensorTask`)  | -                                                                                                                            | Proximity Sensor (I2C), GPIO Interrupt              |
| **Persistence (Events)**       | RTC ring buffer for detections, NVS backup, event ID synchronization.        | `lastEventId` storage (AsyncStorage), detection processing pipeline, local DB storage. | `HistoryService.h/cpp` (`StorageWorker` task)                        | `EventSyncService.ts`, `DetectionsRepository.ts` (JS)                                                                          | RTC, NVS, AsyncStorage, SQLite              |
| **Power Management**           | Device power state machine, deep sleep/light sleep control, battery monitoring, wake sources. | -                                                                             | `PowerManager.h/cpp`                                                 | -                                                                                                                            | ESP-IDF `esp_sleep`, `esp_pm`, ADC          |
| **OTA & Recovery**             | Dual-bank OTA orchestration, firmware self-test, rollback, Flasher Mode (Wi-Fi AP + HTTP server). | OTA download, HTTP upload client, post-update verification, log access.       | `OTAManager.h/cpp`, `FlasherService.h/cpp`, `BootRecoveryTracker.h/cpp` | `OtaService.ts`, `OtaPostUpdateVerifier.ts`, `OtaFileLogger` (JS)                                                            | Wi-Fi (HTTP), BLE (trigger)                 |
| **Time Synchronization**       | NTP client, Wi-Fi connectivity for NTP, system clock management, "One-Shot" policy. | Timezone management, sync anchor, timestamp conversion.                       | `TimeService.h/cpp`, `WifiService.h/cpp`                             | `EventSyncService.ts`                                                                                                        | NTP, BLE (MSG_TIME_SYNC)                    |
| **Config Management**          | User settings persistence (NVS), sensitivity scaling, LED brightness.  | Local cache (AsyncStorage), UI interaction, sync with device.                 | `ConfigService.h/cpp` (`ConfigWorker` task)                          | `DeviceSettingsService.ts`                                                                                                   | BLE (MSG_SET_CONFIG, MSG_GET_CONFIG)        |
| **ML/Gesture Recognition**     | Raw sample logging (SPIFFS), on-device feature extraction, inference, personalization, adaptation. | -                                                                             | `DataLoggerService.h/cpp`, `InferenceService.h/cpp`, `PersonalizationService.h/cpp` | -                                                                                                                            | SPIFFS, NVS                                 |
| **Health Data Ingestion**      | -                                                                      | HealthKit queries (HOT/COLD/CHANGE lanes), data normalization, SQLite writes, background delivery. | -                                                                    | `HealthIngestCore.swift`, `HealthKitQueries.swift`, `HealthNormalization.swift`, `HealthIngestSQLite.swift`, `HealthKitObserver.swift`, `NativeHealthIngestModule.m/swift` | HealthKit API, SQLite                       |
| **App Lifecycle/Reset**        | -                                                                      | Reinstall detection, Keychain wipe, SQLite/AsyncStorage reset, factory reset guards. | -                                                                    | `AppDelegate.mm`, `ReinstallDetector.swift`, `FactoryResetGuard.swift`, `KeychainWipeModule.m/swift`, `FactoryResetModule.m/swift` | Keychain, UserDefaults, FileManager         |

## Core Subsystems
The system is composed of several tightly integrated subsystems, each with a defined role.

### Hardware Abstraction Layer (Firmware)
The HAL (Hardware Abstraction Layer) insulates the application logic from direct hardware complexities. `LED_HAL` manages the WS2812B NeoPixel LED with non-blocking patterns and includes deep sleep preparation. `Sensor_HAL` interfaces with the Proximity Sensor proximity sensor, providing ISR-safe data acquisition, dynamic thresholding, calibration, and I2C bus recovery. `StorageHAL` validates flash partitions (for OTA) and manages NVS initialization with self-recovery. This layer ensures consistent and reliable hardware interaction.

### BLE Connectivity and Protocol
This subsystem manages all Bluetooth Low Energy communication between the device and the app. The firmware's `BLEManager` employs the NimBLE stack for iOS compatibility (RPA, Secure Connections, NVS bonding persistence) and optimized connection parameters. The `ProtocolHandler` implements a custom binary protocol, ensuring reliable, ACK/NACK-based message delivery with retries and a "Panic Governor" to prevent BLE flooding. On the app side, the native `DeviceBLECore` handles iOS-specific CoreBluetooth interactions, including background restoration and robust error classification, while the `BluetoothHandler` orchestrates the JS-level state machine, reconnection logic (circuit breaker, dormant mode), and delegates data transfer to the native module.

### Sensing and Event Detection (Firmware)
The core function of the device is to detect interaction events. The `SensorTask`, running at a high priority (20Hz), continuously polls the `Sensor_HAL` for proximity data. `main.cpp::handleDetection` applies multi-level thresholds (`DETECTION_Z_SCORE`, `CRITICAL_Z_SCORE`), hysteresis, and environmental adaptation to accurately identify detections while filtering noise (e.g., from glass surfaces). A "stuck-detection watchdog" prevents indefinite active states, ensuring reliability.

### Persistence and Data Integrity
Data integrity is paramount. In the firmware, `HistoryService` manages a low-power RTC-backed ring buffer for `DetectionEvent`s, protected by `CRC32` and backed up to NVS by the asynchronous `StorageWorker` to survive power loss. `ConfigService` similarly persists `UserConfig` to NVS with lazy-write debouncing. On the app side, `HealthIngestSQLite` uses transactional writes with `WAL` mode and `CAS` to safely store health data, while `SecureStorageService` leverages iOS Keychain with appropriate accessibility levels (e.g., `AFTER_FIRST_UNLOCK`) for sensitive data, ensuring background access.

### Power Management (Firmware)
The `PowerManager` orchestrates the device's lifecycle through defined power states (e.g., `ACTIVE`, `SLEEP_PREP`, `DEEP_SLEEP`). It monitors battery levels, detects charging status, and analyzes wake reasons. Aggressive idle timeouts, a "sleep guard" mechanism (`canSafelyEnterSleep()`), and a multi-source wakeup strategy (sensor, button, timer failsafe) ensure optimal power efficiency while guaranteeing the device's recoverability from deep sleep.

### OTA and Recovery
The Over-The-Air (OTA) update system is designed for safety and recoverability. The firmware supports dual-bank OTA, managed by `OTAManager`. When triggered, the device enters a dedicated "Flasher Mode" (via `FlasherService`), creating a Wi-Fi SoftAP for high-speed, HTTP-based firmware uploads. This process includes 2-stage battery checks, an explicit erase phase, and a post-update `performSelfTest()` which triggers an automatic rollback if the new firmware fails, preventing device bricking (`BootRecoveryTracker` enhances recovery from power cycles). The app's `OtaService` manages the client-side chunked upload and verification.

### Time Synchronization
Accurate timestamps are critical for event analytics. The system employs a three-tiered time synchronization strategy. In the firmware, `TimeService` leverages Wi-Fi and NTP servers for highly accurate time. A "One-Shot" Wi-Fi policy (`WifiService`) ensures the radio is only active long enough to sync time, minimizing battery drain. The app's `EventSyncService` manages a `timeSyncAnchor` and `lastEventId` to convert device uptime to absolute timestamps and retroactively backfill timestamps for offline events when sync is established.

### On-Device Intelligence (Firmware)
For advanced functionality, the firmware incorporates a TinyML pipeline. The `DataLoggerService` asynchronously logs raw sensor data windows to SPIFFS for training. The `InferenceService` performs on-device gesture classification using a hybrid model (global + local prototypes), while the `PersonalizationService` manages per-device adaptation data in NVS, dynamically adjusting the sensing model for different environments (e.g., various glass surfaces). This subsystem is carefully isolated to avoid impacting critical real-time operations.

### Health Data Ingestion (App)
The iOS app's native `HealthIngestCore` orchestrates efficient HealthKit data ingestion. It uses dedicated `OperationQueue`s for HOT (recent, foreground), COLD (historical backfill, background), and CHANGE (deletions/edits, background delivery) lanes. HealthKit queries (`HealthKitQueries`) are robustly managed with timeouts. `HealthNormalization` transforms raw samples into a SQLite-ready format, handling metric-specific logic and derived sleep intervals. `HealthIngestSQLite` ensures safe, transactional database writes. The `HealthKitObserver` enables background delivery, waking the app for HealthKit changes and triggering the `CHANGE` lane.

### App Lifecycle & Factory Reset
The `AppDelegate.mm` orchestrates critical, early app lifecycle tasks. The `ReinstallDetector` uses Keychain markers and a sandbox sentinel file to reliably detect app reinstallation. Upon detection, `FactoryResetGuard` (with retry limits) triggers `KeychainWipeModule` (Security.framework) and `FactoryResetModule` (SQLite, AsyncStorage) to perform a comprehensive factory reset, ensuring a clean state and preventing stale data from causing crashes after a reinstall.

## Runtime Model

### Firmware (FreeRTOS Multitasking)
The ESP32-S3 firmware operates under a FreeRTOS multitasking model, with critical responsibilities distributed across multiple tasks pinned to specific CPU cores. This separation is crucial for managing timing guarantees and isolating blocking operations.

```mermaid
graph TD
    subgraph CPU Core 0 (BLE)
        BLE_CONTROLLER[NimBLE Host/Controller]
    end

    subgraph CPU Core 1 (Application)
        A[SensorTask (Prio 10)] -- 20Hz Sampling --> B[DataLoggerService (Input)]
        A -- Real-time Hit Detect --> C[Main Loop (ProtocolTask) (Prio 5)]
        B -- Queue Window --> D[LoggerTask (ML) (Prio 1)]
        B -- Notify Window Ready --> E[InferenceTask (ML) (Prio 3)]
        C -- BLE State/Control --> BLE_CONTROLLER
        C -- NVS Flush (Config) --> F[ConfigWorker (Prio 1)]
        C -- NVS Flush (History) --> G[StorageWorker (Prio 2)]
        C -- Health Check --> H[PowerManager]
        C -- OTA Control --> I[OTAManager]
        C -- Wi-Fi/Time Control --> J[WifiService / TimeService]

        D -- Write to SPIFFS (Async) --> SPIFFS_FLASH[(SPIFFS Flash)]
        E -- Read Window / Write Result --> ML_NVS[(ML NVS)]
        F -- Write to NVS (Async) --> NVS_FLASH[(NVS Flash)]
        G -- Write to NVS (Async) --> NVS_FLASH
        H -- Deep Sleep --> RTC_PERIPH_HW[(RTC Periph. & HW)]
        H -- Wakeups --> A

        style BLE_CONTROLLER fill:#add8e6,stroke:#333,stroke-width:2px
        style A fill:#ffcc99,stroke:#333,stroke-width:2px
        style B fill:#ffd700,stroke:#333,stroke-width:1px
        style C fill:#90ee90,stroke:#333,stroke-width:2px
        style D fill:#dda0dd,stroke:#333,stroke-width:1px
        style E fill:#d3d3d3,stroke:#333,stroke-width:1px
        style F fill:#87ceeb,stroke:#333,stroke-width:1px
        style G fill:#ffb6c1,stroke:#333,stroke-width:1px
        style H fill:#f08080,stroke:#333,stroke-width:2px
        style I fill:#ffa07a,stroke:#333,stroke-width:1px
        style J fill:#aaffdd,stroke:#333,stroke-width:1px
        style SPIFFS_FLASH fill:#d8bfd8,stroke:#333,stroke-width:1px
        style NVS_FLASH fill:#d8bfd8,stroke:#333,stroke-width:1px
        style RTC_PERIPH_HW fill:#b0c4de,stroke:#333,stroke-width:1px
    end
```

*   **Core 0 (BLE)**: Dedicated to the NimBLE host and controller stack, ensuring high-priority processing of BLE radio interrupts. Application tasks generally do not run on this core.
*   **Core 1 (Application)**: All application-level tasks are pinned to this core.
    *   `SensorTask` (Prio 10): The highest-priority application task, ensuring precise 20Hz sensor polling via `vTaskDelayUntil()`. It directly feeds data to `DataLoggerService` and signals event detection.
    *   `ProtocolTask` (Prio 5): This is the main application loop, handling BLE protocol (sending/receiving, retries), `PowerManager` updates, `OTAManager` state, `WifiService`/`TimeService` updates, and `LED_HAL` updates. It runs when higher-priority tasks are idle.
    *   `InferenceTask` (Prio 3): Performs on-device ML feature extraction and model inference on incoming sensor windows. It is notified by `DataLoggerService` when a new window is ready.
    *   `StorageWorker` (Prio 2): Handles blocking NVS write operations from `HistoryService` asynchronously via a queue.
    *   `ConfigWorker` (Prio 1): Handles blocking NVS write operations from `ConfigService` asynchronously via a queue.
    *   `LoggerTask` (Prio 1): Handles blocking SPIFFS write operations from `DataLoggerService` asynchronously via a queue.
*   **Inter-Task Communication**: FreeRTOS `xQueue`s (`_storageQueue`, `_loggerQueue`) are used to decouple tasks, ensuring slow I/O operations do not block real-time tasks. `xSemaphoreCreateMutex` protects shared data (`_halMux`, `_stateMutex`, `_inferenceMutex`). `std::atomic` variables ensure thread-safe access to shared flags.

### iOS App (Native Concurrency)
The iOS application manages concurrency using Apple's frameworks, primarily `CBCentralManager` on the main queue and dedicated `OperationQueue`s for background health data ingestion.

*   **CoreBluetooth**: The `CBCentralManager` (`DeviceBLECore`) operates on the main thread (as is standard practice for CoreBluetooth delegates), ensuring responsiveness for UI-related BLE events.
*   **HealthKit Ingestion**: `HealthIngestCore` utilizes three distinct `OperationQueue`s, each configured with an appropriate `QualityOfService` (QoS) for lane isolation and priority management:
    *   `hotQueue` (`.userInitiated`): For foreground, UX-critical, low-latency queries.
    *   `coldQueue` (`.utility`): For background, historical backfill, lower priority.
    *   `changeQueue` (`.default`): For background HealthKit change processing.
*   **SQLite Access**: `HealthIngestSQLite` uses a dedicated `DispatchQueue` (`com.sensordevice.healthingest.sqlite`) for serializing all SQLite read/write operations, preventing database corruption from concurrent access.

## Critical System Flows
These sequences highlight the most important operational paths through the ecosystem, including how firmware and app interact and how resilience is built into each.

### 1. Firmware Boot and Initialization
The boot process is designed for rapid startup and robust recovery.
1.  **Power-On / Reset**: ESP32-S3 powers on.
2.  **Crash Recovery Check**: `main.cpp::performCrashRecoveryCheck()` (`g_crash_counter`) runs *before* NVS init. If multiple WDT resets are detected, it preemptively erases NVS to break boot loops (e.g., NVS corruption). If a new OTA is pending, it defers erase to allow native rollback.
3.  **Flasher Mode Check**: `main.cpp` checks `g_boot_mode` (RTC) and `g_stability_counter` (power-cycle pattern). If either signals Flasher Mode, `FlasherService::run()` is called (never returns).
4.  **HAL Initialization**: `LED_HAL`, `Sensor_HAL` (with I2C bus recovery), `StorageHAL` (partition validation, NVS init/recovery) are initialized. Sensor ID is verified.
5.  **OTA Firmware Validation**: `OTAManager::validateBootedFirmware()` runs. If the current firmware is `PENDING_VERIFY` (after an OTA), it executes `performSelfTest()` (e.g., sensor check, heap, NVS, LED). If tests fail, `esp_ota_mark_app_invalid_rollback_and_reboot()` is triggered. If tests pass, `esp_ota_mark_app_valid_cancel_rollback()`.
6.  **Services Initialization**: `HistoryService` (RTC state validation, NVS restore), `ConfigService` (NVS load, system time restore from `lastKnownEpoch`), `BLEManager` (NimBLE setup, security, advertising), `WifiService` (passive init), `TimeService` (policy evaluation for Wi-Fi/NTP sync), ML services (`DataLoggerService`, `InferenceService`, `PersonalizationService`) init.
7.  **Task Launch**: `SensorTask` (20Hz polling) is launched on Core 1.
8.  **ProtocolTask Start**: `main.cpp` `loop()` becomes the `ProtocolTask`, starting the main event loop.
*   **Reliability**: Multi-stage failure detection (crash counter, OTA self-test), graceful rollback, I2C bus recovery, RTC/NVS data integrity checks.
*   **Related Docs**: `ota-recovery.md`, `failure-modes.md`

### 2. Sensing, Event Detection, and Persistence (Firmware)
This flow ensures accurate, real-time event detection and reliable storage, even when offline.
1.  **Sensor Polling**: `SensorTask` (Core 1, Prio 10) executes every 50ms (20Hz) via `vTaskDelayUntil()`, ensuring precise timing. It calls `Sensor_HAL::readProximity()`.
2.  **Event Detection**: `main.cpp::handleDetection()` processes the `proximityValue`.
    *   **Multi-Level Thresholds**: `DETECTION_THRESHOLD` (3-sigma) starts detection and turns LED green. `CRITICAL_THRESHOLD` (5-sigma) must be met at some point during the hit for confirmation.
    *   **Validation**: A detection is logged only if `durationMs >= MIN_DETECTION_DURATION_MS` (e.g., 1s) AND `isConfirmedDetection == true`.
    *   **Environmental Adaptation**: "Stuck-Hit Watchdog" and "Jitter Prevention" in `sensorTask` and `handleDetection` can trigger `Sensor_HAL::applyEnvironmentBackoff` or recalibration if signal is unstable (e.g., on glass).
3.  **Event Logging**: If validated, `HistoryService::logDetection(durationMs)` is called.
    *   The event is immediately added to an RTC-backed ring buffer in RAM (`_stateMutex` protected).
    *   `DetectionEvent.timestampMs` uses `HAL::getSystemUptime()` (RTC-persistent clock) for timeline continuity across deep sleep.
    *   `DetectionEvent.timestamp` uses system epoch (backfilled if `TIME_SYNC` occurred, else 0).
4.  **Asynchronous NVS Backup**: `HistoryService` detects `NVS_FLUSH_THRESHOLD` or critical buffer fullness.
    *   If BLE connected and not critical: Flush is *deferred* to avoid blocking BLE (`EC-BLE-FLASH-001`).
    *   If BLE disconnected or critical: `HistoryService::queueStorageCommand(SAVE_HIT)` sends a message to `StorageWorker`.
5.  **Storage Worker**: `StorageWorker` (Core 1, Prio 2) dequeues the message, performs `HistoryService::flushToNvs()`.
    *   `flushToNvs()` copies data to a local buffer (outside mutex), then writes to NVS (`Preferences`) (WDT-protected). This ensures BLE (Core 0) is not starved during flash writes.
*   **Reliability**: Real-time task isolation, multi-stage hit validation, persistent RTC timestamps, NVS backup, asynchronous I/O.
*   **Related Docs**: `power-state-machine.md`, `failure-modes.md`

### 3. Offline Data Continuity and App Synchronization
Ensures all recorded detections are eventually synchronized to the app with correct timestamps, even if the device was offline for extended periods.
1.  **Offline Logging**: Device `HistoryService` records detections in RTC/NVS using `HAL::getSystemUptime()` for `timestampMs`. If no time sync, `timestamp` is 0.
2.  **Device Connects**: App `BluetoothHandler` establishes BLE connection.
3.  **Handshake**: App `EventSyncService::onDeviceConnected()` sends `MSG_HELLO` (including App's `lastEventId`).
4.  **Event ID Synchronization**: Firmware `ProtocolHandler::handleHello()` receives `MSG_HELLO`. If App's `lastEventId` is *higher* than the device's `globalEventId` (e.g., after firmware reflash), `HistoryService::fastForwardEventId()` updates the device's `globalEventId` and persists it to NVS (`EC-SYNC-006`).
5.  **Time Sync Anchor**: Firmware `ProtocolHandler::handleHelloAck()` sends `HelloAckPayload` (including device's `currentMillis`, `lastEventId`). App `EventSyncService::handleHelloAck()` receives this, establishes a `timeSyncAnchor` (phone epoch vs. device uptime).
6.  **Timestamp Backfill**: App sends `MSG_TIME_SYNC` (phone's current epoch, timezone). Firmware `ProtocolHandler::handleTimeSync()` updates the system clock to this real time and triggers `HistoryService::applyTimeSync()`.
    *   `applyTimeSync()` iterates through all events in the *current boot session* in RTC. For any `DetectionEvent` with `timestamp == 0`, it calculates `eventEpoch = currentEpoch - (currentUptime - eventUptime)/1000` and updates the event in RTC, queuing an NVS flush.
7.  **Event Request**: If `HelloAckPayload.lastEventId` from device is *higher* than App's `lastEventId`, App `EventSyncService` sends `MSG_SYNC_REQUEST` (since App's `lastEventId`).
8.  **Event Delivery**: Firmware `ProtocolHandler::handleSyncRequest()` retrieves unsynced `DetectionEvent`s from `HistoryService` and sends them individually as `MSG_DETECTION_EVENT`s. The "Panic Governor" is *bypassed* for these sync events (`EC-SYNC-GOVERNOR-001`).
9.  **Event Processing**: App `EdgeDeviceProtocolService::handleDetectionEvent()` receives `MSG_DETECTION_EVENT`.
    *   Sends ACK immediately (awaited).
    *   `EventSyncService::onProcessedDetectionEvent` processes: updates `lastEventId` in AsyncStorage (primary/backup), converts `DetectionEvent.timestampMs` to `Date` using `timeSyncAnchor` (or uses `DetectionEvent.timestamp` if available), stores in app DB.
*   **Reliability**: RTC-backed persistent uptime, NVS backup, `globalEventId` sync, `timeSyncAnchor` for accurate timestamp reconstruction, idempotent event processing, ACK/NACK retries.
*   **Related Docs**: `ble-protocol.md`, `failure-modes.md`

### 4. OTA Update, Self-Test, and Rollback (App to Firmware)
A robust, Wi-Fi-based OTA system ensures safe firmware updates, even on battery power or if the new firmware is faulty.
1.  **Trigger Flasher Mode (BLE)**: App `EdgeDeviceProtocolService::sendEnterOtaMode()` (via BLE) commands the device to enter Flasher Mode.
2.  **Device Reboot (Flasher Mode)**: Firmware `ProtocolHandler::handleEnterOtaMode()` acknowledges, sets `g_boot_mode` (RTC), and reboots. `main.cpp` detects `FlasherMode` and runs `FlasherService::run()` (bypassing main app).
3.  **Flasher Service Init (Firmware)**: `FlasherService` initializes minimal hardware, performs 2-stage battery checks (`MIN_VOLTAGE_IDLE_MV`, `MIN_VOLTAGE_LOAD_MV`, `MAX_VOLTAGE_SAG_MV`), starts Wi-Fi SoftAP (configured SoftAP SSID), and launches HTTP server.
4.  **Start OTA Session (HTTP)**: App `OtaService::startSession()` POSTs to `http://192.168.4.1/start` (with `X-Session-ID`, `X-Total-Size`). `FlasherService::handleStart()` acknowledges and sets a *deferred* `START_SESSION` action for the main loop. This decouples erase from HTTP.
5.  **Flash Erase (Firmware)**: `FlasherService`'s main loop detects `START_SESSION` action, transitions to `ERASING` phase, and calls `Update.begin()` (blocking flash erase).
6.  **Wait for Ready (HTTP Polling)**: App `OtaService::waitForPhase(RECEIVING)` polls `http://192.168.4.1/status` until `FlasherPhase::RECEIVING` (erase complete).
7.  **Chunked Upload (HTTP POST)**: App `OtaService::uploadFirmware()` reads `firmware.bin`, chunks it into 4KB segments (`CHUNK_SIZE`), and POSTs each chunk to `http://192.168.4.1/update_chunk` as `multipart/form-data` (`EC-MULTIPART-BIN-001`).
    *   `FlasherService::handleData()` receives chunks via `upload.buf` (binary-safe), writes to flash (`Update.write()`, WDT-protected), and updates status.
    *   App `OtaService` waits for `CHUNK_ACK_TIMEOUT_MS` after each chunk (`waitForChunkCommit`). `INTER_CHUNK_DELAY_MS` yields between chunks to prevent TCP congestion.
8.  **Commit and Reboot**: Upon last chunk, App `OtaService::waitForCompletion()` polls status until `FlasherPhase::COMPLETE`. Device then reboots.
9.  **Firmware Self-Test**: The newly booted firmware executes `OTAManager::validateBootedFirmware()` which runs `performSelfTest()` (checks sensor, heap, NVS, LED).
    *   If `performSelfTest()` passes: Firmware marks itself `VALID`, cancels rollback.
    *   If `performSelfTest()` fails: Firmware marks itself `INVALID` and `esp_ota_mark_app_invalid_rollback_and_reboot()` triggers a reboot to the *previous* firmware.
10. **Post-Update Verification (BLE)**: App `OtaPostUpdateVerifier` reconnects to the device over BLE, requests `HelloAckPayload`, and verifies the `firmwareVersion` matches the expected value (`EC-OTA-003`).
*   **Reliability**: Dual-bank rollback, `FlasherService` isolation/power gating, explicit erase, `multipart/form-data` encoding, chunked uploads with retries/timeouts, self-test verification, `BootRecoveryTracker` for bricked devices.
*   **Related Docs**: `ota-recovery.md`, `failure-modes.md`

### 5. Sleep and Wake Lifecycle (Firmware)
Efficient power management is central to extending battery life, balancing responsiveness with deep sleep.
1.  **Idle Detection**: `PowerManager::checkIdleTimeout()` (from `ProtocolTask`) continuously monitors activity.
    *   If `ACTIVE` (connected): Checks `getTimeSinceActivity()` against `CONNECTED_IDLE_TIMEOUT_MS` (60s). `onActivityDetected()` resets this timer.
    *   If `WAKE_MODE` (disconnected): Checks `getTimeInState()` against `DISCONNECTED_SLEEP_TIMEOUT_MS` (60s) or `SENSOR_WAKE_QUICK_SLEEP_TIMEOUT_MS` (30s) if woken by sensor (`EC-PHANTOM-WAKE-001`). Includes `_lastSleepCancellationTime` debounce (`EC-PWR-SLEEP-LOOP-001`).
    *   `PowerManager::shouldForceSleep()` handles `_sleepCancelCount` or prolonged disconnection (`FORCE_SLEEP_TIMEOUT_MS`) to ensure eventual sleep.
2.  **Sleep Preparation**: `PowerManager::requestSleep()` transitions to `SLEEP_PREP` state (LED blinks red, `onBeforeSleep` callback).
3.  **Sleep Guards**: `PowerManager::canSafelyEnterSleep()` checks for pending protocol messages, active OTA, `HistoryService` flush. If `false`, sleep is deferred (`_sleepDeferred`).
    *   `Sensor_HAL::isProximityBelowThreshold()` checks if proximity is *already* elevated (e.g., hand on sensor) *before* `esp_deep_sleep_start()` (`EC-ELEVATED-PROX-SLEEP-001`). If so, sleep is aborted.
    *   A "last second" GPIO check prevents rapid taps from disrupting sleep (`EC-RAPID-TAP-001`).
4.  **Peripheral Shutdown**: `PowerManager::enterDeepSleep()` performs a graceful shutdown:
    *   Notifies App with `MSG_SLEEP` (`EC-IDLE-SLEEP-001`).
    *   Flushes `ConfigService` and `HistoryService` to NVS.
    *   Stops `StorageWorker` and disconnects BLE.
    *   `LED_HAL::prepareForSleep()` sends 'black' to NeoPixel and holds GPIO low.
5.  **Sensor Configuration**: `Sensor_HAL::configureForDeepSleep()` sets sensor to low-power, interrupt-driven mode, *with verification*.
6.  **Wake Sources Arming**: `PowerManager::configureWakeSources()` sets `esp_sleep_enable_ext1_wakeup` (for sensor/button). `esp_sleep_enable_timer_wakeup` is *always* configured as a failsafe (2 hours if sensor OK, 30s if sensor config failed - `EC-PERMANENT-SLEEP-001`).
7.  **Deep Sleep**: `esp_deep_sleep_start()`.
8.  **Wake-Up**: Device wakes on configured source. `PowerManager::detectWakeReason()` identifies the cause.
*   **Reliability**: Multi-level idle detection, graceful app notification, comprehensive sleep guards, persistent timers, failsafe wakeup, robust sensor config.
*   **Related Docs**: `power-state-machine.md`, `failure-modes.md`

## Reliability and Failure Containment
The system is designed with a strong emphasis on reliability and graceful recovery, employing various architectural patterns to contain failures and ensure data integrity.

*   **Firmware Crash Recovery**:
    *   **NVS Corruption Boot Loop**: The `g_crash_counter` (RTC_NOINIT_ATTR) in `main.cpp` detects 3 consecutive Watchdog Timer (WDT) resets. Upon detection, `performCrashRecoveryCheck()` preemptively erases the NVS partition, breaking the boot loop and allowing the device to self-heal, albeit by losing bonds and settings (`EC-RECOVERY-001`).
    *   **OTA Rollback**: The `OTAManager` implements dual-bank OTA. New firmware is marked `PENDING_VERIFY`. If `performSelfTest()` (which checks sensor, heap, NVS, LED) fails on boot, the system automatically rolls back to the previous, known-good firmware version (`EC-OTA-001`).
    *   **Power-Cycle Flasher Mode**: `BootRecoveryTracker` (NVS-backed) in conjunction with `g_stability_counter` (RTC-backed) detects a pattern of 3 quick power cycles. This forces the device into `FlasherMode` for recovery via Wi-Fi OTA, providing a failsafe for completely bricked firmware.
*   **Data Integrity**:
    *   **RTC Memory CRC**: `HistoryService` stores critical event data in RTC memory with a `CRC32` checksum. On boot, `validateState()` verifies integrity (`EC-PWR-002`); corruption triggers NVS backup restore or a full reset.
    *   **NVS Data CRC**: `ConfigService` stores `UserConfig` (sensitivity, etc.) with `CRC32`. Corruption leads to defaults.
    *   **BLE Protocol CRC**: Every binary BLE frame includes a `CRC16-CCITT-FALSE` checksum. `ProtocolHandler` discards frames with invalid CRCs and sends `NACK`.
    *   **SQLite Transactionality (App)**: `HealthIngestSQLite` uses `BEGIN IMMEDIATE` transactions, `WAL` mode, and `sqlite3_changes()` (CAS) to ensure atomic updates to `health_samples` and `health_ingest_cursors`, preventing partial writes and data corruption.
*   **Concurrency Safety**:
    *   **FreeRTOS Primitives**: `xSemaphoreCreateMutex` (firmware) protects shared data from race conditions across tasks. `std::atomic` ensures atomic reads/writes for simple flags. `portMUX_TYPE` provides ISR-safe critical sections.
    *   **Thread-Safe Buffering (App)**: `DeviceBLEModule` uses an `EventBuffer` with `NSLock` to store events when JavaScript listeners are inactive, guaranteeing eventual delivery and preventing data loss (`EC-BUFFER-OVERFLOW-001`).
*   **BLE Connection Stability**:
    *   **iOS Compatibility**: `BLEManager` (firmware) configures NimBLE for `NVS_PERSIST`, `SM_SC` (Secure Connections), and `1M PHY` for robust iOS pairing and RPA handling.
    *   **Delayed Parameter Updates**: `BLEManager` defers connection parameter updates (e.g., to 2 seconds after authentication) to avoid colliding with iOS's security handshake process (`EC-BLE-PARAM-TIMING-001`).
    *   **Handshake Whitelist**: `ProtocolHandler` allows essential handshake messages (HELLO, ACK, HEARTBEAT) to bypass encryption checks during early connection stages, preventing "polite standoff" disconnects (`EC-SECURITY-HANDSHAKE-RACE-001`).
    *   **Panic Governor**: `ProtocolHandler` rate-limits `MSG_DETECTION_EVENT`s to prevent BLE channel saturation from spurious sensor triggers, preserving bandwidth for critical control messages (`EC-SELF-DOS-001`).
    *   **Connection Health Monitoring (App)**: `BluetoothHandler` (app) periodically sends `MSG_HEARTBEAT` to verify connection health. It includes specific logic to detect and ignore security-related errors during initial connection, preventing "heartbeat suicide" loops (`EC-HEARTBEAT-SUICIDE-001`).
    *   **Connection Instability Detection (App)**: `BluetoothHandler` tracks `consecutiveHandshakeFailures`, `shortConnectionBondFailureCount`, and `connectionInstabilityCount` to proactively detect bonding key mismatches or unstable connections, triggering `forgetDevice()` if thresholds are exceeded (`EC-GHOST-BOND-001`).
    *   **Dormant Reconnection (App)**: After multiple aggressive reconnect attempts fail, `BluetoothHandler` enters a dormant mode, scheduling periodic background retries using native `autoConnect` to preserve battery while eventually re-establishing connection (`EC-RECONNECT-PERSIST-001`).
    *   **Single BLE Stack (iOS)**: The iOS app uses a unified BLE stack where the native `DeviceBLECore` owns `CBCentralManager`, eliminating conflicts from multiple `BleManager` instances (`EC-SINGLE-STACK-001`).
*   **Sensor and I2C Robustness**:
    *   **I2C Bus Recovery**: `Sensor_HAL::recoverI2CBus()` performs bit-banging on SDA/SCL to clear bus latch-up conditions (e.g., from deep sleep wake), preventing "Coma Device" states (`EC-I2C-LATCHUP-001`).
    *   **Guaranteed Wake-up**: `Sensor_HAL::configureForDeepSleep()` verifies sensor configuration and `EC-GUARANTEED-WAKE-001` ensures deep sleep thresholds are strictly bounded, guaranteeing the device can always wake from physical touch.
    *   **Pre-Sleep Proximity Check**: `PowerManager::enterDeepSleep()` includes a final check `Sensor_HAL::isProximityBelowThreshold()` before deep sleep to prevent entering sleep with an already elevated proximity, which would prevent edge-triggered interrupts from firing (`EC-ELEVATED-PROX-SLEEP-001`).
*   **App-Side Recovery**:
    *   **Reinstall Detection**: `ReinstallDetector.swift` uses a Keychain marker and a sandbox sentinel file to detect app reinstallation, triggering an automatic factory reset (`FactoryResetGuard`, `KeychainWipeModule`, `FactoryResetModule`) to prevent stale Keychain data from causing crashes.
    *   **HealthKit Background Delivery Timeout**: `HealthIngestCore` implements a hard timeout for background `HKAnchoredObjectQuery`s to ensure `completionHandler()` is called within iOS's 30-second budget (`Blocker B fix`).

## Repository Mapping
The repository's structure is logically organized to reflect the architectural layering and component separation.

*   **`firmware/firmware/`**: Contains the ESP32-S3 firmware project.
    *   `partitions.csv`: Defines the flash memory layout, including dual-bank OTA and NVS partitions.
    *   `platformio.ini`: PlatformIO build configuration, defining build flags for NimBLE, FreeRTOS, and custom settings.
    *   `src/HAL/`: The Hardware Abstraction Layer, encapsulating direct interaction with the ESP32-S3 peripherals.
    *   `src/Services/`: Contains the higher-level application services responsible for specific functionalities (BLE, Power, History, etc.).
    *   `src/Domain/`: Houses shared data structures, enums, and constants that define the system's core concepts and protocols.
    *   `src/main.cpp`: The primary application entry point, responsible for system initialization, FreeRTOS task creation, and high-level orchestration.
*   **`packages/app/ios/CompanionApp/`**: Contains the iOS-specific native code for the companion application.
    *   `AppDelegate.mm`: The iOS application delegate, managing the app's lifecycle, early native module initialization, and critical app-wide setup.
    *   `DeviceBLE/`: Swift modules (`DeviceBLECore.swift`, `DeviceBLEModule.swift`) for direct CoreBluetooth integration, including state restoration.
    *   `HealthIngest/`: Swift modules (`HealthIngestCore.swift`, `HealthKitQueries.swift`, `HealthNormalization.swift`, `HealthIngestSQLite.swift`, `HealthKitObserver.swift`, `NativeHealthIngestModule.swift`) for HealthKit data ingestion and SQLite persistence.
    *   `FactoryReset/`: Swift modules (`ReinstallDetector.swift`, `FactoryResetGuard.swift`, `KeychainWipeModule.swift`, `FactoryResetModule.swift`) implementing the factory reset and reinstall detection logic.
*   **`packages/app/src/`**: Contains the React Native / JavaScript codebase for the companion application.
    *   `contexts/BluetoothContext.tsx`: The React Context providing the `BluetoothHandler` instance to components.
    *   `native/DeviceBLE.ts`: The TypeScript interface to the native Swift `DeviceBLE` module.
    *   `services/ble/`: Contains TypeScript services for BLE protocol handling (`EdgeDeviceProtocolService.ts`), event synchronization (`EventSyncService.ts`), device settings (`DeviceSettingsService.ts`), and OTA management (`OtaService.ts`).
    *   `services/health/`: Contains TypeScript services for HealthKit data synchronization (`HealthSyncService.ts`) and related processing.
    *   `services/`: Other core application services (e.g., `DeviceService.ts`, `SecureStorageService.ts`, `AppSetupService.ts`).
    *   `types.ts`: Shared TypeScript type definitions for the application's domain models.
    *   `utils/`: General-purpose utility functions (e.g., `DeviceIdManager.ts`, `EventEmitter.ts`).

## Related Documents
For more in-depth information on specific architectural areas, please refer to the following documents:

*   [`ble-protocol.md`](ble-protocol.md): Detailed specification of the custom binary BLE communication protocol, including frame formats, message types, and CRC algorithms.
*   [`ota-recovery.md`](ota-recovery.md): Comprehensive guide to the firmware's Over-The-Air (OTA) update mechanism, including the Flasher Mode protocol, dual-bank rollback, and recovery procedures.
*   [`power-state-machine.md`](power-state-machine.md): An in-depth explanation of the firmware's power management state machine, idle detection, sleep guards, and various wake-up sources.
*   [`failure-modes.md`](failure-modes.md): A catalog of detected edge cases, common failure scenarios, and their specific mitigation strategies implemented across the system.
*   [`task-model.md`](task-model.md): Detailed information on the FreeRTOS tasking model in the firmware, including task priorities, stack sizes, and inter-task communication patterns.
*   [`healthkit-ingestion.md`](healthkit-ingestion.md): Detailed architecture of the native iOS HealthKit ingestion pipeline, including lane definitions, query strategies, and data normalization.

## Key Takeaways
The system embodies a disciplined approach to building reliable, power-efficient, and user-centric connected devices. Key architectural takeaways include:

*   **Holistic System Design**: A tightly integrated firmware-app ecosystem where each layer complements the other for optimal performance and resilience.
*   **Robustness by Design**: Proactive failure containment (crash recovery, OTA rollback, reliable communication) is central, ensuring the system can operate unattended for extended periods.
*   **Resource Optimization**: Careful management of power and memory resources through FreeRTOS multitasking, tiered persistence, and intelligent peripheral control.
*   **Offline-First Data Model**: Data integrity and continuity are guaranteed even in the absence of connectivity, with robust synchronization mechanisms for eventual consistency.
*   **Native Platform Leverage**: Deep integration with iOS frameworks (CoreBluetooth, HealthKit, Keychain) delivers a seamless and performant user experience, including background operations and secure data handling.
*   **Explicit State Management**: Clear state machines and well-defined interfaces across all layers ensure predictable behavior and simplified debugging.
```
