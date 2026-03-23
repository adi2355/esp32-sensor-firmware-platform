
#include "Sensor_HAL.h"

static const char* TAG = "SENSOR_HAL";

#define Proximity Sensor_REG_PS_CONF1 0x03
#define Proximity Sensor_REG_PS_CONF2 0x03
#define Proximity Sensor_REG_PS_CONF3 0x04
#define Proximity Sensor_REG_PS_THDL 0x06
#define Proximity Sensor_REG_PS_THDH 0x07
#define Proximity Sensor_REG_PS_CANC 0x05
#define Proximity Sensor_REG_PS_DATA 0x08
#define Proximity Sensor_REG_INT_FLAG 0x0D
#define Proximity Sensor_REG_ID 0x0E


#define PS_SD_EN 0x0000
#define PS_SD_DIS 0x0001

#define PS_INT_CLOSE 0x0002
#define PS_INT_AWAY 0x0004
#define PS_INT_BOTH 0x0006

#define PS_AF_AUTO 0x0000
#define PS_AF_ACTIVE_FORCE 0x0008

#define PS_PERS_1 0x0000
#define PS_PERS_2 0x0010
#define PS_PERS_3 0x0020
#define PS_PERS_4 0x0030

#define PS_DUTY_1_40 0x0000
#define PS_DUTY_1_80 0x0040
#define PS_DUTY_1_160 0x0080
#define PS_DUTY_1_320 0x00C0

#define PS_HD_12BIT 0x0000
#define PS_HD_16BIT 0x0100

#define PS_IT_1T 0x0000
#define PS_IT_1_5T 0x0200
#define PS_IT_2T 0x0400
#define PS_IT_2_5T 0x0600
#define PS_IT_3T 0x0800
#define PS_IT_3_5T 0x0A00
#define PS_IT_4T 0x0C00
#define PS_IT_8T 0x0E00

#define PS_SC_EN 0x0001
#define PS_TRIG 0x0004
#define PS_SMART_PERS 0x0010

#define PS_LED_I_50MA 0x0000
#define PS_LED_I_75MA 0x0100
#define PS_LED_I_100MA 0x0200
#define PS_LED_I_120MA 0x0300
#define PS_LED_I_140MA 0x0400
#define PS_LED_I_160MA 0x0500
#define PS_LED_I_180MA 0x0600
#define PS_LED_I_200MA 0x0700

#define PS_NS_1STEP 0x0000
#define PS_NS_2STEP 0x1000

Sensor_HAL* Sensor_HAL::_instance = nullptr;

void IRAM_ATTR sensorInterruptHandler {
 Sensor_HAL* sensor = Sensor_HAL::_instance;
 if (sensor != nullptr) {
 sensor->_interruptTriggered.store(true, std::memory_order_relaxed);
 }
}

Sensor_HAL::Sensor_HAL
 : _initialized(false)
 , _powerOn(false)
 , _i2cInitialized(false)
 , _wakeThreshold(ProximityConfig::WAKE_THRESHOLD_DEFAULT)
 , _detectionThreshold(ProximityConfig::DETECTION_THRESHOLD_DEFAULT)
 , _criticalDetectionThreshold(ProximityConfig::CRITICAL_DETECTION_THRESHOLD_DEFAULT)
 , _baseWakeThreshold(ProximityConfig::WAKE_THRESHOLD_DEFAULT)
 , _baseDetectionThreshold(ProximityConfig::DETECTION_THRESHOLD_DEFAULT)
 , _baseCriticalThreshold(ProximityConfig::CRITICAL_DETECTION_THRESHOLD_DEFAULT)
 , _lastCalMean(0.0f)
 , _lastCalSigma(0.0f)
 , _lastReading(0)
 , _lastReadTime(0)
 , _interruptTriggered(false)
 , _lastInterruptTime(0)
 , _hitsSuppressed(false)
{
}

HalError Sensor_HAL::init {
 if (_initialized) {
 return HalError::OK;
 }

 HAL_LOG_I(TAG, "Initializing Sensor HAL on I2C (SDA=%d, SCL=%d)",
 Pins::I2C_SDA, Pins::I2C_SCL);

 if (!ensureI2CReady) {
 HAL_LOG_E(TAG, "Failed to initialize I2C bus!");
 return HalError::INIT_FAILED;
 }

 if (!isPresent) {
 HAL_LOG_E(TAG, "Proximity Sensor not found on I2C bus!");
 return HalError::DEVICE_NOT_FOUND;
 }

 uint16_t id = getSensorId;
 HAL_LOG_I(TAG, "Proximity Sensor ID: 0x%04X", id);

 initializeRegisters;

 pinMode(Pins::PROXIMITY_INT, INPUT_PULLUP);

 _initialized = true;
 _powerOn = true;

 HAL_LOG_I(TAG, "Sensor HAL initialized successfully");
 return HalError::OK;
}

uint16_t Sensor_HAL::readProximity {
 if (!_initialized) return 0;
 if (!_powerOn) return 0;
 if (!_i2cInitialized) return 0;

 uint16_t conf3 = PS_SC_EN | PS_TRIG | PS_SMART_PERS | PS_LED_I_200MA | PS_NS_2STEP;

 if (!writeRegister(Proximity Sensor_REG_PS_CONF3, conf3)) {
 HAL_LOG_E(TAG, "Failed to trigger proximity measurement");
 return _lastReading;
 }

 delay(4);

 uint16_t value = 0;
 if (!readRegister(Proximity Sensor_REG_PS_DATA, &value)) {
 HAL_LOG_E(TAG, "Failed to read proximity data");
 return _lastReading.load(std::memory_order_relaxed);
 }

 _lastReading.store(value, std::memory_order_relaxed);
 _lastReadTime.store(millis, std::memory_order_relaxed);

 return value;
}

HalError Sensor_HAL::setWakeThreshold(uint16_t threshold) {
 if (!_initialized) return HalError::NOT_INITIALIZED;
 _wakeThreshold = threshold;
 HAL_LOG_D(TAG, "Wake threshold set to %u", threshold);
 return HalError::OK;
}

HalError Sensor_HAL::setDetectionThreshold(uint16_t threshold) {
 if (!_initialized) return HalError::NOT_INITIALIZED;
 _detectionThreshold.store(threshold, std::memory_order_relaxed);
 HAL_LOG_D(TAG, "Hit threshold set to %u", threshold);
 return HalError::OK;
}

HalError Sensor_HAL::setCriticalDetectionThreshold(uint16_t threshold) {
 if (!_initialized) return HalError::NOT_INITIALIZED;
 _criticalDetectionThreshold.store(threshold, std::memory_order_relaxed);
 HAL_LOG_D(TAG, "Critical detection threshold set to %u", threshold);
 return HalError::OK;
}

void Sensor_HAL::configureForWakeMode {
 if (!_initialized) return;
 HAL_LOG_I(TAG, "Configuring sensor for wake mode");
 writeRegister(Proximity Sensor_REG_PS_THDH, _wakeThreshold);
 writeRegister(Proximity Sensor_REG_PS_THDL, 0);
}

void Sensor_HAL::configureForHitDetection {
 if (!_initialized) return;
 HAL_LOG_I(TAG, "Configuring sensor for event detection");
 writeRegister(Proximity Sensor_REG_PS_THDH, _detectionThreshold.load(std::memory_order_relaxed));
 writeRegister(Proximity Sensor_REG_PS_THDL, 0);
}


bool Sensor_HAL::configureForDeepSleep {
 if (!_initialized) {
 HAL_LOG_E(TAG, "Cannot configure - sensor not initialized");
 return false;
 }

 HAL_LOG_I(TAG, "+ Configuring Low Power Sleep");


 // Computes optimal wake sensitivity from calibrated thresholds
 // with bounded clamping for safety.

 uint16_t deepSleepThreshold = 0;
// THRESHOLD FORMULA 

 bool wasCapped = false;
 const char* capReason = nullptr;

 if (deepSleepThreshold < ProximityConfig::MIN_DEEP_SLEEP_THRESHOLD) {
 deepSleepThreshold = ProximityConfig::MIN_DEEP_SLEEP_THRESHOLD;
 wasCapped = true;
 capReason = "MIN_FLOOR";
 } else if (deepSleepThreshold > ProximityConfig::MAX_DEEP_SLEEP_THRESHOLD) {
 deepSleepThreshold = ProximityConfig::MAX_DEEP_SLEEP_THRESHOLD;
 wasCapped = true;
 capReason = "MAX_CAP";
 }

 for (int attempt = 1; attempt <= 3; attempt++) {
 HAL_LOG_D(TAG, "Configuration attempt %d/3", attempt);

 uint16_t currentConf1 = 0;
 if (!readRegister(Proximity Sensor_REG_PS_CONF1, &currentConf1)) {
 HAL_LOG_W(TAG, "Attempt %d: Failed to read CONF1 (I2C error)", attempt);
 recoverI2CBus;
 delay(10);
 continue;
 }

 if (!writeRegister(Proximity Sensor_REG_PS_CONF1, currentConf1 | PS_SD_DIS)) {
 HAL_LOG_W(TAG, "Attempt %d: Failed to write shutdown command", attempt);
 recoverI2CBus;
 delay(10);
 continue;
 }
 delay(5);

 if (!writeRegister(Proximity Sensor_REG_PS_THDH, deepSleepThreshold)) {
 HAL_LOG_W(TAG, "Attempt %d: Failed to write THDH", attempt);
 continue;
 }
 if (!writeRegister(Proximity Sensor_REG_PS_THDL, 0)) {
 HAL_LOG_W(TAG, "Attempt %d: Failed to write THDL", attempt);
 continue;
 }

 uint16_t conf3 = PS_SC_EN | PS_LED_I_120MA | PS_NS_2STEP;
 if (!writeRegister(Proximity Sensor_REG_PS_CONF3, conf3)) {
 HAL_LOG_W(TAG, "Attempt %d: Failed to write CONF3", attempt);
 continue;
 }

 uint16_t conf1 = PS_SD_EN | PS_AF_AUTO | PS_INT_CLOSE | PS_PERS_1 |
 PS_DUTY_1_320 | PS_IT_8T | PS_HD_16BIT;

 if (!writeRegister(Proximity Sensor_REG_PS_CONF1, conf1)) {
 HAL_LOG_W(TAG, "Attempt %d: Failed to write CONF1", attempt);
 continue;
 }
 delay(5);

 uint16_t verifyConf1 = 0;
 if (!readRegister(Proximity Sensor_REG_PS_CONF1, &verifyConf1)) {
 HAL_LOG_W(TAG, "Attempt %d: Failed to read back CONF1 for verification", attempt);
 recoverI2CBus;
 delay(10);
 continue;
 }

 bool isPoweredOn = (verifyConf1 & PS_SD_DIS) == 0;
 bool isIntEnabled = (verifyConf1 & PS_INT_CLOSE) != 0;
 bool isAutoMode = (verifyConf1 & PS_AF_ACTIVE_FORCE) == 0;

 if (isPoweredOn && isIntEnabled && isAutoMode) {
 HAL_LOG_I(TAG, "Max Sensitivity Mode VERIFIED");

 clearInterrupt;

 return true;
 } else {
 HAL_LOG_E(TAG, "Attempt %d: Verification FAILED!", attempt);
 }

 recoverI2CBus;
 delay(20);
 }

 HAL_LOG_E(TAG, "CRITICAL FAILURE");
 HAL_LOG_E(TAG, " Sensor configuration failed after 3 attempts!");

 return false;
}

//Multi-level statistical calibration algorithm
// Performs dark-environment baseline measurement and computes
// wake / detection / critical thresholds using statistical methods

HalError Sensor_HAL::calibrate(uint32_t durationMs, uint16_t* outWakeThreshold,
 uint16_t* outDetectionThreshold, uint16_t* outCriticalThreshold) {
 if (!_initialized) return HalError::NOT_INITIALIZED;

 HAL_LOG_I(TAG, "Starting calibration for %lu ms with Multi-Level Thresholding", durationMs);


 //   1. Sensitivity normalization (temporarily reset to neutral)
 //   2. Baseline cancellation register measurement
 //   3. Post-cancellation noise floor sampling
 //   4. Statistical analysis (mean, variance, standard deviation)
 //   5. Multi-level threshold computation using Z-score multipliers
 //   6. Minimum offset enforcement per threshold level
 //   7. Threshold bounds clamping to safe ranges
 //   8. Base threshold storage for sensitivity recalculation
 //   9. User sensitivity restoration and final threshold output
 //
 // Outputs: wake, detection, and critical thresholds
 // Side effects: updates base thresholds, calibration statistics

 return HalError::OK;
}

uint16_t Sensor_HAL::getSensorId {
 if (!_i2cInitialized && !ensureI2CReady) return 0;
 uint16_t id = 0;
 readRegister(Proximity Sensor_REG_ID, &id);
 return id;
}

void Sensor_HAL::recoverI2CBus {
 pinMode(Pins::I2C_SDA, INPUT_PULLUP);
 pinMode(Pins::I2C_SCL, OUTPUT);
 digitalWrite(Pins::I2C_SCL, HIGH);
 delayMicroseconds(10);

 if (digitalRead(Pins::I2C_SDA) == LOW) {
 HAL_LOG_W(TAG, "I2C SDA stuck LOW! Attempting recovery...");

 for (int i = 0; i < 9; i++) {
 digitalWrite(Pins::I2C_SCL, LOW);
 delayMicroseconds(10);
 digitalWrite(Pins::I2C_SCL, HIGH);
 delayMicroseconds(10);

 if (digitalRead(Pins::I2C_SDA) == HIGH) {
 HAL_LOG_I(TAG, "I2C Bus recovered after %d clock pulses", i + 1);
 break;
 }
 }

 if (digitalRead(Pins::I2C_SDA) == LOW) {
 HAL_LOG_E(TAG, "I2C Bus recovery FAILED! SDA still LOW.");
 }
 }

 pinMode(Pins::I2C_SDA, OUTPUT);
 digitalWrite(Pins::I2C_SDA, LOW);
 delayMicroseconds(10);
 digitalWrite(Pins::I2C_SCL, HIGH);
 delayMicroseconds(10);
 digitalWrite(Pins::I2C_SDA, HIGH);
 delayMicroseconds(10);

 pinMode(Pins::I2C_SDA, INPUT_PULLUP);
 pinMode(Pins::I2C_SCL, INPUT_PULLUP);

 HAL_LOG_D(TAG, "I2C recovery sequence complete");
}

bool Sensor_HAL::ensureI2CReady {
 if (_i2cInitialized) return true;

 recoverI2CBus;

 Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
 Wire.setClock(I2CConfig::CLOCK_SPEED);
 Wire.setTimeOut(10);
 _i2cInitialized = true;
 return true;
}

bool Sensor_HAL::isPresent {
 if (!_i2cInitialized && !ensureI2CReady) return false;
 Wire.beginTransmission(I2CConfig::Proximity Sensor_ADDR);
 return Wire.endTransmission == 0;
}

void Sensor_HAL::powerDown {
 if (!_initialized) return;
 uint16_t conf1;
 if (readRegister(Proximity Sensor_REG_PS_CONF1, &conf1)) {
 writeRegister(Proximity Sensor_REG_PS_CONF1, conf1 | PS_SD_DIS);
 } else {
 writeRegister(Proximity Sensor_REG_PS_CONF1, PS_SD_DIS);
 }
 _powerOn = false;
}

void Sensor_HAL::powerUp {
 if (!_initialized) return;
 initializeRegisters;
 _powerOn = true;
}

HalError Sensor_HAL::enableInterrupt {
 if (!_initialized) return HalError::NOT_INITIALIZED;
 _interruptTriggered.store(false, std::memory_order_release);
 _lastInterruptTime.store(0, std::memory_order_relaxed);
 attachInterrupt(digitalPinToInterrupt(Pins::PROXIMITY_INT), sensorInterruptHandler, FALLING);
 return HalError::OK;
}

void Sensor_HAL::disableInterrupt {
 detachInterrupt(digitalPinToInterrupt(Pins::PROXIMITY_INT));
 _interruptTriggered.store(false, std::memory_order_release);
}

bool Sensor_HAL::checkTrigger {
 bool wasTriggered = _interruptTriggered.load(std::memory_order_acquire);

 if (!wasTriggered) return false;

 unsigned long lastTriggerTime = _lastInterruptTime.load(std::memory_order_relaxed);
 unsigned long now = millis;

 if (now - lastTriggerTime < Timing::GPIO_WAKEUP_DEBOUNCE_MS) {
 _interruptTriggered.store(false, std::memory_order_release);
 return false;
 }

 _lastInterruptTime.store(now, std::memory_order_relaxed);
 _interruptTriggered.store(false, std::memory_order_release);

 uint16_t proximity = readProximity;
 if (proximity > _wakeThreshold) {
 return true;
 }
 return false;
}

void Sensor_HAL::clearInterrupt {
 _interruptTriggered.store(false, std::memory_order_release);

 uint16_t intFlag = 0;
 readRegister(Proximity Sensor_REG_INT_FLAG, &intFlag);
}

bool Sensor_HAL::writeRegister(uint8_t reg, uint16_t value) {
 Wire.beginTransmission(I2CConfig::Proximity Sensor_ADDR);
 Wire.write(reg);
 Wire.write(value & 0xFF);
 Wire.write((value >> 8) & 0xFF);
 return Wire.endTransmission == 0;
}

bool Sensor_HAL::readRegister(uint8_t reg, uint16_t* value) {
 Wire.beginTransmission(I2CConfig::Proximity Sensor_ADDR);
 Wire.write(reg);
 if (Wire.endTransmission(false) != 0) return false;
 if (Wire.requestFrom((uint8_t)I2CConfig::Proximity Sensor_ADDR, (uint8_t)2) != 2) return false;
 uint8_t low = Wire.read;
 uint8_t high = Wire.read;
 *value = (high << 8) | low;
 return true;
}


void Sensor_HAL::setUserSensitivity(uint8_t sensitivity) {
 if (sensitivity > 100) sensitivity = 100;

 uint8_t oldSens = _userSensitivity.load(std::memory_order_relaxed);
 if (oldSens == sensitivity) {
 HAL_LOG_D(TAG, "Sensitivity unchanged: %u", sensitivity);
 return;
 }

 _userSensitivity.store(sensitivity, std::memory_order_relaxed);

 HAL_LOG_I(TAG, "User sensitivity changed: %u -> %u", oldSens, sensitivity);

 recalculateThresholdsFromSensitivity;
}


void Sensor_HAL::applyEnvironmentBackoff(int16_t offset) {
 int16_t oldBackoff = _environmentBackoff.load(std::memory_order_relaxed);
 _environmentBackoff.store(offset, std::memory_order_release);

 HAL_LOG_I(TAG, "Environment backoff applied: %d -> %d",
 oldBackoff, offset);
}

void Sensor_HAL::clearEnvironmentBackoff {
 int16_t oldBackoff = _environmentBackoff.load(std::memory_order_relaxed);
 if (oldBackoff != 0) {
 _environmentBackoff.store(0, std::memory_order_release);
 HAL_LOG_I(TAG, "Environment backoff cleared: %d -> 0", oldBackoff);
 }
}


void Sensor_HAL::suppressDetections(uint32_t durationMs) {
 unsigned long endTime = millis + durationMs;
 _suppressEndTime.store(endTime, std::memory_order_relaxed);
 _hitsSuppressed.store(true, std::memory_order_release);

 HAL_LOG_I(TAG, "Hit suppression enabled for %lums", durationMs);
}

bool Sensor_HAL::areHitsSuppressed const {
 if (!_hitsSuppressed.load(std::memory_order_acquire)) {
 return false;
 }

 unsigned long endTime = _suppressEndTime.load(std::memory_order_relaxed);
 unsigned long now = millis;

 if (now >= endTime) {
 const_cast<std::atomic<bool>*>(&_hitsSuppressed)->store(false, std::memory_order_release);
 HAL_LOG_I(TAG, "Hit suppression expired");
 return false;
 }

 return true;
}

void Sensor_HAL::recalculateThresholdsFromSensitivity {


 //   1. Piecewise sensitivity-to-multiplier curve (two segments)
 //   2. Fixed-point arithmetic threshold scaling
 //   3. Min/max threshold clamping per level
 //   4. Proportional gap enforcement between detection and critical
 //   5. Overflow protection with coordinated threshold adjustment
 //   6. Atomic threshold publication
}

void Sensor_HAL::setBaseThresholds(uint16_t wake, uint16_t detection, uint16_t critical) {
 HAL_LOG_I(TAG, "Restoring base thresholds from RTC");
 HAL_LOG_I(TAG, " Base Wake: %u", wake);
 HAL_LOG_I(TAG, " Base Hit: %u", detection);
 HAL_LOG_I(TAG, " Base Critical: %u", critical);

 _baseWakeThreshold = wake;
 _baseDetectionThreshold = detection;
 _baseCriticalThreshold = critical;

 _wakeThreshold = wake;

 recalculateThresholdsFromSensitivity;

 HAL_LOG_I(TAG, " Effective thresholds after sensitivity (sens=%u):",
 _userSensitivity.load(std::memory_order_relaxed));
 HAL_LOG_I(TAG, " Hit: %u, Critical: %u",
 _detectionThreshold.load(std::memory_order_relaxed),
 _criticalDetectionThreshold.load(std::memory_order_relaxed));
}

bool Sensor_HAL::isProximityBelowThreshold(uint16_t threshold) {
 if (!_initialized || !_powerOn || !_i2cInitialized) {
 HAL_LOG_W(TAG, "Sensor not ready, assuming safe");
 return true;
 }

 uint16_t currentProx = readProximity;

 bool isBelowThreshold = (currentProx < threshold);

 if (!isBelowThreshold) {
 HAL_LOG_W(TAG, "Proximity %u >= threshold %u",
 currentProx, threshold);
 } else {
 HAL_LOG_D(TAG, "Proximity %u < threshold %u - OK",
 currentProx, threshold);
 }

 return isBelowThreshold;
}

void Sensor_HAL::initializeRegisters {
 HAL_LOG_D(TAG, "Initializing sensor registers with Max Sensitivity");

 uint16_t conf1 = PS_SD_EN | PS_AF_ACTIVE_FORCE | PS_IT_8T | PS_DUTY_1_40 | PS_HD_16BIT | PS_PERS_2 | PS_INT_CLOSE;
 writeRegister(Proximity Sensor_REG_PS_CONF1, conf1);

 uint16_t conf3 = PS_LED_I_200MA | PS_NS_2STEP | PS_SMART_PERS | PS_SC_EN;
 writeRegister(Proximity Sensor_REG_PS_CONF3, conf3);

 writeRegister(Proximity Sensor_REG_PS_CANC, 0);

 delay(20);

 uint32_t calibSum = 0;

 uint16_t dummy;
 writeRegister(Proximity Sensor_REG_PS_CONF3, conf3 | PS_TRIG);
 delay(4);
 readRegister(Proximity Sensor_REG_PS_DATA, &dummy);

 for (int i = 0; i < 10; i++) {
 writeRegister(Proximity Sensor_REG_PS_CONF3, conf3 | PS_TRIG);
 delay(4);
 uint16_t r = 0;
 readRegister(Proximity Sensor_REG_PS_DATA, &r);
 calibSum += r;
 }

 uint16_t baseline = (uint16_t)(calibSum / 10);

 writeRegister(Proximity Sensor_REG_PS_CANC, baseline);

 writeRegister(Proximity Sensor_REG_PS_THDH, _wakeThreshold);
 writeRegister(Proximity Sensor_REG_PS_THDL, 0);

 HAL_LOG_I(TAG, "Sensor Initialized: LED=200mA, 2-Step, 16-bit, 8T, Baseline Canc=%u", baseline);
}
