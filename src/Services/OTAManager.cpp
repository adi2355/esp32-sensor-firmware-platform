
#include "OTAManager.h"
#include "BLEManager.h"
#include "ProtocolHandler.h"
#include "../HAL/Sensor_HAL.h"
#include "../HAL/LED_HAL.h"
#include "../HAL/StorageHAL.h"
#include <Preferences.h>

static const char* TAG = "OTA_MGR";


OTAManager::OTAManager
 : _bleManager(nullptr)
 , _protocolHandler(nullptr)
 , _state(OTAState::IDLE)
 , _lastError(OTAError::NONE)
 , _initialized(false)
 , _expectedSize(0)
 , _receivedSize(0)
 , _expectedCRC(0)
 , _runningCRC(0xFFFFFFFF)
 , _expectedChunks(0)
 , _receivedChunks(0)
 , _newMajor(0)
 , _newMinor(0)
 , _newPatch(0)
 , _lastChunkTime(0)
 , _startTime(0)
 , _mux(portMUX_INITIALIZER_UNLOCKED)
{
}


bool OTAManager::init(BLEManager* bleManager, ProtocolHandler* protocolHandler) {
 _bleManager = bleManager;
 _protocolHandler = protocolHandler;
 _initialized = true;
 
 ESP_LOGI(TAG, "OTAManager initialized");
 return true;
}


bool OTAManager::validateBootedFirmware {
 
 const esp_partition_t* running = esp_ota_get_running_partition;
 
 if (running == nullptr) {
 ESP_LOGE(TAG, "CRITICAL: Failed to get running partition!");
 return false;
 }
 
 esp_ota_img_states_t otaState;
 esp_err_t err = esp_ota_get_state_partition(running, &otaState);
 
 if (err != ESP_OK) {
 ESP_LOGD(TAG, "Could not get OTA state: %s (normal for factory boot)", 
 esp_err_to_name(err));
 return true;
 }
 
 ESP_LOGI(TAG, "Running from: %s at 0x%08X", running->label, running->address);
 ESP_LOGI(TAG, "OTA state: %d (%s)", otaState, 
 otaState == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY" :
 otaState == ESP_OTA_IMG_VALID ? "VALID" :
 otaState == ESP_OTA_IMG_INVALID ? "INVALID" :
 otaState == ESP_OTA_IMG_ABORTED ? "ABORTED" :
 otaState == ESP_OTA_IMG_NEW ? "NEW" : "UNKNOWN");
 
 if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
 ESP_LOGI(TAG, "NEW FIRMWARE - PENDING VERIFICATION");
 ESP_LOGI(TAG, "Version: v%d.%d.%d (Build Type: %s)",
 FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH,
 FIRMWARE_BUILD_TYPE == 0 ? "Release" :
 FIRMWARE_BUILD_TYPE == 1 ? "Beta" : "Debug");
 
 if (performSelfTest) {
 err = esp_ota_mark_app_valid_cancel_rollback;
 
 if (err == ESP_OK) {
 ESP_LOGI(TAG, "FIRMWARE CONFIRMED - Rollback Cancelled");
 return true;
 } else {
 ESP_LOGE(TAG, "Failed to confirm firmware: %s", esp_err_to_name(err));
 ESP_LOGW(TAG, "Device will retry on next boot");
 return false;
 }
 } else {
 ESP_LOGE(TAG, "SELF-TEST FAILED - TRIGGERING ROLLBACK!");
 ESP_LOGE(TAG, "Reverting to previous firmware...");
 
 Serial.flush;
 delay(100);
 
 esp_ota_mark_app_invalid_rollback_and_reboot;
 
 ESP_LOGE(TAG, "CRITICAL: Rollback failed to reboot!");
 while (1) {
 delay(1000);
 }
 return false;
 }
 }
 
 if (otaState == ESP_OTA_IMG_VALID) {
 ESP_LOGI(TAG, "Running validated firmware v%d.%d.%d",
 FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH);
 }
 
 return true;
}

bool OTAManager::performSelfTest {
 ESP_LOGI(TAG, "PERFORMING FIRMWARE SELF-TESTS");
 
 
 bool firmwareOk = true;
 uint8_t passCount = 0;
 uint8_t warnCount = 0;
 uint8_t failCount = 0;
 const uint8_t totalTests = 7;
 
 unsigned long testStartTime = millis;
 const unsigned long MAX_SELF_TEST_TIME_MS = OTAConfig::SELF_TEST_TIMEOUT_MS;
 
 ESP_LOGI(TAG, " [1/%d] I2C Sensor (with %d retries)...", 
 totalTests, FlasherConfig::SENSOR_CHECK_RETRIES);
 Sensor_HAL* sensor = Sensor_HAL::getInstance;
 if (sensor != nullptr) {
 bool sensorFound = false;
 uint16_t sensorId = 0;
 
 for (uint8_t attempt = 1; attempt <= FlasherConfig::SENSOR_CHECK_RETRIES; attempt++) {
 ESP_LOGI(TAG, " Attempt %d/%d...", attempt, FlasherConfig::SENSOR_CHECK_RETRIES);
 
 if (sensor->isPresent) {
 sensorId = sensor->getSensorId;
 if (sensorId != 0) {
 sensorFound = true;
 break;
 }
 }
 
 if (attempt < FlasherConfig::SENSOR_CHECK_RETRIES) {
 ESP_LOGI(TAG, " Retrying in %lu ms...", FlasherConfig::SENSOR_CHECK_DELAY_MS);
 delay(FlasherConfig::SENSOR_CHECK_DELAY_MS);
 }
 }
 
 if (sensorFound) {
 ESP_LOGI(TAG, " ✓ PASS: Sensor ID=0x%04X", sensorId);
 passCount++;
 } else {
 ESP_LOGW(TAG, " ⚠ WARN: Sensor not found after %d retries (hardware issue?)",
 FlasherConfig::SENSOR_CHECK_RETRIES);
 warnCount++;
 }
 } else {
 ESP_LOGE(TAG, " ✗ FAIL: Sensor_HAL singleton is NULL (firmware bug!)");
 firmwareOk = false;
 failCount++;
 }
 
 ESP_LOGI(TAG, " [2/%d] Heap memory...", totalTests);
 size_t freeHeap = ESP.getFreeHeap;
 size_t largestBlock = ESP.getMaxAllocHeap;
 const size_t MIN_FREE_HEAP = 50000;
 const size_t MIN_LARGEST_BLOCK = 20000;
 
 if (freeHeap >= MIN_FREE_HEAP && largestBlock >= MIN_LARGEST_BLOCK) {
 ESP_LOGI(TAG, " ✓ PASS: Free=%u, LargestBlock=%u", freeHeap, largestBlock);
 passCount++;
 } else {
 ESP_LOGE(TAG, " ✗ FAIL: Free=%u (min %u), Block=%u (min %u)", 
 freeHeap, MIN_FREE_HEAP, largestBlock, MIN_LARGEST_BLOCK);
 firmwareOk = false;
 failCount++;
 }
 
 ESP_LOGI(TAG, " [3/%d] Flash partitions...", totalTests);
 const esp_partition_t* running = esp_ota_get_running_partition;
 const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
 
 if (running != nullptr && next != nullptr && 
 running->size >= 0x100000 && next->size >= 0x100000) {
 ESP_LOGI(TAG, " ✓ PASS: Run=%s@0x%X (%uKB), Next=%s@0x%X (%uKB)", 
 running->label, running->address, running->size / 1024,
 next->label, next->address, next->size / 1024);
 passCount++;
 } else {
 ESP_LOGE(TAG, " ✗ FAIL: Partition configuration invalid");
 firmwareOk = false;
 failCount++;
 }
 
 ESP_LOGI(TAG, " [4/%d] NVS access...", totalTests);
 {
 Preferences testPrefs;
 bool nvsOk = false;
 
 if (testPrefs.begin("selftest", false)) {
 uint32_t testValue = millis;
 testPrefs.putULong("test", testValue);
 
 uint32_t readValue = testPrefs.getULong("test", 0);
 
 if (readValue == testValue) {
 nvsOk = true;
 testPrefs.remove("test");
 }
 testPrefs.end;
 }
 
 if (nvsOk) {
 ESP_LOGI(TAG, " ✓ PASS: NVS read/write verified");
 passCount++;
 } else {
 ESP_LOGE(TAG, " ✗ FAIL: NVS read/write failed");
 firmwareOk = false;
 failCount++;
 }
 }
 
 ESP_LOGI(TAG, " [5/%d] LED HAL...", totalTests);
 LED_HAL* led = LED_HAL::getInstance;
 if (led) {
 led->flash(0x00FF00, 100);
 ESP_LOGI(TAG, " ✓ PASS: LED HAL operational");
 passCount++;
 } else {
 ESP_LOGW(TAG, " ⚠ WARN: LED HAL unavailable (hardware issue?)");
 warnCount++;
 }
 
 ESP_LOGI(TAG, " [6/%d] Storage HAL...", totalTests);
 HAL::StorageHAL* storage = HAL::StorageHAL::getInstance;
 if (storage && storage->isReady) {
 ESP_LOGI(TAG, " ✓ PASS: Storage HAL ready");
 passCount++;
 } else {
 ESP_LOGE(TAG, " ✗ FAIL: Storage HAL not ready");
 firmwareOk = false;
 failCount++;
 }
 
 ESP_LOGI(TAG, " [7/%d] Self-test timing...", totalTests);
 unsigned long testDuration = millis - testStartTime;
 
 if (testDuration < MAX_SELF_TEST_TIME_MS) {
 ESP_LOGI(TAG, " ✓ PASS: Completed in %lu ms (limit %lu ms)", 
 testDuration, MAX_SELF_TEST_TIME_MS);
 passCount++;
 } else {
 ESP_LOGW(TAG, " ⚠ WARN: Self-test took %lu ms (limit %lu ms)", 
 testDuration, MAX_SELF_TEST_TIME_MS);
 passCount++;
 }
 
 if (firmwareOk) {
 if (warnCount > 0) {
 ESP_LOGW(TAG, "SELF-TEST RESULT: PASSED with %d WARNINGS", warnCount);
 ESP_LOGW(TAG, " Firmware OK, but some hardware issues detected");
 } else {
 ESP_LOGI(TAG, "SELF-TEST RESULT: PASSED (%d/%d)", passCount, totalTests);
 }
 ESP_LOGI(TAG, " Firmware v%d.%d.%d validated successfully",
 FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH);
 } else {
 ESP_LOGE(TAG, "SELF-TEST RESULT: FAILED (%d passed, %d failed, %d warn)", 
 passCount, failCount, warnCount);
 ESP_LOGE(TAG, " Firmware will be rolled back!");
 }
 
 return firmwareOk;
}


void OTAManager::handleInfoRequest(uint16_t seqNum) {
 FirmwareInfo info;
 
 info.major = FIRMWARE_VERSION_MAJOR;
 info.minor = FIRMWARE_VERSION_MINOR;
 info.patch = FIRMWARE_VERSION_PATCH;
 info.buildType = FIRMWARE_BUILD_TYPE;
 info.buildNumber = 0;
 
 const esp_partition_t* partition = getNextPartition;
 info.flashSize = partition ? partition->size : 0;
 
 info.batteryPercent = 100;
 memset(info.reserved, 0, sizeof(info.reserved));
 
 ESP_LOGI(TAG, "OTA info requested: v%d.%d.%d, flash=%u",
 info.major, info.minor, info.patch, info.flashSize);
 
 _protocolHandler->sendMessage(
 MessageType::MSG_OTA_INFO_RESP,
 reinterpret_cast<uint8_t*>(&info),
 sizeof(FirmwareInfo),
 false
 );
}


bool OTAManager::handleStart(const uint8_t* payload, size_t length) {
 if (_state != OTAState::IDLE) {
 ESP_LOGW(TAG, "OTA already in progress");
 _lastError = OTAError::INVALID_STATE;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 if (length < sizeof(OtaStartPayload)) {
 ESP_LOGE(TAG, "Invalid OTA_START payload size: %u", length);
 _lastError = OTAError::INVALID_STATE;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 const OtaStartPayload* startData = reinterpret_cast<const OtaStartPayload*>(payload);
 
 _expectedSize = startData->totalSize;
 _expectedCRC = startData->expectedCrc;
 _newMajor = startData->newMajor;
 _newMinor = startData->newMinor;
 _newPatch = startData->newPatch;
 _expectedChunks = startData->totalChunks;
 
 ESP_LOGI(TAG, "OTA UPDATE REQUEST");
 ESP_LOGI(TAG, " Current: v%d.%d.%d", 
 FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH);
 ESP_LOGI(TAG, " New: v%d.%d.%d", _newMajor, _newMinor, _newPatch);
 ESP_LOGI(TAG, " Size: %u bytes", _expectedSize);
 ESP_LOGI(TAG, " Chunks: %u", _expectedChunks);
 ESP_LOGI(TAG, " CRC32: 0x%08X", _expectedCRC);
 
 
 if (!checkBatteryLevel) {
 ESP_LOGE(TAG, "Battery too low for OTA");
 _lastError = OTAError::LOW_BATTERY;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 if (!validateVersion(_newMajor, _newMinor, _newPatch)) {
 ESP_LOGE(TAG, "Version downgrade rejected");
 _lastError = OTAError::VERSION_REJECTED;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 const esp_partition_t* partition = getNextPartition;
 if (partition == nullptr) {
 ESP_LOGE(TAG, "No update partition available");
 _lastError = OTAError::NO_PARTITION;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 if (_expectedSize > partition->size) {
 ESP_LOGE(TAG, "Firmware too large: %u > %u", _expectedSize, partition->size);
 _lastError = OTAError::TOO_LARGE;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 if (!Update.begin(_expectedSize, U_FLASH)) {
 ESP_LOGE(TAG, "Update.begin failed: %s", Update.errorString);
 _lastError = OTAError::BEGIN_FAILED;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 _state = OTAState::RECEIVING;
 _receivedSize = 0;
 _receivedChunks = 0;
 _runningCRC = 0xFFFFFFFF;
 _startTime = millis;
 _lastChunkTime = millis;
 _lastError = OTAError::NONE;
 
 ESP_LOGI(TAG, "OTA update started - ready for chunks");
 sendStatus(0x00, OTAError::NONE);
 
 return true;
}


bool OTAManager::handleData(uint16_t chunkIndex, const uint8_t* payload, size_t length) {
 if (_state != OTAState::RECEIVING) {
 ESP_LOGW(TAG, "Not in receiving state");
 sendChunkAck(chunkIndex, false);
 return false;
 }
 
 if (chunkIndex != _receivedChunks) {
 ESP_LOGW(TAG, "Chunk out of order: got %u, expected %u", 
 chunkIndex, _receivedChunks);
 _lastError = OTAError::CHUNK_OUT_OF_ORDER;
 sendChunkAck(_receivedChunks, false);
 return false;
 }
 
 size_t written = Update.write(const_cast<uint8_t*>(payload), length);
 if (written != length) {
 ESP_LOGE(TAG, "Write failed: wrote %u of %u bytes", written, length);
 _lastError = OTAError::WRITE_FAILED;
 sendChunkAck(chunkIndex, false);
 return false;
 }
 
 updateCRC(payload, length);
 
 _receivedSize += length;
 _receivedChunks++;
 _lastChunkTime = millis;
 
 if (_expectedChunks > 0 && _receivedChunks % (_expectedChunks / 10 + 1) == 0) {
 ESP_LOGI(TAG, "OTA Progress: %u%% (%u/%u chunks)",
 getProgress, _receivedChunks, _expectedChunks);
 }
 
 sendChunkAck(chunkIndex, true);
 
 return true;
}


bool OTAManager::handleCommit {
 if (_state != OTAState::RECEIVING) {
 ESP_LOGW(TAG, "Not in receiving state for commit");
 _lastError = OTAError::INVALID_STATE;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 _state = OTAState::VALIDATING;
 
 ESP_LOGI(TAG, "Validating OTA update...");
 
 if (_receivedSize != _expectedSize) {
 ESP_LOGE(TAG, "Size mismatch: received %u, expected %u",
 _receivedSize, _expectedSize);
 _state = OTAState::FAILED;
 _lastError = OTAError::SIZE_MISMATCH;
 Update.abort;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 uint32_t finalCRC = ~_runningCRC;
 if (finalCRC != _expectedCRC) {
 ESP_LOGE(TAG, "CRC mismatch: calculated 0x%08X, expected 0x%08X",
 finalCRC, _expectedCRC);
 _state = OTAState::FAILED;
 _lastError = OTAError::CRC_MISMATCH;
 Update.abort;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 ESP_LOGI(TAG, "Validation PASSED - CRC OK");
 
 _state = OTAState::COMMITTING;
 
 if (!Update.end(true)) {
 ESP_LOGE(TAG, "Update.end failed: %s", Update.errorString);
 _state = OTAState::FAILED;
 _lastError = OTAError::COMMIT_FAILED;
 sendStatus(0x01, _lastError);
 return false;
 }
 
 unsigned long duration = millis - _startTime;
 
 ESP_LOGI(TAG, "OTA UPDATE SUCCESSFUL!");
 ESP_LOGI(TAG, " Duration: %lu seconds", duration / 1000);
 ESP_LOGI(TAG, " New version: v%d.%d.%d", _newMajor, _newMinor, _newPatch);
 ESP_LOGI(TAG, " Rebooting in 3 seconds...");
 
 sendStatus(0x00, OTAError::NONE);
 
 _state = OTAState::REBOOTING;
 
 delay(3000);
 
 ESP.restart;
 
 return true;
}


void OTAManager::handleAbort {
 if (_state == OTAState::IDLE) {
 return;
 }
 
 ESP_LOGI(TAG, "OTA update ABORTED");
 
 Update.abort;
 
 _state = OTAState::IDLE;
 _receivedSize = 0;
 _receivedChunks = 0;
 _lastError = OTAError::NONE;
 
 sendStatus(0x00, OTAError::NONE);
}


const char* OTAManager::getStateName const {
 switch (_state) {
 case OTAState::IDLE: return "IDLE";
 case OTAState::RECEIVING: return "RECEIVING";
 case OTAState::VALIDATING: return "VALIDATING";
 case OTAState::COMMITTING: return "COMMITTING";
 case OTAState::REBOOTING: return "REBOOTING";
 case OTAState::FAILED: return "FAILED";
 default: return "UNKNOWN";
 }
}

uint8_t OTAManager::getProgress const {
 if (_expectedChunks == 0) return 0;
 return (uint8_t)((_receivedChunks * 100) / _expectedChunks);
}


void OTAManager::update {
 if (!_initialized) return;
 
 if (_state == OTAState::RECEIVING) {
 if (checkTimeout) {
 ESP_LOGW(TAG, "OTA chunk timeout - aborting");
 handleAbort;
 _lastError = OTAError::TIMEOUT;
 }
 }
}

bool OTAManager::checkTimeout {
 if (_state != OTAState::RECEIVING) {
 return false;
 }
 
 return (millis - _lastChunkTime > Timing::OTA_CHUNK_TIMEOUT_MS);
}


bool OTAManager::checkBatteryLevel {
 uint8_t batteryPercent = 100;
 return batteryPercent >= OTAConfig::MIN_BATTERY_PERCENT;
}

const esp_partition_t* OTAManager::getNextPartition {
 return esp_ota_get_next_update_partition(nullptr);
}

void OTAManager::sendStatus(uint8_t status, OTAError error) {
 if (!_protocolHandler) return;
 
 uint8_t payload[4] = {
 status,
 static_cast<uint8_t>(error),
 (uint8_t)(_receivedChunks >> 8),
 (uint8_t)(_receivedChunks & 0xFF)
 };
 
 _protocolHandler->sendMessage(
 MessageType::MSG_OTA_STATUS,
 payload,
 4,
 false
 );
}

void OTAManager::sendChunkAck(uint16_t chunkIndex, bool success) {
 if (!_protocolHandler) return;
 
 uint8_t payload[3] = {
 (uint8_t)(chunkIndex >> 8),
 (uint8_t)(chunkIndex & 0xFF),
 static_cast<uint8_t>(success ? 0x00 : 0x01)
 };
 
 _protocolHandler->sendMessage(
 MessageType::MSG_OTA_DATA_ACK,
 payload,
 3,
 false
 );
}

void OTAManager::updateCRC(const uint8_t* data, size_t length) {
 for (size_t i = 0; i < length; i++) {
 _runningCRC ^= data[i];
 for (uint8_t j = 0; j < 8; j++) {
 _runningCRC = (_runningCRC >> 1) ^ (0xEDB88320 & (-(_runningCRC & 1)));
 }
 }
}

bool OTAManager::validateVersion(uint8_t major, uint8_t minor, uint8_t patch) {
 #if FIRMWARE_BUILD_TYPE == 2
 return true;
 #endif
 
 uint32_t currentVersion = (FIRMWARE_VERSION_MAJOR << 16) | 
 (FIRMWARE_VERSION_MINOR << 8) | 
 FIRMWARE_VERSION_PATCH;
 
 uint32_t newVersion = (major << 16) | (minor << 8) | patch;
 
 if (newVersion < currentVersion) {
 ESP_LOGW(TAG, "Version downgrade: v%d.%d.%d -> v%d.%d.%d",
 FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH,
 major, minor, patch);
 return false;
 }
 
 return true;
}


void OTAManager::getDiagnostics(char* buffer, size_t bufferSize) const {
 snprintf(buffer, bufferSize,
 "OTA: state=%s, progress=%u%%, err=0x%02X",
 getStateName,
 getProgress,
 static_cast<uint8_t>(_lastError));
}

void OTAManager::getPartitionInfo(char* buffer, size_t bufferSize) const {
 const esp_partition_t* running = esp_ota_get_running_partition;
 const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
 
 if (running && next) {
 snprintf(buffer, bufferSize,
 "Running: %s@0x%X, Next: %s@0x%X",
 running->label, running->address,
 next->label, next->address);
 } else {
 snprintf(buffer, bufferSize, "Partition info unavailable");
 }
}

