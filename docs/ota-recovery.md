# OTA Recovery: Robust Firmware Updates and Device Resiliency

## 1. Overview
This document details the Over-The-Air (OTA) firmware update and recovery system for the ESP32-S3 device. It frames OTA not merely as a feature for delivering new firmware, but as a comprehensive recovery architecture designed to ensure reliability, prevent bricking, and provide robust field recovery paths for a battery-powered consumer product that lacks physical recovery buttons.

## 2. Design Constraints
The architectural design of theOTA system is fundamentally shaped by several critical hardware and operational constraints:
*   **Battery-Powered Hardware**: The 500mAh LiPo battery (`Config.h:FlasherConfig::MIN_VOLTAGE_IDLE_MV`) mandates extreme power efficiency. High-power operations like Wi-Fi transmission must be isolated to prevent brownouts.
*   **No Physical Recovery Button**: The absence of a dedicated hardware recovery input necessitates software-triggered and non-physical (e.g., power-cycle) recovery mechanisms.
*   **Field Reliability / Anti-Bricking**: The paramount requirement to prevent the device from becoming permanently inoperable after an update, especially in an uncontrolled user environment.
*   **Flash Partition Constraints**: The fixed dual-bank A/B partition layout (`partitions.csv`) dictates the OTA update strategy and rollback capabilities.
*   **Update Asset Boundaries**: The system is designed to update only the main application firmware (`firmware.bin`), not critical bootloader or partition data.


## 4. System Architecture
The OTA and recovery system is a collaborative architecture spanning the mobile application and the ESP32-S3 firmware, leveraging hardware features for robustness.

*   **Mobile Application (`packages/app`)**:
    *   **`OtaService`**: The core app-side orchestrator for OTA. It bundles the `firmware.bin` asset, communicates with the device's `FlasherService` via HTTP, and performs post-update verification.
    *   **`EdgeDeviceProtocolService`**: Manages the BLE binary protocol, enabling the app to send control commands like `MSG_ENTER_OTA_MODE`.
    *   **`BluetoothHandler`**: Handles the underlying BLE connection, managing device discovery, connection state, and providing an interface for the `EdgeDeviceProtocolService` to send/receive data. It also manages complex reconnection and bonding error handling, leveraging the native iOS `DeviceBLENative` module for enhanced reliability.
    *   **`OtaPostUpdateVerifier`**: A specialized module that reconnects to the device via BLE post-reboot to confirm firmware version and device identity.

*   **ESP32-S3 Firmware (`firmware/firmware`)**:
    *   **`Bootloader`**: The ESP32's built-in bootloader. It reads `otadata` (`partitions.csv`) to decide which application partition (`app0` or `app1`) to boot. It supports `Pending Verify` states and automatic rollback.
    *   **`OTAManager`**: The primary firmware service for OTA management. It handles `MSG_OTA_INFO/START/DATA/COMMIT/ABORT` (though the actual update transfer uses `FlasherService`). Crucially, it contains `performSelfTest()` for post-boot validation and invokes rollback.
    *   **`FlasherService`**: A completely isolated, minimal application invoked when the device enters "Flasher Mode." It sets up a Wi-Fi SoftAP and an HTTP server, managing the direct firmware upload. It also performs critical pre-upload checks (battery, partition size).
    *   **`ProtocolHandler`**: On the main application, handles incoming BLE commands, including `MSG_ENTER_OTA_MODE`, which triggers the device to reboot into `FlasherService`.
    *   **`RTC Memory (RTC_NOINIT_ATTR)`**: Used by `main.cpp` to store `g_boot_mode` (flasher mode request) and `g_stability_counter` (power-cycle count) persistently across resets, even if NVS or flash are corrupted.
    *   **`HAL (Hardware Abstraction Layer)`**: Provides interfaces to peripherals like `LED_HAL`, `Sensor_HAL`, and `StorageHAL`, used by `FlasherService` for basic operation (LED, ADC) and `OTAManager` for self-tests (sensor check, NVS access).

## 5. Update Paths
The the system utilizes two primary update paths: the standard Flasher Mode for robust firmware uploads and an implicit BLE-based path for smaller, less critical updates.

### 5.1 Flasher Mode Recovery Path (Standard Firmware Update)
This is the recommended and primary path for delivering full firmware updates, explicitly designed for safety and reliability under battery constraints.

*   **Why it exists**: The Flasher Mode decouples the power-intensive Wi-Fi upload process from the main application's sensor stack and BLE operations. This prevents brownouts on a 500mAh battery and avoids system instability during critical flash write operations. It also provides an out-of-band recovery channel even if the main application is unbootable or unresponsive.
*   **Flow**:
    1.  **Trigger**: The user initiates an update from the app UI. The app sends `MSG_ENTER_OTA_MODE` (0x38) via BLE (`ProtocolHandler.sendEnterOtaMode()`).
    2.  **Device Acknowledges & Reboots**: The device `ACK`s the `MSG_ENTER_OTA_MODE`, sets an RTC flag (`g_boot_mode = MODE_FLASHER`), and performs a software restart (`esp_restart()`).
    3.  **App Guides User**: The app disconnects BLE and instructs the user to connect their phone to the device's Wi-Fi SoftAP (configured SSID and password from `Config.h`).
    4.  **Device Enters Flasher Mode**: On reboot, `main.cpp` detects `MODE_FLASHER` from RTC memory and immediately invokes `FlasherService::run()` which takes complete control of the device.
    5.  **Flasher Service Initialization**: `FlasherService` starts:
        *   Initializes minimal hardware (LED, ADC, WDT). Sets CPU to 240MHz, disables Wi-Fi power save for stability.
        *   **2-Stage Power Gating**: Performs a critical battery check: first at idle, then under Wi-Fi load (`Config.h:FlasherConfig::MIN_VOLTAGE_IDLE_MV`, `MIN_VOLTAGE_LOAD_MV`, `MAX_VOLTAGE_SAG_MV`). If power fails, it aborts and reboots.
        *   Starts Wi-Fi SoftAP and an HTTP server (`FlasherService.cpp:setupServer()`).
        *   Transitions to `FlasherPhase.READY`.
    6.  **Start Session & Erase**: The app POSTs to `/start` on `http://192.168.4.1` with `X-Session-ID` and `X-Total-Size` headers (`OtaService.startSession()`). The device acknowledges immediately (HTTP 200) and *defers the flash erase* to its main loop, transitioning to `FlasherPhase.ERASING`.
    7.  **Wait for Erase Completion**: The app polls `/status` (`OtaService.waitForPhase()`) until the device transitions to `FlasherPhase.RECEIVING` (indicating flash erase is complete). During this phase, the device's LED is purple.
    8.  **Chunked Upload**: The app reads `firmware.bin` in 4KB chunks (`CHUNK_SIZE`) and POSTs each chunk to `/update_chunk` as `multipart/form-data` (`OtaService.uploadFirmware()`).
        *   **Multipart Encoding (v2.7.0 Fix)**: Chunks are wrapped in a `multipart/form-data` envelope (`buildMultipartBody()`). This is critical to ensure the ESP32 WebServer's `upload` handler (`upload.buf`) is used, which is binary-safe, preventing crashes from NUL bytes.
        *   The device buffers and writes each chunk to the inactive `app` partition (`Update.write()`), updates its running CRC.
        *   The app waits for device acknowledgment (`waitForChunkCommit()`) after each chunk.
    9.  **Verification & Commit**: After the final chunk, the device performs a final CRC check (`Update.end(true)`) and marks the new firmware as bootable, transitioning to `FlasherPhase.COMPLETE`.
    10. **Reboot**: The device `esp_restart()`s.
    11. **Post-Update Verification**: The app initiates BLE reconnection and verifies the new firmware version and device identity (`OtaPostUpdateVerifier.verify()`), updating local device records.

### 5.2 BLE-Based OTA Path (Implicit / Limited Use)
While the firmware includes `OTAManager` methods for `MSG_OTA_START`, `MSG_OTA_DATA`, and `MSG_OTA_COMMIT` via BLE, this path is *not* the primary method for full firmware updates.

*   **Why it exists**: It can be used for very small data transfers or metadata updates, or in scenarios where Wi-Fi is absolutely unavailable. However, it's not suitable for large `firmware.bin` payloads.
*   **Limitations**: BLE throughput is significantly lower than Wi-Fi. Transmitting large files over BLE is slower, more susceptible to timeouts (especially with iOS background execution), and still incurs significant battery drain on both device and phone. The `Config.h:OTAConfig::CHUNK_SIZE` (180 bytes) is a remnant of this BLE-optimized chunking.
*   **Usage**: The current mobile application primarily uses the Flasher Mode path, rendering this BLE-based OTA path largely implicit and de-emphasized for full firmware upgrades.

## 6. Boot Validation, Acceptance, and Rollback
This is the core reliability mechanism, ensuring only verified, healthy firmware images remain active, preventing device bricking.

*   **A/B Partition Model**: The flash memory (`partitions.csv`) is divided into two application partitions: `app0` (the currently running image) and `app1` (the target for the next update). This provides an isolated space for new firmware and a guaranteed fallback.
*   **`Pending Verify` State**: After a successful `FlasherService` upload and reboot, the `Bootloader` boots the newly flashed image into an `ESP_OTA_IMG_PENDING_VERIFY` state. This indicates the firmware is new and requires self-validation.
*   **Mandatory Self-Test (`OTAManager.performSelfTest()`)**:
    *   On every boot into an `ESP_OTA_IMG_PENDING_VERIFY` state, `main.cpp` calls `OTAManager::validateBootedFirmware()`, which executes a comprehensive `performSelfTest()`.
    *   **Test Criteria**: This self-test validates critical hardware and software components:
        *   **I2C Sensor Communication**: Checks if the `Proximity Sensor` sensor is responsive, with retries (`Config.h:FlasherConfig::SENSOR_CHECK_RETRIES`, `SENSOR_CHECK_DELAY_MS`) to mitigate transient issues post-boot.
        *   **Heap Memory Availability**: Ensures sufficient free and contiguous heap memory to prevent runtime crashes.
        *   **Flash Partition Integrity**: Verifies the `app` partitions are valid and correctly sized (`partitions.csv`).
        *   **NVS Read/Write Access**: Confirms non-volatile storage is accessible for bonding keys and configuration.
        *   **LED HAL Operational**: Checks basic LED functionality.
    *   **Critical Distinction (HARD vs. SOFT Failures)**:
        *   **HARD Failures**: (e.g., heap corruption, NVS access failure) indicate a fundamental firmware issue. These trigger *rollback*.
        *   **SOFT Failures**: (e.g., sensor not found after retries, LED not responsive) indicate a hardware problem. These are logged as warnings and do *not* trigger rollback (prevents rollback loops for broken hardware).
*   **Firmware Acceptance (Commit)**: If all `performSelfTest()` checks (specifically, no HARD failures) pass within `Config.h:OTAConfig::SELF_TEST_TIMEOUT_MS`, `esp_ota_mark_app_valid_cancel_rollback()` is called. This marks the new image as permanently bootable, canceling the automatic rollback.
*   **Automatic Rollback**: If `performSelfTest()` detects any HARD failure, `esp_ota_mark_app_invalid_rollback_and_reboot()` is immediately invoked. This command:
    *   Marks the new (unhealthy) application image as invalid.
    *   Instructs the bootloader to revert to the previously validated `app` partition on the next reboot.
    *   Performs an immediate `esp_restart()`.
*   **NVS Protection During Rollback (`EC-OTA-PROTECT-NVS`)**: `main.cpp` explicitly guards against triggering an NVS erase during `performCrashRecoveryCheck()` if the device is booting a `PENDING_VERIFY` image. This prevents accidental deletion of user data (bonding keys, configuration) when a new firmware image fails and rolls back.
*   **Why Rollback Prevents Bricking**: By guaranteeing a return to the last-known-good firmware, the A/B partition scheme with mandatory self-test makes the update process highly fault-tolerant against corrupt, incompatible, or buggy firmware images.

## 7. Failure Handling Matrix
The system is designed to detect and respond deterministically to various failure conditions, ensuring recovery and guiding the user.

| Failure Condition                | Detection Point                            | Immediate Device Response                             | App UI Feedback (if any)                                       | Recovery Action                                                     |
| :------------------------------- | :----------------------------------------- | :---------------------------------------------------- | :------------------------------------------------------------- | :------------------------------------------------------------------ |
| **Low Battery (Idle)**           | Device (FlasherService, Stage 1)           | Abort Flasher Mode, Reboot, Red LED flashes (`FlasherError::LOW_BATTERY_IDLE`) | "Device battery too low for update"                            | Charge device (min 20%), retry.                                     |
| **Low Battery (Under Load)**     | Device (FlasherService, Stage 2)           | Abort Flasher Mode, Reboot, Red LED flashes (`FlasherError::LOW_BATTERY_LOAD`) | "Battery cannot sustain Wi-Fi, try again after charging"         | Charge device, ensure stable power, retry.                          |
| **Wi-Fi AP Failed to Start**     | Device (FlasherService)                    | Abort Flasher Mode, Reboot, Red LED flashes (`FlasherError::WIFI_FAILED`) | "Device failed to start Wi-Fi AP"                              | Power cycle device, retry.                                          |
| **Network Timeout (App to Device)** | App (OtaService fetch)                      | Reattempt connection/request                                  | "Cannot reach device, check Wi-Fi connection"                  | Verify Wi-Fi the device's SoftAP connection, retry.                 |
| **Upload Stalled (Progress Timeout)** | Device (FlasherService watchdog)           | Abort Flasher Mode, Reboot, Red LED flashes (`FlasherError::TIMEOUT_PROGRESS`) | "Upload stalled, check Wi-Fi signal"                           | Improve Wi-Fi signal, retry upload.                                 |
| **Incomplete/Corrupt Chunk**     | Device (FlasherService, CRC check)         | Reject chunk, await retry or NACK (`FlasherError::INVALID_CHUNK`) | "Chunk upload failed, retrying..."                             | App retries chunk automatically.                                    |
| **Flash Write Failure**          | Device (FlasherService, `Update.write()`)  | Abort Flasher Mode, Reboot, Red LED flashes (`FlasherError::FLASH_WRITE_FAILED`) | "Firmware write failed"                                        | Retry update. If persistent, contact support.                       |
| **Unhealthy First Boot**         | Device (OTAManager, `performSelfTest()`)   | Automatic Rollback to previous firmware (`esp_ota_mark_app_invalid_rollback_and_reboot()`) | "Firmware update failed, reverting to previous version"          | App verifies old version. User can try re-updating or contact support. |
| **Main App Unreachable (Boot Loop)** | Device (main.cpp, `g_stability_counter`) | Enter Flasher Mode via power-cycle pattern (`RECOVERY_TRIGGER_COUNT`) | "Device unresponsive, enter recovery mode"                     | Power cycle device 3 times quickly, upload new firmware via Flasher Mode. |
| **BLE Bonding Lost / Security Mismatch** | App (BluetoothHandler, CBError) / Device (ProtocolHandler, `ERR_NOT_BONDED`) | App `handleBondingError()`: disconnect, clear local device data, prompt user to "Forget Device" in iOS settings. Device: `MSG_CLEAR_BONDS` clears server-side bonds. | "Pairing Update Required: Forget device in iOS settings" | Forget device in iOS settings, re-pair from app.                    |
| **NVS Corruption Boot Loop**     | Device (main.cpp, `g_crash_counter`)       | `performCrashRecoveryCheck()`: Erase NVS, Soft Reset, Red LED flashes. | "Device experiencing issues, resetting to factory defaults"      | Device self-heals, user re-pairs and reconfigures.                  |

## 8. Operational Invariants and Safety Boundaries
These are fundamental rules for the system's safe operation and must be strictly maintained.

*   **OTA Payload (`firmware.bin` only)**: The OTA update mechanism is designed exclusively for the main application firmware (`firmware.bin`). Only this binary is bundled with the app (`OtaService.ts:FIRMWARE_MODULE`).
*   **Excluded Artifacts**: The ESP32 bootloader (`bootloader.bin`) and partition table (`partitions.bin`) are **NEVER** OTA-updatable. Attempting to update these would result in a hard brick, requiring physical re-flashing.
*   **Upload Size Constraint**: Any `firmware.bin` payload must fit entirely within one of the `app` partitions (`app0` or `app1`), as defined in `partitions.csv`. The `FlasherService` enforces this via `X-Total-Size` validation.
*   **Recovery Path Reachability**: The Flasher Mode recovery path (triggered by `MSG_ENTER_OTA_MODE` or the power-cycle pattern) must always remain accessible and functional, even if the main application partition is corrupt or unbootable.
*   **Validation Before Acceptance**: Any newly flashed firmware image must undergo a mandatory `performSelfTest()` post-boot (`ESP_OTA_IMG_PENDING_VERIFY` state) before being marked as a valid, permanent boot image.

## 9. Mobile Integration Contract
The mobile application plays a crucial role in orchestrating the OTA update and recovery process, adhering to a defined contract with the firmware.

*   **Firmware Asset Bundling**: The `firmware.bin` is bundled as an asset within the React Native application (`packages/app/assets/firmware/firmware.bin`), resolved via `expo-asset`.
*   **Enter-OTA-Mode Protocol Trigger**: The app initiates Flasher Mode by sending the `MSG_ENTER_OTA_MODE` (0x38) binary protocol command via BLE (`EdgeDeviceProtocolService.sendEnterOtaMode()`).
*   **Network Transition**: The app is responsible for:
    *   Detecting the BLE disconnect following `MSG_ENTER_OTA_MODE`.
    *   Guiding the user to manually connect their phone to the device's Wi-Fi SoftAP (configured in `Config.h`).
    *   Switching its network stack to use the Wi-Fi connection for HTTP communication with the device.
*   **Payload Delivery**: The app delivers the `firmware.bin` payload in 4KB chunks (`CHUNK_SIZE`) using HTTP POST requests to `http://192.168.4.1/update_chunk`. Chunks are sent as `multipart/form-data` with specific headers (`X-Session-ID`, `X-Chunk-Index`, `X-Total-Chunks`, `X-Chunk-Size`).
*   **Status Monitoring**: The app continuously polls the device's `/status` HTTP endpoint (`OtaService.getStatus()`) to monitor the `FlasherPhase`, upload progress (`written`/`total`), and detect any errors.
*   **Post-Update Verification**: After the device reboots, the app reconnects via BLE and uses `OtaPostUpdateVerifier` to confirm the new firmware version (`HelloAckPayload`) and validate the device's identity (`hardwareId`).
*   **Bilateral Bond Clearing**: On detecting a BLE bonding error or user-initiated "forget device" action, the app sends `MSG_CLEAR_BONDS` to the device (`EdgeDeviceProtocolService.sendClearBonds()`) to ensure device-side bonding keys are also cleared, facilitating a clean re-pairing.
*   **Boundary between App Orchestration and Device Logic**: The app manages the high-level flow and user interaction. The device (`FlasherService`) is solely responsible for low-level hardware control, flash operations, power management, and bootloader interactions.

## 10. Observability and Debugging
Effective debugging is crucial for identifying and resolving OTA failures in the field.

*   **App-side OTA Logs (`ota_debug_log.txt`)**: The `OtaFileLogger` (`OtaService.ts`) maintains a persistent, file-based log (up to 500KB) of all OTA operations on the mobile device. This log captures network events, HTTP requests/responses, phase transitions, and errors, providing a forensic record for offline analysis.
*   **Device Status (`/status` endpoint)**: The device's `FlasherService` exposes a `/status` HTTP endpoint (`FlasherService.cpp`) returning a JSON object (`FlasherStatus`) with the current `phase`, bytes `written`/`total`, `error` code (`FlasherError`), and a human-readable `msg`. This is a primary real-time diagnostic tool.
*   **Device Serial Logs**: For development builds, the firmware provides extensive `ESP_LOG` messages (`main.cpp`, `OTAManager.cpp`, `FlasherService.cpp`). Critical errors trigger `systemPanic()`, which halts execution and produces a distinct LED pattern while continuously feeding the WDT.
*   **BLE Error Events**: The `BluetoothHandler` (app-side) emits `onBondingLost` and `onOperationRejected` events (`DeviceBLENative.ts`), providing immediate feedback on BLE communication failures.
*   **Firmware Version and Hardware ID**: The `HelloAckPayload` transmitted by the device contains `firmwareMajor/Minor/Patch` and the stable `hardwareId` (MAC address), essential for post-update verification and device identification.

## 11. Non-Goals and Limits
This section clarifies what theOTA Recovery system intentionally does not attempt to solve, setting realistic expectations.

*   **Bootloader and Partition Table Updates**: The system is explicitly designed *not* to OTA update the ESP32 bootloader or modify the partition table. These components are considered immutable once flashed at manufacturing.
*   **Hardware Damage Recovery**: The system cannot recover from severe physical hardware damage (e.g., flash memory corruption, Wi-Fi module failure, damaged sensor). In such cases, physical repair or replacement is required.
*   **Multi-Client Uploads**: The Wi-Fi Flasher Mode is designed for a single client (the mobile app) to upload firmware at a time. It does not support concurrent uploads from multiple sources.
*   **Generic Device Flashing**: This system is purpose-built for the ESP32-S3 device and its specific firmware/app contract. It is not a generic tool for flashing arbitrary ESP32 devices.
```
