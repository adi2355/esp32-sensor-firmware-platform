# Sensing / Calibration / ML: Embedded Intelligence Pipeline

This document serves as the authoritative technical explanation of the firmware's embedded intelligence pipeline, which transforms raw proximity sensor data into reliable, low-latency user interaction classifications. It details the architecture, design rationale, and operational specifics of the sensing, calibration, and machine learning components, demonstrating how the system overcomes real-world challenges like environmental noise, false positives, and device drift on a resource-constrained ESP32-S3 microcontroller.

## 1. Overview

The device's core functionality relies on precisely detecting user interactions through a proximity sensor, even in challenging and variable environments. This subsystem is responsible for converting raw, often noisy, physical signals into trustworthy digital events. It encompasses a multi-layered approach: from robust signal acquisition and adaptive thresholds to sophisticated heuristic safeguards, on-device machine learning, and personalized adaptation. This comprehensive pipeline ensures high accuracy, reliability, and responsiveness while operating efficiently on limited embedded hardware.

## 2. Design Goals

The design of the sensing, calibration, and machine learning pipeline is optimized for the following priorities:

* **Detection Accuracy:** Precisely distinguish genuine user interactions from environmental noise and false positives.
* **False-Positive Resistance:** Robustly filter out spurious triggers caused by reflections (e.g., from glass surfaces), vibrations, or electrical interference.
* **Environmental Robustness:** Dynamically adapt detection behavior to variations in device mounting, ambient light conditions, and battery voltage levels.
* **Real-time Responsiveness:** Maintain a high sampling rate (20Hz) and minimize processing latency to provide immediate feedback and capture interaction nuances.
* **Low-Power and Bounded-Resource Operation:** Execute efficiently on the ESP32-S3's limited CPU, memory, and power budget, leveraging non-blocking I/O and asynchronous tasking.
* **Device-Specific Adaptability:** Implement personalized calibration and machine learning models to ensure consistent, high performance across individual devices and diverse user contexts.
* **Self-Healing:** Proactively detect and automatically recover from unstable sensor states, internal anomalies, or calibration drifts.

## 3. System Context

The sensing, calibration, and ML pipeline is a core component within the firmware's FreeRTOS multi-tasking architecture. It operates in close conjunction with several other services, forming an integrated data processing flow.

The physical interface is the proximity sensor, managed by the `Sensor_HAL`. Data flows from the `Sensor_HAL` to a dedicated `sensorTask`, which orchestrates the initial stages of intelligence. This task then feeds raw samples to the `DataLoggerService` for ML windowing and `handleDetection` for heuristic classification. Outputs are consumed by `HistoryService` for event persistence, `InferenceService` for advanced classification, and `PersonalizationService` for adaptive model parameters. `ConfigService` provides user-defined sensitivity settings, while `PowerManager` coordinates deep sleep entry and exit based on sensor activity. `ProtocolHandler` handles communication with the mobile application, including sensor-related commands and event transmission.


## 4. End-to-End Signal Pipeline

The journey of a physical interaction from the proximity sensor to a confirmed, logged event or a machine learning classification involves a multi-stage process designed for robustness and intelligence:

1. **Raw Sensor Acquisition:** The `Sensor_HAL`'s `readProximity` method actively polls the proximity sensor at a consistent 20Hz rate. This retrieves the raw ambient-compensated infrared reflection signal.
2. **Baseline & Noise Handling:** Within `Sensor_HAL`, the `PS_CANC` (Proximity Cancellation) register is dynamically managed through calibration (`Sensor_HAL::calibrate`). This actively subtracts static ambient IR interference, ensuring an idle sensor typically reports a value near `~0`.
3. **Adaptive Threshold Evaluation:** The `sensorTask` compares the incoming raw readings against a set of three dynamic thresholds (`wake`, `detection`, `critical`). These thresholds are not static; they are derived from calibration statistics, adjusted by user-defined `_userSensitivity` (from `ConfigService`), and can be further modified by a temporary `_environmentBackoff` for proactive environmental adaptation.
4. **Heuristic Event Logic:** The `handleDetection` function in `main.cpp` implements a robust heuristic state machine. It employs a multi-level Schmitt trigger with hysteresis, enforcing a `MIN_DETECTION_DURATION_MS` (1 second) and requiring the signal to exceed a `CRITICAL_DETECTION_THRESHOLD` at some point during the interaction for it to be considered a valid event. This two-factor validation (duration + strength) filters out spurious triggers.
5. **Sample Window Capture:** Concurrently, the `DataLoggerService::appendSample` method continuously feeds raw sensor samples into a rolling `SampleWindow`. Each window (e.g., 40 samples, representing a 2.0-second span at 20Hz) is packaged with a `WindowHeader` containing vital runtime metadata, such as current thresholds, sensitivity, and BLE state. This buffer is immediately available for ML inference.
6. **Feature Extraction:** The `InferenceService::extractFeaturesNormalized` component extracts 10 statistical and temporal features from each `SampleWindow`. These include measures like mean, standard deviation, peak-to-peak amplitude, and various derivative statistics. Crucially, feature values are normalized using dynamically updated sensor baselines (`baselineMean`, `baselineSigma`, `dynamicRange`) provided by `PersonalizationService`.
7. **Hybrid Inference:** The `InferenceService::predictHybrid` function then applies a lightweight hybrid classification model. This model fuses scores from a global, pre-trained logistic regression model with locally derived, device-specific prototypes and adaptive biases (both sourced from `PersonalizationService`). This fusion refines the classification beyond simple thresholds.
8. **Final Classification & Decision:** The output of the `InferenceService` is an `InferenceResult` containing a `predictedLabel` (e.g., `GestureLabel::APPROACH`, `GestureLabel::HOVER`) and a confidence score. This result is used for debugging and advanced analytics. For core product functionality, the heuristic `handleDetection` already validated user interactions, which then trigger the logging of a `DetectionEvent`.
9. **Persistence & Adaptation Hooks:** Validated `DetectionEvent`s are logged to the `HistoryService` for persistent storage (first in RTC memory, then backed up to NVS). Raw `SampleWindow`s are queued by `DataLoggerService` for asynchronous storage to SPIFFS. `InferenceResult`s feed back into the `PersonalizationService` to update and adapt the ML model's local parameters over time.

## 5. Sensing Model

Reliable interaction detection begins with an accurate understanding of the physical sensor's behavior and the environmental signals it perceives.

**5.1. Proximity Sensor Operating Principles**
The the device utilizes the proximity sensor, which operates by emitting an infrared (IR) light pulse and measuring the reflection. The intensity of the reflected light correlates with the proximity of an object. This active sensing approach provides consistent readings by minimizing interference from ambient visible light.

**5.2. Sampling Cadence**
The `sensorTask` (`main.cpp`) drives the `Sensor_HAL` to acquire raw proximity samples at a fixed rate of **20Hz** (`Config.h::Timing::SAMPLING_PERIOD_MS = 50ms`). This consistent sampling cadence balances real-time responsiveness with computational and power efficiency on the ESP32-S3. The use of `vTaskDelayUntil` ensures mathematically precise periodic execution, crucial for accurate temporal feature extraction in the ML pipeline.

**5.3. Baseline Management and Noise Floor**
A key challenge in proximity sensing is differentiating a user interaction from constant ambient reflections (e.g., a device resting on a table, or light from a nearby window).
* **Active Cancellation (`PS_CANC`):** The `Sensor_HAL` actively manages the `PS_CANC` (Proximity Cancellation) register on the Proximity Sensor. During calibration, this register is tuned to electrically subtract the static ambient IR reflections inherent to the device's immediate environment. The goal is for an idle sensor (no object nearby) to produce readings near `~0`.
* **Noise Floor:** Even after ambient cancellation, inherent sensor noise, minor environmental fluctuations, and micro-vibrations contribute a low-level, random signal. This "noise floor" is characterized statistically during calibration by its mean and standard deviation, which are crucial for deriving robust detection thresholds.

**5.4. Multi-Level Threshold Tiers**
Instead of a single binary threshold, the firmware employs a three-tier adaptive threshold system for robust interaction detection:

* **`WAKE_THRESHOLD`:**
 * **Purpose:** The lowest, most sensitive threshold. Primarily used to detect minimal activity sufficient to wake the device from deep sleep.
 * **Derivation:** Calculated as `Mean + 2 * Standard Deviation` of the noise floor (`ProximityConfig::WAKE_Z_SCORE=2.0f`). This provides ~95.4% confidence that a signal above this level is not random noise.
* **`DETECTION_THRESHOLD`:**
 * **Purpose:** The primary trigger for initiating a potential user interaction. When the proximity signal exceeds this, the device transitions to a "detection active" state (e.g., LED turns green) and starts a duration timer.
 * **Derivation:** Calculated as `Mean + 3 * Standard Deviation` of the noise floor (`ProximityConfig::DETECTION_Z_SCORE=3.0f`). This provides ~99.7% confidence, offering a balance between responsiveness and false-positive reduction. This threshold is further adjusted by user `sensitivity`.
* **`CRITICAL_DETECTION_THRESHOLD`:**
 * **Purpose:** The highest, most stringent threshold. A potential detection *must* exceed this level at some point during its duration to be confirmed as a legitimate user interaction. This serves as a "second factor" authentication for detections.
 * **Derivation:** Calculated as `Mean + 5 * Standard Deviation` of the noise floor (`ProximityConfig::CRITICAL_Z_SCORE=5.0f`), with a `MIN_CRITICAL_THRESHOLD_ABS` minimum floor. This provides extremely high confidence (~99.99994%) that the signal is a strong, intentional interaction, effectively filtering out weak environmental reflections.

**5.5. Hysteresis**
To prevent rapid state flickering (jitter) when the proximity signal hovers near a threshold, the system employs hysteresis. The `DETECTION_THRESHOLD` (trigger-on) is distinct from the `releaseThreshold` (trigger-off). The `releaseThreshold` is calculated as `DETECTION_THRESHOLD * 0.8f` (`20%` lower), but this gap is capped by `ProximityConfig::MAX_HYSTERESIS_GAP` (`200` units) to prevent excessive deadbands at high threshold values. This ensures stable transitions between `IDLE` and `DETECTION_ACTIVE` states.

**5.6. User Sensitivity (`_userSensitivity`)**
The `ConfigService` manages a user-configurable `_userSensitivity` setting (0-100). This value is applied by `Sensor_HAL::recalculateThresholdsFromSensitivity` as a multiplier to the *base* calibrated `DETECTION_THRESHOLD` and `CRITICAL_DETECTION_THRESHOLD`:
* `0%` Sensitivity: Multiplier `~3.0x` (least sensitive, very high thresholds).
* `50%` Sensitivity: Multiplier `1.0x` (neutral, uses base calibrated thresholds).
* `100%` Sensitivity: Multiplier `~0.4x` (most sensitive, very low thresholds).
This dynamic scaling uses fixed-point arithmetic (`FP_SCALE=1024`) for deterministic, efficient execution on the ESP32-S3, providing professional-grade control over detection responsiveness.

## 6. Calibration Model

Calibration is a critical, multi-stage process that dynamically adapts the sensor's operating parameters to the device's specific environment, ensuring accurate and reliable detection. It is not a one-time setup but a continuous, adaptive process central to the system's robustness.

```mermaid
graph TD
 subgraph Environment
 A[Ambient IR] --> B(Surface Reflections)
 C[Physical Mounting] --> B
 D[Sensor Noise] --> B
 end

 subgraph Calibration Process (Sensor_HAL::calibrate)
 B --> E[Clear PS_CANC Register]
 E --> F[Measure Raw Ambient Baseline]
 F --> G[Set PS_CANC = Baseline]
 G --> H[Delay (200ms, pipeline flush)]
 H --> I[Measure Noise Floor (Mean & StdDev)]
 I --> J[Derive Base Thresholds (Z-scores)]
 J --> K[Apply Initial Offsets & Sane Bounds]
 K --> L[Apply User Sensitivity Scaling]
 end

 subgraph Runtime Consumers
 L --> M[Sensor_HAL (Effective Thresholds)]
 L --> N[ConfigService (Persistent Settings)]
 L --> O[PersonalizationService (ML Normalization Baseline)]
 end

 M -- "Proximity Checks" --> P(handleDetection)
 N -- "Sensitivity" --> L
 O -- "Mean, Sigma, Dynamic Range" --> Q(InferenceService Features)

 style A fill:#ECEFF1,stroke:#90A4AE
 style B fill:#CFD8DC,stroke:#607D8B
 style C fill:#ECEFF1,stroke:#90A4AE
 style D fill:#ECEFF1,stroke:#90A4AE
 style E fill:#FFF2CC,stroke:#FFA000
 style F fill:#FFF2CC,stroke:#FFA000
 style G fill:#FFF2CC,stroke:#FFA000
 style H fill:#E0F7FA,stroke:#00BCD4
 style I fill:#FFF2CC,stroke:#FFA000
 style J fill:#FFF2CC,stroke:#FFA000
 style K fill:#FFF2CC,stroke:#FFA000
 style L fill:#FFEBEE,stroke:#E57373
 style M fill:#E8F5E9,stroke:#4CAF50
 style N fill:#FFE0B2,stroke:#FF9800
 style O fill:#F0F4C3,stroke:#C0CA33
 style P fill:#4CAF50,stroke:#2E7D32
 style Q fill:#E57373,stroke:#B71C1C

 linkStyle 0 stroke:#607D8B
 linkStyle 1 stroke:#607D8B
 linkStyle 2 stroke:#607D8B
 linkStyle 3 stroke:#FFA000
 linkStyle 4 stroke:#FFA000
 linkStyle 5 stroke:#FFA000
 linkStyle 6 stroke:#00BCD4
 linkStyle 7 stroke:#FFA000
 linkStyle 8 stroke:#FFA000
 linkStyle 9 stroke:#E57373
 linkStyle 10 stroke:#E57373
 linkStyle 11 stroke:#E57373
 linkStyle 12 stroke:#E57373
 linkStyle 13 stroke:#4CAF50
 linkStyle 14 stroke:#FF9800
 linkStyle 15 stroke:#C0CA33
 linkStyle 16 stroke:#2E7D32
 linkStyle 17 stroke:#B71C1C
```
**Figure B: Calibration Influence Diagram**

**6.1. Why Calibration is Required**
Proximity sensor readings are highly sensitive to:
* **Environmental Context:** The distance and reflectivity of nearby surfaces (e.g., a glass table) significantly alter the sensor's baseline.
* **Device Mounting:** Slight variations in sensor placement or housing can cause unit-to-unit differences.
* **Ambient Light:** While `PS_CANC` mitigates IR, strong light sources can introduce noise.
Calibration addresses these variables by adapting the device to its unique operating environment, ensuring consistent detection performance.

**6.2. The Calibration Process (`Sensor_HAL::calibrate`)**
The `Sensor_HAL::calibrate` method performs a multi-step, statistical analysis to establish the sensor's baseline and noise characteristics:

1. **Dark Calibration :** On cold boot, the main LED is explicitly turned OFF, followed by a `500ms` delay (`main.cpp`). This allows the battery's voltage rail to stabilize from any initial load, preventing an artificially low baseline that could lead to "stuck detection" states later.
2. **Baseline Measurement:** The `PS_CANC` register in the Proximity Sensor is temporarily cleared to zero. The sensor then measures the *raw ambient* IR reflection of its immediate environment. This value represents the static background signal (e.g., from a table surface).
3. **Ambient Cancellation:** The measured raw ambient baseline is then written back into the `PS_CANC` register. This electrically cancels out the static background signal, so subsequent readings primarily reflect changes *relative* to this baseline, with an ideal idle reading near `~0`. A `200ms` delay ensures the sensor's internal measurement pipeline flushes with the new cancellation value.
4. **Noise Floor Characterization:** With the ambient background cancelled, `Sensor_HAL` collects numerous samples (`2000ms` duration in `main.cpp`) to statistically characterize the remaining low-level fluctuations, identifying the noise floor's mean and standard deviation.
5. **Threshold Derivation:** Optimal `wake`, `detection`, and `critical` thresholds are then derived from these noise statistics. Each threshold is calculated as `Mean + Z_SCORE * Standard Deviation`, where `Z-scores` (`ProximityConfig::WAKE_Z_SCORE`, `DETECTION_Z_SCORE`, `CRITICAL_Z_SCORE`) define the statistical confidence required for each detection level.
6. **Initial Offsets & Safety Bounds:** Minimum absolute offsets (e.g., `MIN_WAKE_OFFSET`) are applied to ensure thresholds are physically meaningful even in extremely low-noise environments. Crucially, enforces sanity `BOUNDS` (`WAKE_THRESHOLD_MIN_SANE`, `WAKE_THRESHOLD_MAX_SANE`) to reject corrupted calibration data (e.g., if the sensor was pointed at a highly reflective surface) that could otherwise set impossibly high thresholds, effectively "bricking" the device from waking.
7. **Dynamic Sensitivity Adjustment :** During the calibration process, the user's `_userSensitivity` setting is temporarily reset to its neutral default (`50`) to ensure unbiased noise floor sampling. After base thresholds are determined, the user's preferred sensitivity is re-applied (`Sensor_HAL::setBaseThresholds`).

**6.3. Persistence and Restoration**
The calculated base thresholds (`wakeThresholdLow`, `detectionThresholdLow`, `criticalDetectionThreshold`) are stored in an `RTC_DATA_ATTR CalibrationData` struct (`DetectionEvent.h`). This structure is protected by a `CRC32` checksum, ensuring integrity across deep sleep cycles and soft reboots.

On boot, `main.cpp` attempts to load this `RTC_DATA_ATTR` calibration data . If valid and sane, it's loaded instantly, allowing the device to be ready in `<1s` (fast boot). If corrupted or invalid (`isThresholdSane` fails), the device automatically reverts to safe default thresholds. ensures `Sensor_HAL::setBaseThresholds` correctly sets the raw calibrated values, allowing `_userSensitivity` to be properly applied afterward.

**6.4. ML Integration**
The measured `_lastCalMean` and `_lastCalSigma` (noise statistics) from `Sensor_HAL::calibrate` are cached within `Sensor_HAL`. These values are then made available to the `PersonalizationService` to dynamically normalize features extracted for the ML pipeline, preventing hard-coded normalization constants that would fail to adapt across different physical environments.

## 7. Heuristic Safeguards and Self-Healing

Beyond dynamic thresholds, the firmware incorporates a robust set of heuristic safeguards and self-healing logic directly into the `handleDetection` function (running within `sensorTask`). These mechanisms provide immediate, deterministic responses to common sensor anomalies and environmental interference, forming a critical reliability layer independent of the ML pipeline.

* **Multi-Level Thresholding :**
 * **Problem:** Weak, persistent reflections, particularly from glass surfaces, can consistently exceed a single `DETECTION_THRESHOLD`, leading to false positives and jittering LED feedback.
 * **Safeguard:** Requires a potential detection to not only cross the `DETECTION_THRESHOLD` but also *at some point during its duration* to exceed a stricter `CRITICAL_DETECTION_THRESHOLD` (5-sigma). Additionally, the interaction must last for a `MIN_DETECTION_DURATION_MS` (1 second).
 * **Outcome:** Glass surfaces may cause the LED to briefly turn green (responsive feedback), but no detection is logged unless both the duration and critical strength conditions are met.

* **Smart Noise Floor Clamp :**
 * **Problem:** In environments with an inherently high ambient baseline (e.g., direct IR reflection from a glass table), the standard 20% hysteresis applied to the `DETECTION_THRESHOLD` can push the calculated `releaseThreshold` *below* the actual noise floor. This prevents the signal from dropping low enough to register as "released," causing the device to become permanently "stuck" in `DETECTION_ACTIVE` (infinite green LED).
 * **Safeguard:** `handleDetection` actively clamps the `releaseThreshold` to ensure it never falls below the calibrated `baseDetectionThreshold` (when operating at normal/low sensitivity) or `wakeThreshold` (at high sensitivity). This guarantees the `releaseThreshold` is always reachable by the sensor.
 * **Outcome:** Prevents permanent "stuck detection" states on elevated baselines, ensuring detections always release correctly.

* **Gray Zone Watchdog :**
 * **Problem:** If the sensor signal, during an active `DETECTION_ACTIVE` state, becomes "stuck" within the hysteresis band (the "gray zone" between `releaseThreshold` and `currentDetectionThreshold`) for an extended period (e.g., > 5 seconds), it is physically impossible for this to be a human interaction. This indicates a static, spurious reflection.
 * **Safeguard:** `handleDetection` actively monitors the duration a signal spends in this gray zone. If the `5-second` threshold is exceeded, it sets `detectionState.requestBackoff` (an atomic flag).
 * **Outcome:** Triggers a *proactive*, early warning for stuck-detection conditions, leading to immediate `_environmentBackoff` and recalibration before the full `60-second` watchdog timeout is reached.

* **Jitter Prevention :**
 * **Problem:** If the *effective hysteresis gap* (the difference between `scaledDetectionThreshold` and `releaseThreshold` after `Smart Noise Floor Clamp` and sensitivity adjustments) becomes too small (e.g., `<50` units), even minor sensor noise can cause rapid, false detection/release cycles (jitter). This is a *configuration problem*, not a runtime anomaly.
 * **Safeguard:** `sensorTask` proactively monitors this `effective hysteresis gap`. If it falls below a `MINIMUM_HYSTERESIS_GAP` (`50` units), `Sensor_HAL::applyEnvironmentBackoff` is immediately invoked to temporarily raise the `DETECTION_THRESHOLD` and `CRITICAL_DETECTION_THRESHOLD` with an additive offset. This adjustment is ephemeral (RAM-only) and does not alter user sensitivity.
 * **Outcome:** Prevents jitter by adjusting thresholds before false detections occur, ensuring a minimum functional separation between `detection` and `release` points. A `30-second cooldown` prevents spamming this adjustment.

* **Stuck-Hit Watchdog (`MAX_DETECTION_DURATION_MS`):**
 * **Problem:** A severe software bug or sensor anomaly could cause the device to remain indefinitely in the `DETECTION_ACTIVE` state (e.g., 60+ seconds), leading to an infinite green LED and draining battery.
 * **Safeguard:** The `sensorTask` maintains a watchdog timer (`g_stuckHitWatchdogActive`). If the device remains in `DETECTION_ACTIVE` for `MAX_DETECTION_DURATION_MS` (60 seconds) OR if the `Gray Zone Watchdog` sets `detectionState.requestBackoff`, a comprehensive self-healing sequence is triggered.
 * **Outcome:** The self-healing sequence resets `DetectionState` flags, turns off the LED, applies an `_environmentBackoff` to `Sensor_HAL`, forces a full `calibrate`, persists new thresholds to RTC, clears the backoff, and transitions back to `ACTIVE`/`WAKE_MODE`. This breaks the infinite loop and restores stable operation.

* **Warmup Guard (`g_isWarmupPeriod`):**
 * **Problem:** Transient noise and power rail instability immediately after a cold boot can produce spurious sensor readings, leading to false detections.
 * **Safeguard:** The `sensorTask` skips full event *detection* (though it continues to sample and update rolling averages) for the first `1 second` after boot (`g_isWarmupPeriod`).
 * **Outcome:** Ensures a stable startup without false triggers during system initialization.

* **Wake-Time False-Hit Mitigation (, ):**
 * **Problem:** The sensor's edge-triggered interrupt (`PS_INT_CLOSE`) only fires when proximity *crosses* above the threshold. If an object is already close (or user taps) when the device enters deep sleep, no crossing event occurs on wake, or an event is lost mid-shutdown.
 * **Safeguard:** `PowerManager::enterDeepSleep` performs a final proactive check:
 1. It verifies `sensor->isProximityBelowThreshold(deepSleepThreshold)` after configuring the sensor for low-power sleep.
 2. It checks the raw `digitalRead(Pins::PROXIMITY_INT)` one last time before calling `esp_deep_sleep_start`.
 * **Outcome:** If proximity is elevated or a tap is detected, sleep is aborted. The device returns to `WAKE_MODE` or `DETECTION_ACTIVE` to properly handle the interaction, preventing missed events or being stuck in sleep.

* **Panic Governor (`ProtocolHandler::sendDetectionEvent`):**
 * **Problem:** An extremely high `_userSensitivity` or environmental anomaly can cause continuous, rapid sensor triggers. If all these `MSG_DETECTION_EVENT`s are sent over BLE, they can saturate the transmit buffer, starve the BLE receive path, and prevent critical `MSG_SET_CONFIG` (to adjust sensitivity) from reaching the device. This creates a "Self-DoS" scenario.
 * **Safeguard:** The `ProtocolHandler` implements a rate limiter (`Panic Governor`) for `MSG_DETECTION_EVENT`. It limits live detection events to `MAX_DETECTIONS_PER_WINDOW` (e.g., 5 detections) within a `DETECTION_RATE_WINDOW_MS` (1 second). Excess events are *dropped* (not queued). Additionally, if the governor triggers, `sensor->suppressDetections(1000)` is called for a 1-second cooldown at the sensor level.
 * **Outcome:** Preserves BLE receive bandwidth, prevents device unresponsiveness from excessive events, and provides a CPU cooldown period. Historical data (triggered by `MSG_SYNC_REQUEST`) is explicitly `bypassGovernor` to prevent loss.

## 8. ML Pipeline

The Machine Learning (ML) pipeline provides an advanced layer of on-device gesture classification, enabling the device to distinguish nuanced user interactions (e.g., approach, hover, quick pass) from simple detections, using an efficient TinyML approach.

```mermaid
graph TD
 subgraph Raw Data Acquisition
 A[Raw Proximity Samples (20Hz)] --> B(Sliding Sample Buffer)
 end

 subgraph Windowing and Pre-processing
 B --> C{Form Fixed-Size SampleWindow}
 C -- "40 samples, 2s duration, 75% overlap" --> D[SampleWindow Object]
 D -- "Includes WindowHeader (metadata)" --> E[DataLoggerService]
 end

 subgraph Feature Extraction (InferenceService::extractFeaturesNormalized)
 E --> F[Extract 10 Features]
 F -- "Features: Mean, StdDev, Min, Max, Peak-to-Peak, MeanDeriv, StdDeriv, DwellAbove, TimeToPeak, Energy" --> G[Feature Vector (Normalized)]
 style F fill:#F5F5DC,stroke:#C2B280,stroke-width:2px
 end

 subgraph Personalization Service (Adaptation Data)
 P[PersonalizationService] -- "Baseline (Mean, Sigma, Dynamic Range)" --> G
 P -- "Local Prototypes, Biases" --> H[Hybrid Classifier]
 style P fill:#F0F4C3,stroke:#C0CA33,stroke-width:2px
 end

 subgraph On-Device Inference (InferenceService::predictHybrid)
 G --> H
 H --> I[Inference Result]
 I -- "Predicted Label (GestureLabel), Confidence (0-100), Latency (us)" --> J{Confidence Thresholding}
 J -- "Result: GestureLabel::UNKNOWN if below threshold" --> K[Final Classification]
 style H fill:#FFEBEE,stroke:#E57373,stroke-width:2px
 style I fill:#E0E0E0,stroke:#9E9E9E,stroke-width:2px
 style J fill:#FFECB3,stroke:#FFC107,stroke-width:2px
 style K fill:#C8E6C9,stroke:#4CAF50,stroke-width:2px
 end

 subgraph Outputs & Logging
 K --> L[Telemetry / Debug Logs]
 E --> M[SPIFFS Storage (Raw Training Data)]
 M -- "Async LoggerTask" --> N[Offline ML Training / Debugging]
 end

 style A fill:#D0E0FF,stroke:#3672E9,stroke-width:2px
 style B fill:#E0F2F7,stroke:#26C6DA,stroke-width:2px
 style C fill:#E0F2F7,stroke:#26C6DA,stroke-width:2px
 style D fill:#E0F2F7,stroke:#26C6DA,stroke-width:2px
 style E fill:#E0F2F7,stroke:#26C6DA,stroke-width:2px
 style L fill:#B0BEC5,stroke:#78909C
 style M fill:#E1F5FE,stroke:#4FC3F7
 style N fill:#F0F4C3,stroke:#C0CA33

 linkStyle 0 stroke:#26C6DA,stroke-width:1.5px
 linkStyle 1 stroke:#26C6DA,stroke-width:1.5px
 linkStyle 2 stroke:#26C6DA,stroke-width:1.5px
 linkStyle 3 stroke:#26C6DA,stroke-width:1.5px
 linkStyle 4 stroke:#C2B280,stroke-width:1.5px
 linkStyle 5 stroke:#C0CA33,stroke-width:1.5px
 linkStyle 6 stroke:#E57373,stroke-width:1.5px
 linkStyle 7 stroke:#FFC107,stroke-width:1.5px
 linkStyle 8 stroke:#4CAF50,stroke-width:1.5px
 linkStyle 9 stroke:#78909C,stroke-width:1.5px
 linkStyle 10 stroke:#4FC3F7,stroke-width:1.5px
 linkStyle 11 stroke:#C0CA33,stroke-width:1.5px
```
**Figure D: ML Windowing and Inference Diagram**

**8.1. Window Formation**
The `DataLoggerService::appendSample` method continuously collects raw proximity readings from the `sensorTask` into a rolling buffer. Once this buffer contains `MLConfig::WINDOW_SIZE` (`40`) samples, a complete `SampleWindow` is formed. At a 20Hz sampling rate, this corresponds to a `2.0-second` temporal span of sensor data. The window then slides, advancing by `MLConfig::WINDOW_STRIDE` (`10`) samples (resulting in a `75%` overlap) to ensure continuous inference without missing events. Each `SampleWindow` is enriched with a `WindowHeader` containing vital runtime metadata (e.g., current thresholds, user sensitivity, BLE connection status) essential for contextualized model training and inference.

**8.2. Feature Extraction**
`InferenceService::extractFeaturesNormalized` takes a `SampleWindow` and computes 10 distinct statistical and temporal features from its raw sample array. These features are designed to capture the essence of different proximity gestures:
1. **Mean:** Average proximity value within the window.
2. **Standard Deviation:** Variability of the signal, indicating movement or stability.
3. **Minimum Value:** Lowest proximity recorded, often reflecting the ambient baseline.
4. **Maximum Value:** Peak proximity reached during the window.
5. **Peak-to-Peak:** Range between max and min, indicating signal dynamic range.
6. **Mean Derivative:** Average rate of change, indicating overall trend (approach/recede).
7. **Standard Deviation of Derivative:** Variability of the rate of change, indicating signal smoothness or jitter.
8. **Dwell Above Midpoint:** Fraction of samples above the window's midpoint, indicating sustained presence.
9. **Time to Peak:** Normalized time (index) within the window when the maximum proximity was reached.
10. **Energy:** Sum of squared deviations from the mean, representing signal power.

Crucially, these features are *normalized* using device-specific `baselineMean`, `baselineSigma`, and `dynamicRange` parameters retrieved from the `PersonalizationService`. This dynamic normalization (`PersonalizationConfig::DEFAULT_PROX_NORM` as fallback) allows the model to adapt to different sensor baselines and environmental characteristics (e.g., various glass types).

**8.3. Hybrid Classifier Structure**
The `InferenceService::predictHybrid` function employs a lightweight hybrid classifier to achieve robust gesture classification:
* **Global Model:** This is a core logistic regression model with pre-trained `_weights` and `_biases` (currently placeholders, `WEIGHTS_TRAINED = false`). It provides a general understanding of gesture patterns learned from a diverse, generic dataset.
* **Local Prototypes:** `PersonalizationService` supplies device-specific prototypes, which are feature centroids (mean feature vectors) for each `GestureLabel` (e.g., for `APPROACH`, `HOVER`). These prototypes are learned on-device from user-provided examples.
* **Local Biases:** `PersonalizationService` also provides per-class additive `biasOffsets` that can refine the global model's predictions based on device-specific adaptations.
The hybrid classifier fuses the scores from the global model, the local prototypes (based on similarity/distance), and the local biases using configurable weights (`PersonalizationConfig::GLOBAL_WEIGHT`, `LOCAL_WEIGHT`, `BIAS_WEIGHT`). This fusion method allows the model to leverage both generalized knowledge and individualized adaptations.

**8.4. Inference Runtime**
The ML inference process runs on a dedicated FreeRTOS `InferenceTask` (Core 1, priority 3), isolating it from higher-priority `sensorTask` and `ProtocolTask` operations. This task is triggered by `DataLoggerService` via `xTaskNotifyGive` whenever a new `SampleWindow` is fully formed and ready for processing. The `InferenceTask` then computes features, performs `predictHybrid`, and generates an `InferenceResult`.

**8.5. Output Interpretation and Confidence**
The `InferenceResult` contains the `predictedLabel` (a `GestureLabel` such as `IDLE`, `APPROACH`, `HOVER`, `QUICK_PASS`, etc.), a confidence score (0-100%), and the `latencyUs` (time taken for inference). A configurable `PersonalizationConfig::CONFIDENCE_THRESHOLD` determines if a prediction is sufficiently strong; if confidence falls below this threshold, the `predictedLabel` defaults to `GestureLabel::UNKNOWN`. This prevents unreliable classifications from being used.

**8.6. Model Status**
It is important to note that the current firmware implementation for `InferenceService` uses placeholder model weights (`_weights`, `_biases` in `InferenceService.cpp` are marked with `WEIGHTS_TRAINED = false`). While the ML pipeline is fully architecturally defined and integrated, its actual intelligent classification capabilities are dependent on real-world training data being collected (via `DataLoggerService`), a model being trained offline, and the resulting weights being incorporated into the firmware. Until then, the model's predictions will be effectively random.

## 9. Personalization and Adaptation

Personalization is a critical layer that enables thedevice to adapt its machine learning model to individual devices and diverse user environments. This process overcomes inherent sensor variability and environmental context, ensuring consistent and accurate gesture classification.

**9.1. Why Personalization Exists**
Generic ML models, even when well-trained, often struggle with real-world deployment due to:
* **Sensor Variance:** Slight manufacturing differences between individual Proximity Sensor units.
* **Mounting Differences:** The exact way the sensor is positioned within the device housing can alter its optical field.
* **Environmental Context:** The primary surfaces a device interacts with (e.g., a specific glass tabletop vs. an opaque surface) significantly change reflection patterns.
Personalization tailors the ML model to these specific device and environmental characteristics, allowing it to "learn" the unique signature of gestures in its actual operating context, leading to improved accuracy and robustness.

**9.2. Personalization Data (`MLCalibrationData`)**
The `PersonalizationService` manages the `MLCalibrationData` struct (`PersonalizationService.h`), which is persistently stored in NVS. This `CRC32`-protected and versioned data blob is the heart of device-specific adaptation and includes:
* **Sensor Baseline Parameters:** `baselineMean`, `baselineSigma`, and `dynamicRange`. These are derived directly from `Sensor_HAL`'s most recent `calibrate` operation. They are crucial for normalizing raw proximity features, ensuring that feature values fed into the ML model are relative to the device's current environmental baseline.
* **Per-Class Prototypes:** These are feature centroids—mean feature vectors for each `GestureLabel` (e.g., `IDLE`, `APPROACH`, `HOVER`). They are learned on-device by accumulating multiple user-provided examples of each gesture class.
* **Per-Class Bias Offsets:** These are additive adjustments (`biasOffsets`) to the global model's logits for each `GestureLabel`. They allow the local adaptation to fine-tune the likelihood of predicting a specific class based on device-specific tendencies.

**9.3. Calibration Capture Flow**
Users (or diagnostic tools) can initiate a guided calibration process for personalization:
1. **`startCalibration`:** Resets internal accumulators within `PersonalizationService` for a new calibration session.
2. **`captureWindow(features, label)`:** The user provides examples of specific gestures (e.g., "perform a quick pass gesture"). For each `SampleWindow` captured by `DataLoggerService`, its extracted `features` are provided to this method along with the user-assigned `label` (e.g., `GestureLabel::QUICK_PASS`). `PersonalizationService` accumulates these feature vectors, computing a running sum and count for each class. Limits (`MIN_CALIBRATION_WINDOWS`, `MAX_CALIBRATION_WINDOWS`) are enforced to ensure sufficient, but not excessive, training data.
3. **`commitCalibration`:** Once enough samples are captured for a specific class, this method is called. It computes the centroid (mean) of the accumulated feature vectors for that `GestureLabel` and updates the corresponding `prototypes` in `MLCalibrationData`. It also increments `numCalibratedClasses`.

**9.4. Persistence and Adaptation Guardrails**
* **Asynchronous Persistence:** `MLCalibrationData` changes (e.g., after `commitCalibration` or `updateSensorBaseline`) are saved to NVS asynchronously by a low-priority `PersonalizationWorker` FreeRTOS task (Core 1, priority 1). A `COMMIT_DELAY_MS` (`10` seconds) debounce timer prevents excessive flash wear during rapid updates. This design ensures that UI responsiveness and critical real-time tasks are not blocked by slower NVS write operations.
* **Runtime Application:** `InferenceService::predictHybrid` actively retrieves an immutable snapshot (`_snapshot`) of `MLCalibrationData` from `PersonalizationService`. It then fuses the global model's predictions with these device-specific `prototypes` and `biasOffsets` using configurable weights (`GLOBAL_WEIGHT`, `LOCAL_WEIGHT`, `BIAS_WEIGHT`).
* **Adaptation Quality:** The `getAdaptationQuality` method reports an overall measure (0-100%) of how thoroughly the device has been personalized, based on the total number of captured samples across all classes.
* **Dynamic Baseline Update:** Whenever `Sensor_HAL::calibrate` completes (either user-initiated or triggered by self-healing logic like the `Stuck-Hit Watchdog`), `PersonalizationService::updateSensorBaseline` is invoked. This ensures that the ML pipeline's feature normalization constantly uses the most up-to-date `mean`, `sigma`, and `dynamicRange` of the sensor's operating environment.

## 10. Runtime and Resource Constraints

The sensing, calibration, and ML pipeline is meticulously designed to operate robustly and efficiently within the tight constraints of the ESP32-S3 microcontroller, balancing performance with resource utilization.

**10.1. ESP32-S3 Microcontroller Constraints**
The ESP32-S3, while powerful, is an embedded device with finite resources:
* **Dual-Core CPU:** Two Xtensa LX7 cores (Core 0 for Wi-Fi/BLE/RTOS; Core 1 for application tasks). Proper task pinning and synchronization are critical.
* **Limited SRAM:** ~512KB SRAM. Large dynamic allocations can lead to fragmentation or crashes.
* **Flash Memory:** 8MB (dual-bank OTA). NVS and SPIFFS are flash-based, meaning write operations are slow and have wear-leveling concerns.
* **Power Budget:** Battery-operated, requiring aggressive power management (deep sleep, modem sleep) and careful management of peripherals.

**10.2. Real-time Processing (20Hz Cadence)**
The `sensorTask` (`main.cpp`) is the highest-priority application task, running on Core 1 at a precise 20Hz cadence (`Config.h::Timing::SAMPLING_PERIOD_MS = 50ms`). The use of `vTaskDelayUntil` ensures deterministic execution, where each iteration starts at its scheduled time regardless of previous task overruns, critical for consistent data acquisition and accurate temporal feature extraction.

**10.3. Concurrency and Task Separation (FreeRTOS)**
The pipeline leverages FreeRTOS's multi-tasking capabilities to maintain responsiveness and isolate blocking operations:
* **`sensorTask` (Core 1, Prio 10):** Dedicated to high-frequency sensor polling and immediate heuristic processing (`handleDetection`).
* **`ProtocolTask` (main loop, Core 1, Prio 5):** Handles BLE communication, message dispatch, and main application logic.
* **Asynchronous Workers (Core 1, Prio 1-3):**
 * `StorageWorker` (`HistoryService`): Performs slow NVS writes for event persistence.
 * `LoggerTask` (`DataLoggerService`): Writes `SampleWindow` data to SPIFFS.
 * `InferenceTask` (`InferenceService`): Executes feature extraction and ML classification.
 * `PersonalizationWorker` (`PersonalizationService`): Persists `MLCalibrationData` to NVS.
This task separation ensures that time-critical operations (like sensing and BLE communication on Core 0) are never blocked by slower I/O (flash writes) or computationally intensive ML tasks.

**10.4. Non-blocking Design**
* **Asynchronous I/O:** All flash-based storage operations (NVS, SPIFFS) are offloaded to dedicated low-priority FreeRTOS tasks that communicate via `xQueueSend` (`DataLoggerService`) or `xTaskNotifyGive` (`InferenceService`, `PersonalizationService`). This is critical for preventing CPU stalls that can starve the BLE stack and trigger connection timeouts.
* **Thread Safety:** Shared mutable state across tasks (e.g., `_lastReading` in `Sensor_HAL`, internal `DetectionState` flags) is protected using `std::atomic` for fine-grained, lock-free access, or `SemaphoreHandle_t` (mutexes) for larger critical sections. This avoids disabling interrupts (spinlocks) that would impact BLE performance.

**10.5. Memory and Storage Considerations**
* **Bounded Stack Sizes:** Each FreeRTOS task is allocated a fixed stack size (e.g., `SENSOR_TASK_STACK_SIZE=4KB`, `INFERENCE_TASK_STACK_SIZE=4KB`, `STORAGE_TASK_STACK_SIZE=16KB`). These are rigorously monitored and set to prevent stack overflows, especially during stack-hungry NVS operations (, ).
* **Fixed-Size Buffers:** `SampleWindow` structs are fixed-size (`192` bytes at `MLConfig::WINDOW_SIZE=40`). `HistoryService`'s ring buffer (50 `DetectionEvent`s = `1200` bytes) uses `RTC_DATA_ATTR` memory, minimizing dynamic allocation and preventing fragmentation.
* **Tiered Persistence:**
 * **RTC Memory (`RTC_DATA_ATTR`):** For critical, frequently accessed data that must survive deep sleep (e.g., `HistoryService` event buffer, `CalibrationData`).
 * **NVS Flash:** For non-volatile settings (`ConfigService`), long-term event backups (`HistoryService`), and `MLCalibrationData` (`PersonalizationService`). These writes are debounced (`COMMIT_DELAY_MS`) to mitigate flash wear.
 * **SPIFFS Flash:** For larger, raw `SampleWindow` data captured by `DataLoggerService`, primarily for offline ML training.

**10.6. Latency Expectations**
* **Sensor Polling:** 50ms (`20Hz`).
* **Heuristic Event Detection:** Near real-time, within a single 50ms `sensorTask` iteration.
* **ML Feature Extraction & Inference:** `InferenceService` reports `latencyUs` typically under `100us` per `SampleWindow`, ensuring classification results are available with minimal delay.
* **Asynchronous I/O:** Blocking flash operations (10-100ms) are handled in background tasks, ensuring they do not impact the real-time responsiveness of the sensing or BLE pipelines.

**10.7. Power Optimization**
Deep sleep (`PowerManager`) is central to the device's battery life. The sensing pipeline supports this by:
* Configuring `Sensor_HAL` for ultra-low power, interrupt-driven wake-up (`Sensor_HAL::configureForDeepSleep`).
* Implementing failsafe timer wakeups (`EMERGENCY_WAKE_INTERVAL_US`) to prevent indefinite sleep if the sensor fails.
* Intelligently shutting down power-hungry peripherals (e.g., Wi-Fi, LEDs) before sleep (`PowerManager::enterDeepSleep`).

## 11. Failure Modes and Safeguards

The sensing pipeline is designed with numerous safeguards and self-healing mechanisms to ensure reliability and robustness against common failure modes in real-world operating environments.

| Issue/Scenario | Symptom | Safeguard/Mechanism | Outcome/Recovery Behavior |
| :------------------------------ | :----------------------------------------------------- | :------------------------------------------------------------------ | :------------------------------------------------------------------- |
| **Environmental Reflections** | False "detections" from glass tables or static objects. | Multi-Level Thresholds (`DETECTION`, `CRITICAL`), `MIN_DETECTION_DURATION_MS`. | Green LED, but detection not logged. |
| **Elevated Baselines** | Sensor readings consistently high (e.g., glass), `releaseThreshold` falls below baseline, device stuck in `DETECTION_ACTIVE`. | `Smart Noise Floor Clamp` (clamps `releaseThreshold` to `baseDetectionThreshold`). | Prevents permanent stuck detections; ensures signal can always drop to "release" state. |
| **Sensor Jitter/Noise** | Rapid detection/release cycles around thresholds due to signal noise. | Hysteresis (`MAX_HYSTERESIS_GAP`), `Jitter Prevention` (min gap enforcement). | `Sensor_HAL::applyEnvironmentBackoff` proactively raises thresholds to restore sufficient gap. |
| **Stuck Sensor/Software (Hit)** | Device remains indefinitely in `DETECTION_ACTIVE` state. | `Gray Zone Watchdog` (5s timeout in hysteresis band), `MAX_DETECTION_DURATION_MS` (60s total timeout). | Triggers `_environmentBackoff` + `Sensor_HAL::calibrate` (self-healing), resetting state. |
| **Bad Calibration Data** | Calibration yields absurdly high/low thresholds (e.g., sensor pointed at reflective surface). | `isThresholdSane` validation (min/max bounds on `wakeThreshold`). | Rejects bad calibration results, applies safe default thresholds. |
| **RTC Memory Corruption** | `RTC_DATA_ATTR` (event buffer, calibration) checksum fails on boot. | `CRC32` checksum in `RTCState`, NVS backup (`HistoryService`). | Data recovered from NVS backup, or reset to defaults. |
| **NVS Write Interruption** | Power loss during NVS write; `nvs_flash_init` hangs. | `g_crash_counter` in `main.cpp`, `performCrashRecoveryCheck`, NVS backup (`HistoryService`). | Repeated crash-resets (3x) trigger emergency NVS erase, then device self-heals. |
| **CPU/BLE Starvation** | Flash writes halt CPU for 10-100ms, starving BLE stack. | Asynchronous `StorageWorker`, mutex-protected data copy, tiered `vTaskDelay` yields. | Stable BLE communication, deferred I/O, prevents disconnects. |
| **Memory Exhaustion** | Stack overflows (`Guru Meditation Error`). | Bounded stack sizes (`STORAGE_TASK_STACK_SIZE=16KB`, `PROTOCOL_TASK_STACK_SIZE=16KB`). | Prevents system crashes, ensures stability during heavy operations. |
| **Phantom Wakeups** | Sensor triggers wake from deep sleep due to noise/vibration. | `_wokeFromSensor` flag, shorter `WAKE_MODE` timeouts (30-45s). | Device returns to deep sleep quickly if no valid activity/connection. |
| **Elevated Proximity on Sleep** | Device sleeps with object near sensor; edge-triggered interrupt won't fire on wake. | `Sensor_HAL::isProximityBelowThreshold` check before `esp_deep_sleep_start`. | Aborts sleep, returns to `WAKE_MODE`, retries when proximity drops. |
| **Rapid Tap During Sleep Prep** | User taps sensor during final sleep shutdown window. | `digitalRead(Pins::PROXIMITY_INT)` check before `esp_deep_sleep_start`. | Aborts sleep, returns to `DETECTION_ACTIVE` to process the valid detection. |
| **Config "Split Brain"** | App/Device disagree on current sensitivity value. | `Sensor_HAL` is source of truth; `handleDetection` fetches real-time `Sensor_HAL` thresholds. | Consistent sensing behavior; UI reflects actual device state. |
| **NVS Write During Pairing** | NVS corruption due to concurrent bonding key writes by NimBLE. | `HistoryService::setBleConnected` defers non-critical NVS flush if `_bleManager->isPairingModeEnabled`. | Prevents NVS corruption during sensitive BLE operations. |
| **ML Inference Backlog** | `InferenceTask` struggles to keep up with `SampleWindow` rate. | `MLConfig::LOGGER_QUEUE_DEPTH` (4 items) is shallow, older `SampleWindow`s are dropped. | Inference prioritizes fresh data; older data may be skipped for classification (raw data still logged to SPIFFS for offline analysis). |
| **Protocol "Self-DoS"** | Excessive detections saturate BLE TX, starving RX. | `Panic Governor` in `ProtocolHandler::sendDetectionEvent` (rate limits `MSG_DETECTION_EVENT`). | Drops excess detections, calls `sensor->suppressDetections(1000)` to cool down sensor. |

## 12. Tradeoffs and Design Rationale

The sensing, calibration, and ML pipeline embodies deliberate engineering tradeoffs, balancing performance, reliability, and intelligence within embedded constraints.

* **Adaptive Heuristics First, ML Second:** The decision to build a robust foundation of adaptive thresholds and deterministic heuristic safeguards (`handleDetection`) before integrating ML is crucial. Heuristics provide immediate, guaranteed responsiveness and form a fail-safe layer, ensuring core "detection" detection remains reliable even if the ML model is disabled, untuned, or fails. ML then enhances this foundation with nuanced gesture classification. This avoids over-reliance on ML for critical, real-time control.
* **On-Device TinyML:** Implementing the ML pipeline entirely on the ESP32-S3 prioritizes low-latency gesture classification, ensures offline functionality, and significantly enhances user privacy by keeping raw sensor data local. This comes at the cost of using simpler models (e.g., linear classifiers) compared to cloud-based solutions, but it is optimized for the embedded environment.
* **Bounded Personalization:** While `PersonalizationService` enables device-specific adaptation for the ML model, this adaptation is deliberately bounded (`MAX_CALIBRATION_WINDOWS` for sample collection). This prevents uncontrolled model drift or over-fitting to noisy individual device data, ensuring long-term model stability and preventing erratic behavior. The core `Sensor_HAL` calibration (which sets the baseline parameters for ML normalization) always remains the foundation.
* **Power vs. Responsiveness:** Aggressive power management, including deep sleep (with tiered failsafe wakeups) and active Wi-Fi shutdown, directly impacts battery life. This design prioritizes extended battery life over constant, full-power operation. Responsiveness for core user interactions is maintained by immediate sensor polling on wake, while non-critical background tasks (like Wi-Fi for NTP sync) are carefully metered.
* **Asynchronous Processing:** The multi-tasking FreeRTOS architecture, coupled with asynchronous I/O to flash storage (NVS, SPIFFS), is a critical tradeoff. It introduces complexity but is necessary to prevent blocking the real-time `sensorTask` and the `BLE stack` (running on a separate core), which are highly sensitive to CPU stalls. This ensures stable BLE connectivity and consistent sensor timing.
* **Deterministic Calibration:** The extensive multi-step calibration process (`Sensor_HAL::calibrate`) with sanity checks (`isThresholdSane`) is a deliberate design choice. It sacrifices a few seconds of immediate boot time on cold starts to achieve a highly accurate, robust, and validated sensor baseline, preventing unreliable detection and "bricked" devices.

## 13. Validation Strategy

The reliability and correctness of the sensing, calibration, and ML pipeline are validated through a multi-faceted strategy encompassing various testing and analytical approaches.

* **Unit and Integration Testing:**
 * Low-level components such as CRC calculation (`ProtocolDefs.h`), binary payload parsing (`BinaryProtocol.ts`), and threshold derivation logic (`Sensor_HAL::recalculateThresholdsFromSensitivity`) are rigorously unit-tested.
 * Integration tests verify the core `sensorTask` -> `handleDetection` flow for expected state transitions and event logging.

* **Hardware-in-the-Loop (HIL) Testing:**
 * Physical prototypes are subjected to controlled, repeatable interaction scenarios using robotic arms or jigs (e.g., precise object approach/removal at varying speeds, sustained hovering).
 * Simulated environmental disturbances, such as vibrations or placing the device on different reflective surfaces (e.g., various types of glass), are used to verify robustness.

* **Live Field Testing:**
 * Devices are deployed with users in diverse real-world environments to uncover unforeseen edge cases, environmental interferences, and natural user interaction patterns that may not be reproducible in a lab.
 * This provides critical feedback for refining heuristics and collecting data for ML model improvements.

* **Comprehensive Logging:**
 * The firmware generates verbose debug logs (`ESP_LOG` at `CORE_DEBUG_LEVEL=5` in `platformio.ini`) detailing raw sensor readings, processed proximity values, state transitions, threshold calculations, ML inference results, and error conditions.
 * The mobile application utilizes an `OtaFileLogger` to capture all OTA-related operations to a persistent text file (`ota_debug_log.txt`), allowing offline analysis of critical update flows.

* **Offline Log and Data Review:**
 * Captured `SampleWindow` data from `DataLoggerService` (stored on SPIFFS) is extracted and analyzed offline using Python scripts. This data is critical for validating feature extraction accuracy and evaluating ML model performance against real-world inputs.
 * Detailed `ESP_LOG` traces are parsed to reconstruct runtime behavior, diagnose timing issues, and verify event sequences.

* **Calibration Verification:**
 * Manual checks confirm that `Sensor_HAL::calibrate` produces reasonable `mean` and `stdDev` values for the noise floor, and that derived `wake`, `detection`, and `critical` thresholds are appropriate for the environment.
 * The `isThresholdSane` function provides an automated sanity check, rejecting obviously erroneous calibration data to prevent misconfiguration.

* **Inference Sanity Checks:**
 * For known physical gestures (e.g., a hand quickly swiped across the sensor), the `InferenceService` is expected to produce the correct `GestureLabel` with high confidence.
 * The reported `latencyUs` for inference is monitored to ensure real-time performance.

* **Edge-Case Validation:**
 * Specific test cases are designed to intentionally trigger and verify the behavior of documented safeguards, such as:
 * Simulating a "stuck detection" condition (e.g., holding an object in the "gray zone").
 * Introducing noise to trigger `Jitter Prevention`.
 * Rapidly power cycling to verify the NVS crash recovery.
 * Simulating a disconnect during NVS write to verify asynchronous handling.

* **Watchdog Monitoring:**
 * The ESP32's Task Watchdog Timer (TWDT) is enabled (`CONFIG_ESP_TASK_WDT=1` in `platformio.ini`). All critical FreeRTOS tasks (e.g., `sensorTask`, `StorageWorker`) periodically call `esp_task_wdt_reset` to confirm responsiveness. This prevents CPU starvation and ensures system stability.

## 14. Key Takeaways

The the sensing, calibration, and machine learning subsystem represents a robust, adaptive, and intelligent embedded solution, designed with production-grade reliability and resource efficiency as core tenets:

* The firmware employs a sophisticated, multi-layered approach that transforms noisy physical sensor data into trustworthy user interaction classifications, balancing responsiveness with accuracy.
* Adaptive heuristics and a multi-tiered threshold system form a resilient foundation, proactively managing environmental noise, reflections, and sensor anomalies through self-healing mechanisms like the `Smart Noise Floor Clamp` and `Gray Zone Watchdog`.
* An on-device TinyML pipeline, powered by contextualized feature extraction and a hybrid classifier, provides nuanced gesture recognition capabilities, enhanced by adaptive personalization for consistent performance across individual devices and diverse environments.
* The entire pipeline is meticulously engineered for the ESP32-S3's constraints, leveraging FreeRTOS task separation, asynchronous I/O, and non-blocking design to ensure real-time performance, stable BLE communication, and efficient resource utilization.
* Extensive safeguards, including `CRC32` data integrity checks, NVS-backed persistence, and `Panic Governor` rate limiting, collectively ensure high reliability, prevent data loss, and enable graceful recovery from potential failure modes.

---
