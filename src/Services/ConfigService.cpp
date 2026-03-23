
#include "ConfigService.h"
#include "../HAL/Sensor_HAL.h"
#include "../Domain/ProtocolDefs.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <errno.h>


ConfigService* ConfigService::_instance = nullptr;

ConfigService* ConfigService::getInstance {
 if (_instance == nullptr) {
 _instance = new ConfigService;
 }
 return _instance;
}


ConfigService::ConfigService
 : _sensorHal(nullptr)
 , _initialized(false)
 , _dirty(false)
 , _commitTimer(nullptr)
 , _configWorkerTask(nullptr)
 , _configWorkerRunning(false)
 , _mutex(nullptr)
{
 initUserConfigDefaults(&_config);
 
 _mutex = xSemaphoreCreateMutex;
 if (_mutex == nullptr) {
 ESP_LOGE(TAG, "Failed to create mutex!");
 }
}


bool ConfigService::init(Sensor_HAL* sensorHal) {
 if (_initialized) {
 ESP_LOGW(TAG, "Already initialized");
 return true;
 }
 
 if (sensorHal == nullptr) {
 ESP_LOGE(TAG, "Sensor_HAL is null!");
 return false;
 }
 
 _sensorHal = sensorHal;
 
 ESP_LOGI(TAG, "Initializing ConfigService...");
 
 _commitTimer = xTimerCreate(
 "ConfigCommit",
 pdMS_TO_TICKS(COMMIT_DELAY_MS),
 pdFALSE,
 (void*)this,
 commitTimerCallback
 );
 
 if (_commitTimer == nullptr) {
 ESP_LOGE(TAG, "Failed to create commit timer!");
 return false;
 }
 
 _configWorkerRunning = true;
 BaseType_t taskResult = xTaskCreatePinnedToCore(
 configWorkerTask,
 "ConfigWorker",
 2048,
 this,
 1,
 &_configWorkerTask,
 0
 );
 
 if (taskResult != pdPASS || _configWorkerTask == nullptr) {
 ESP_LOGE(TAG, "Failed to create ConfigWorker task!");
 _configWorkerRunning = false;
 return false;
 }
 
 ESP_LOGI(TAG, "ConfigWorker task created (handle=%p)", _configWorkerTask);
 
 if (readFromNvs) {
 ESP_LOGI(TAG, "Loaded config from NVS:");
 ESP_LOGI(TAG, " Sensitivity: %u", _config.sensitivity);
 ESP_LOGI(TAG, " LED Brightness: %u", _config.ledBrightness);
 ESP_LOGI(TAG, " Last Known Epoch: %lu", _config.lastKnownEpoch);
 
 if (_config.lastKnownEpoch > 0) {
 struct timeval tv;
 tv.tv_sec = _config.lastKnownEpoch + 1;
 tv.tv_usec = 0;
 
 int result = settimeofday(&tv, NULL);
 if (result == 0) {
 ESP_LOGI(TAG, "FORWARD-CHAIN: Restored system clock to epoch %lu (+1s)",
 _config.lastKnownEpoch);
 } else {
 ESP_LOGW(TAG, "FORWARD-CHAIN: Failed to restore system clock (errno=%d)", errno);
 }
 } else {
 ESP_LOGI(TAG, "FORWARD-CHAIN: No previous epoch stored (first sync will set time)");
 }
 } else {
 ESP_LOGW(TAG, "No valid config in NVS, using defaults");
 initUserConfigDefaults(&_config);
 
 _config.crc32 = calculateConfigCrc;
 writeToNvs;
 }
 
 applySensitivityToHal;
 
 _initialized = true;
 _dirty = false;
 
 ESP_LOGI(TAG, "ConfigService initialized successfully");
 return true;
}


bool ConfigService::readFromNvs {
 nvs_handle_t handle;
 esp_err_t err;
 
 err = nvs_open(UserConfigNvs::NAMESPACE, NVS_READONLY, &handle);
 if (err != ESP_OK) {
 if (err == ESP_ERR_NVS_NOT_FOUND) {
 ESP_LOGD(TAG, "NVS namespace not found (first boot)");
 } else {
 ESP_LOGW(TAG, "Failed to open NVS for read: %s", esp_err_to_name(err));
 }
 return false;
 }
 
 size_t requiredSize = sizeof(UserConfig);
 UserConfig tempConfig;
 
 err = nvs_get_blob(handle, UserConfigNvs::KEY_DATA, &tempConfig, &requiredSize);
 nvs_close(handle);
 
 if (err != ESP_OK) {
 if (err == ESP_ERR_NVS_NOT_FOUND) {
 ESP_LOGD(TAG, "Config key not found in NVS");
 } else {
 ESP_LOGW(TAG, "Failed to read config from NVS: %s", esp_err_to_name(err));
 }
 return false;
 }
 
 if (requiredSize != sizeof(UserConfig)) {
 ESP_LOGW(TAG, "Config size mismatch: got %u, expected %u", 
 requiredSize, sizeof(UserConfig));
 return false;
 }
 
 uint32_t calculatedCrc = calculateCRC32(
 reinterpret_cast<const uint8_t*>(&tempConfig),
 UserConfigNvs::CRC_DATA_SIZE
 );
 
 if (calculatedCrc != tempConfig.crc32) {
 ESP_LOGW(TAG, "Config CRC mismatch: stored=0x%08X, calculated=0x%08X",
 tempConfig.crc32, calculatedCrc);
 return false;
 }
 
 if (tempConfig.version != UserConfigDefaults::VERSION) {
 ESP_LOGW(TAG, "Config version mismatch: stored=%u, current=%u",
 tempConfig.version, UserConfigDefaults::VERSION);
 
 
 if (tempConfig.version == 1 && UserConfigDefaults::VERSION >= 2) {
 
 ESP_LOGI(TAG, "Migrating config v1 -> v2 (adding signature)");
 
 uint8_t oldSensitivity = tempConfig.sensitivity;
 uint8_t oldLedBrightness = tempConfig.ledBrightness;
 
 tempConfig.version = 2;
 tempConfig.sensitivity = oldSensitivity;
 tempConfig.ledBrightness = oldLedBrightness;
 tempConfig.reserved = 0;
 tempConfig.signature = esp_random;
 
 ESP_LOGI(TAG, "v1->v2 complete: sens=%u, ledBright=%u, signature=0x%08X",
 tempConfig.sensitivity, tempConfig.ledBrightness, tempConfig.signature);
 }
 
 if (tempConfig.version == 2 && UserConfigDefaults::VERSION == 3) {
 
 ESP_LOGI(TAG, "Migrating config v2 -> v3 (adding lastKnownEpoch)");
 
 tempConfig.version = UserConfigDefaults::VERSION;
 tempConfig.lastKnownEpoch = 0;
 
 ESP_LOGI(TAG, "v2->v3 complete: lastKnownEpoch initialized to 0");
 }
 
 if (tempConfig.version == UserConfigDefaults::VERSION) {
 _config = tempConfig;
 _config.crc32 = calculateConfigCrc;
 writeToNvs;
 
 ESP_LOGI(TAG, "Migration complete to v%u", UserConfigDefaults::VERSION);
 return true;
 }
 
 ESP_LOGW(TAG, "Cannot migrate from version %u - using defaults", tempConfig.version);
 return false;
 }
 
 if (tempConfig.sensitivity > 100) {
 ESP_LOGW(TAG, "Invalid sensitivity value: %u", tempConfig.sensitivity);
 return false;
 }
 
 _config = tempConfig;
 return true;
}

bool ConfigService::writeToNvs {
 if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
 ESP_LOGW(TAG, "Failed to acquire mutex for NVS write");
 return false;
 }
 
 _config.crc32 = calculateConfigCrc;
 
 nvs_handle_t handle;
 esp_err_t err;
 
 err = nvs_open(UserConfigNvs::NAMESPACE, NVS_READWRITE, &handle);
 if (err != ESP_OK) {
 ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
 xSemaphoreGive(_mutex);
 return false;
 }
 
 err = nvs_set_blob(handle, UserConfigNvs::KEY_DATA, &_config, sizeof(UserConfig));
 if (err != ESP_OK) {
 ESP_LOGE(TAG, "Failed to write config to NVS: %s", esp_err_to_name(err));
 nvs_close(handle);
 xSemaphoreGive(_mutex);
 return false;
 }
 
 err = nvs_commit(handle);
 nvs_close(handle);
 
 if (err != ESP_OK) {
 ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
 xSemaphoreGive(_mutex);
 return false;
 }
 
 _dirty = false;
 ESP_LOGI(TAG, "Config written to NVS (sensitivity=%u)", _config.sensitivity);
 
 xSemaphoreGive(_mutex);
 return true;
}

uint32_t ConfigService::calculateConfigCrc const {
 return calculateCRC32(
 reinterpret_cast<const uint8_t*>(&_config),
 UserConfigNvs::CRC_DATA_SIZE
 );
}


void ConfigService::commitTimerCallback(TimerHandle_t xTimer) {
 ConfigService* self = static_cast<ConfigService*>(pvTimerGetTimerID(xTimer));
 if (self == nullptr) {
 ESP_LOGE("CONFIG_TIMER", "Timer callback: self is null!");
 return;
 }
 
 if (self->_dirty && self->_configWorkerTask != nullptr) {
 ESP_LOGD("CONFIG_TIMER", "Commit timer expired - notifying ConfigWorker");
 xTaskNotify(self->_configWorkerTask, NOTIFY_SAVE_CONFIG, eSetBits);
 }
}


void ConfigService::configWorkerTask(void* pvParameters) {
 ConfigService* self = static_cast<ConfigService*>(pvParameters);
 if (self == nullptr) {
 ESP_LOGE("CONFIG_WORKER", "Task started with null context!");
 vTaskDelete(nullptr);
 return;
 }
 
 ESP_LOGI("CONFIG_WORKER", "ConfigWorker task started");
 
 while (self->_configWorkerRunning) {
 uint32_t notificationValue = 0;
 BaseType_t result = xTaskNotifyWait(
 0,
 UINT32_MAX,
 &notificationValue,
 pdMS_TO_TICKS(1000)
 );
 
 if (result == pdTRUE) {
 if (notificationValue & NOTIFY_SHUTDOWN) {
 ESP_LOGI("CONFIG_WORKER", "Shutdown notification received");
 break;
 }
 
 if (notificationValue & NOTIFY_SAVE_CONFIG) {
 if (self->_dirty) {
 ESP_LOGI("CONFIG_WORKER", "Performing async NVS write");
 
 
 esp_task_wdt_reset;
 
 vTaskDelay(pdMS_TO_TICKS(100));
 
 self->writeToNvs;
 
 vTaskDelay(pdMS_TO_TICKS(100));
 }
 }
 }
 }
 
 ESP_LOGI("CONFIG_WORKER", "ConfigWorker task exiting");
 self->_configWorkerTask = nullptr;
 vTaskDelete(nullptr);
}


uint8_t ConfigService::getSensitivity const {
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint8_t value = _config.sensitivity;
 xSemaphoreGive(_mutex);
 return value;
 }
 return _config.sensitivity;
}

void ConfigService::setSensitivity(uint8_t sensitivity) {
 sensitivity = validateSensitivity(sensitivity);
 
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
 ESP_LOGW(TAG, "Failed to acquire mutex for setSensitivity");
 return;
 }
 
 if (_config.sensitivity == sensitivity) {
 if (_mutex) xSemaphoreGive(_mutex);
 ESP_LOGD(TAG, "Sensitivity unchanged: %u", sensitivity);
 return;
 }
 
 uint8_t oldValue = _config.sensitivity;
 _config.sensitivity = sensitivity;
 _dirty = true;
 
 if (_mutex) xSemaphoreGive(_mutex);
 
 ESP_LOGI(TAG, "Sensitivity changed: %u -> %u", oldValue, sensitivity);
 
 applySensitivityToHal;
 
 if (_commitTimer != nullptr) {
 if (xTimerIsTimerActive(_commitTimer) != pdFALSE) {
 xTimerReset(_commitTimer, 0);
 ESP_LOGD(TAG, "Commit timer reset");
 } else {
 xTimerStart(_commitTimer, 0);
 ESP_LOGD(TAG, "Commit timer started");
 }
 }
}

void ConfigService::applySensitivityToHal {
 if (_sensorHal == nullptr) {
 ESP_LOGW(TAG, "Cannot apply sensitivity: Sensor_HAL is null");
 return;
 }
 
 _sensorHal->setUserSensitivity(_config.sensitivity);
}


uint8_t ConfigService::getLedBrightness const {
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint8_t value = _config.ledBrightness;
 xSemaphoreGive(_mutex);
 return value;
 }
 return _config.ledBrightness;
}

void ConfigService::setLedBrightness(uint8_t brightness) {
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
 ESP_LOGW(TAG, "Failed to acquire mutex for setLedBrightness");
 return;
 }
 
 if (_config.ledBrightness == brightness) {
 if (_mutex) xSemaphoreGive(_mutex);
 return;
 }
 
 _config.ledBrightness = brightness;
 _dirty = true;
 
 if (_mutex) xSemaphoreGive(_mutex);
 
 ESP_LOGI(TAG, "LED brightness changed to: %u", brightness);
 
 if (_commitTimer != nullptr) {
 if (xTimerIsTimerActive(_commitTimer) != pdFALSE) {
 xTimerReset(_commitTimer, 0);
 } else {
 xTimerStart(_commitTimer, 0);
 }
 }
 
}


bool ConfigService::isDirty const {
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 bool value = _dirty;
 xSemaphoreGive(_mutex);
 return value;
 }
 return _dirty;
}

bool ConfigService::forceFlush {
 if (_commitTimer != nullptr && xTimerIsTimerActive(_commitTimer) != pdFALSE) {
 xTimerStop(_commitTimer, 0);
 }
 
 if (!_dirty) {
 ESP_LOGD(TAG, "forceFlush: nothing to write");
 return true;
 }
 
 
 ESP_LOGI(TAG, "forceFlush: writing to NVS (synchronous)");
 return writeToNvs;
}

bool ConfigService::factoryReset {
 ESP_LOGW(TAG, "Factory reset requested");
 
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
 ESP_LOGW(TAG, "Failed to acquire mutex for factoryReset");
 return false;
 }
 
 if (_commitTimer != nullptr && xTimerIsTimerActive(_commitTimer) != pdFALSE) {
 xTimerStop(_commitTimer, 0);
 }
 
 initUserConfigDefaults(&_config);
 _config.crc32 = calculateConfigCrc;
 
 if (_mutex) xSemaphoreGive(_mutex);
 
 bool success = writeToNvs;
 
 if (success) {
 applySensitivityToHal;
 ESP_LOGI(TAG, "Factory reset complete");
 }
 
 return success;
}


void ConfigService::getConfig(UserConfig* outConfig) const {
 if (outConfig == nullptr) return;
 
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 *outConfig = _config;
 xSemaphoreGive(_mutex);
 } else {
 *outConfig = _config;
 }
}

uint32_t ConfigService::getConfigCrc const {
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint32_t crc = _config.crc32;
 xSemaphoreGive(_mutex);
 return crc;
 }
 return _config.crc32;
}

uint32_t ConfigService::getConfigSignature const {
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint32_t sig = _config.signature;
 xSemaphoreGive(_mutex);
 return sig;
 }
 return _config.signature;
}


uint32_t ConfigService::getLastKnownEpoch const {
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
 uint32_t epoch = _config.lastKnownEpoch;
 xSemaphoreGive(_mutex);
 return epoch;
 }
 return _config.lastKnownEpoch;
}

void ConfigService::updateLastKnownEpoch(uint32_t epochSeconds) {
 if (epochSeconds <= _config.lastKnownEpoch) {
 ESP_LOGD(TAG, "updateLastKnownEpoch: Not updating (new=%lu <= current=%lu)",
 epochSeconds, _config.lastKnownEpoch);
 return;
 }
 
 if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
 ESP_LOGW(TAG, "Failed to acquire mutex for updateLastKnownEpoch");
 return;
 }
 
 uint32_t oldEpoch = _config.lastKnownEpoch;
 _config.lastKnownEpoch = epochSeconds;
 _dirty = true;
 
 if (_mutex) xSemaphoreGive(_mutex);
 
 ESP_LOGI(TAG, "lastKnownEpoch updated: %lu -> %lu", oldEpoch, epochSeconds);
 
 if (_commitTimer != nullptr) {
 if (xTimerIsTimerActive(_commitTimer) != pdFALSE) {
 xTimerReset(_commitTimer, 0);
 } else {
 xTimerStart(_commitTimer, 0);
 }
 }
}


void ConfigService::getDiagnostics(char* buffer, size_t bufferSize) const {
 if (buffer == nullptr || bufferSize == 0) return;
 
 uint8_t sens = getSensitivity;
 float mult = sensitivityToMultiplier(sens);
 uint32_t lastEpoch = getLastKnownEpoch;
 
 snprintf(buffer, bufferSize,
 "Config: sens=%u (mult=%.2f), led=%u, lastEpoch=%lu, dirty=%s",
 sens, mult, _config.ledBrightness, lastEpoch,
 _dirty ? "Y" : "N");
}

