
#include "StorageHAL.h"
#include "esp_ota_ops.h"

namespace HAL {

StorageHAL* StorageHAL::_instance = nullptr;


static constexpr uint32_t MIN_NVS_SIZE = 0x5000;

static constexpr uint32_t MIN_APP_SIZE = 0x100000;


StorageHAL::StorageHAL
 : _initialized(false)
 , _lastError(PartitionError::OK)
 , _nvsRecovered(false)
 , _nvsSize(0)
 , _appSlotSize(0)
{
}


bool StorageHAL::validatePartitions {
 HAL_LOG_I(TAG, "VALIDATING FLASH PARTITION SCHEME");
 
 const esp_partition_t* nvs = esp_partition_find_first(
 ESP_PARTITION_TYPE_DATA, 
 ESP_PARTITION_SUBTYPE_DATA_NVS, 
 "nvs"
 );
 
 if (nvs == nullptr) {
 HAL_LOG_E(TAG, "CRITICAL: NVS Partition MISSING!");
 _lastError = PartitionError::NVS_MISSING;
 return false;
 }
 
 _nvsSize = nvs->size;
 HAL_LOG_I(TAG, " NVS Partition: 0x%X bytes (%u KB)", nvs->size, nvs->size / 1024);
 
 if (nvs->size < MIN_NVS_SIZE) {
 HAL_LOG_E(TAG, "CRITICAL: NVS Partition too small for bonds!");
 HAL_LOG_E(TAG, " Required: >= 0x%X (%u KB)", MIN_NVS_SIZE, MIN_NVS_SIZE / 1024);
 HAL_LOG_E(TAG, " Actual: 0x%X (%u KB)", nvs->size, nvs->size / 1024);
 _lastError = PartitionError::NVS_TOO_SMALL;
 return false;
 }
 
 const esp_partition_t* otadata = esp_partition_find_first(
 ESP_PARTITION_TYPE_DATA, 
 ESP_PARTITION_SUBTYPE_DATA_OTA, 
 "otadata"
 );
 
 if (otadata == nullptr) {
 HAL_LOG_E(TAG, "CRITICAL: OTA Data Partition MISSING!");
 HAL_LOG_E(TAG, " OTA updates will fail without this partition.");
 _lastError = PartitionError::OTADATA_MISSING;
 return false;
 }
 
 HAL_LOG_I(TAG, " OTA Data: 0x%X bytes", otadata->size);
 
 const esp_partition_t* app0 = esp_partition_find_first(
 ESP_PARTITION_TYPE_APP, 
 ESP_PARTITION_SUBTYPE_APP_OTA_0, 
 "app0"
 );
 
 const esp_partition_t* app1 = esp_partition_find_first(
 ESP_PARTITION_TYPE_APP, 
 ESP_PARTITION_SUBTYPE_APP_OTA_1, 
 "app1"
 );
 
 if (app0 == nullptr) {
 HAL_LOG_E(TAG, "CRITICAL: App0 Partition MISSING!");
 _lastError = PartitionError::APP0_MISSING;
 return false;
 }
 
 if (app1 == nullptr) {
 HAL_LOG_E(TAG, "CRITICAL: App1 Partition MISSING!");
 _lastError = PartitionError::APP1_MISSING;
 return false;
 }
 
 HAL_LOG_I(TAG, " App0: 0x%X bytes (%.2f MB)", 
 app0->size, (float)app0->size / (1024 * 1024));
 HAL_LOG_I(TAG, " App1: 0x%X bytes (%.2f MB)", 
 app1->size, (float)app1->size / (1024 * 1024));
 
 if (app0->size != app1->size) {
 HAL_LOG_E(TAG, "CRITICAL: OTA Partition SIZE MISMATCH!");
 HAL_LOG_E(TAG, " App0: 0x%X, App1: 0x%X", app0->size, app1->size);
 HAL_LOG_E(TAG, " OTA requires identical partition sizes.");
 _lastError = PartitionError::APP_SIZE_MISMATCH;
 return false;
 }
 
 _appSlotSize = app0->size;
 
 const esp_partition_t* running = esp_ota_get_running_partition;
 if (running != nullptr) {
 HAL_LOG_I(TAG, " Running: %s @ 0x%X", running->label, running->address);
 }
 
 const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
 if (next != nullptr) {
 HAL_LOG_I(TAG, " Next OTA: %s @ 0x%X", next->label, next->address);
 }
 
 HAL_LOG_I(TAG, "PARTITION SCHEME VALIDATED SUCCESSFULLY");
 HAL_LOG_I(TAG, " NVS: %u KB (sufficient for %u bonds)",
 _nvsSize / 1024, _nvsSize / 2048);
 HAL_LOG_I(TAG, " App Slot: %.2f MB", (float)_appSlotSize / (1024 * 1024));
 
 _lastError = PartitionError::OK;
 _initialized = true;
 return true;
}

const char* StorageHAL::getErrorString(PartitionError error) {
 switch (error) {
 case PartitionError::OK:
 return "OK";
 case PartitionError::NVS_MISSING:
 return "NVS partition missing - reflash with correct partition table";
 case PartitionError::NVS_TOO_SMALL:
 return "NVS partition too small for bonding - increase to 24KB";
 case PartitionError::OTADATA_MISSING:
 return "OTA data partition missing - OTA updates will fail";
 case PartitionError::APP0_MISSING:
 return "App0 partition missing - invalid partition table";
 case PartitionError::APP1_MISSING:
 return "App1 partition missing - dual-bank OTA unavailable";
 case PartitionError::APP_SIZE_MISMATCH:
 return "App partitions different sizes - OTA will fail";
 case PartitionError::NVS_INIT_FAILED:
 return "NVS initialization failed after recovery attempt";
 case PartitionError::NVS_CORRUPTED:
 return "NVS was corrupted and has been reset";
 default:
 return "Unknown partition error";
 }
}


HalError StorageHAL::initNvs {
 HAL_LOG_I(TAG, "Initializing NVS Flash...");
 
 esp_err_t err = nvs_flash_init;
 
 if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
 HAL_LOG_W(TAG, "NVS corrupted or needs migration - erasing...");
 
 err = nvs_flash_erase;
 if (err != ESP_OK) {
 HAL_LOG_E(TAG, "NVS erase failed: %s", esp_err_to_name(err));
 _lastError = PartitionError::NVS_INIT_FAILED;
 return HalError::INIT_FAILED;
 }
 
 err = nvs_flash_init;
 if (err != ESP_OK) {
 HAL_LOG_E(TAG, "NVS re-init failed after erase: %s", esp_err_to_name(err));
 _lastError = PartitionError::NVS_INIT_FAILED;
 return HalError::INIT_FAILED;
 }
 
 _nvsRecovered = true;
 _lastError = PartitionError::NVS_CORRUPTED;
 HAL_LOG_W(TAG, "NVS recovered - all bonds and settings cleared!");
 } else if (err != ESP_OK) {
 HAL_LOG_E(TAG, "NVS init failed: %s", esp_err_to_name(err));
 _lastError = PartitionError::NVS_INIT_FAILED;
 return HalError::INIT_FAILED;
 }
 
 HAL_LOG_I(TAG, "NVS Flash initialized successfully%s", 
 _nvsRecovered ? " (recovered from corruption)" : "");
 
 return HalError::OK;
}

HalError StorageHAL::eraseNvs {
 HAL_LOG_W(TAG, "ERASING ALL NVS DATA (Factory Reset)");
 
 esp_err_t err = nvs_flash_erase;
 if (err != ESP_OK) {
 HAL_LOG_E(TAG, "NVS erase failed: %s", esp_err_to_name(err));
 return HalError::BUS_ERROR;
 }
 
 err = nvs_flash_init;
 if (err != ESP_OK) {
 HAL_LOG_E(TAG, "NVS re-init failed after erase: %s", esp_err_to_name(err));
 return HalError::INIT_FAILED;
 }
 
 HAL_LOG_I(TAG, "NVS erased and re-initialized");
 return HalError::OK;
}


void StorageHAL::getPartitionInfo(char* buffer, size_t bufferSize) const {
 const esp_partition_t* running = esp_ota_get_running_partition;
 const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
 
 if (running != nullptr && next != nullptr) {
 snprintf(buffer, bufferSize,
 "Running: %s@0x%X (%uKB), Next: %s@0x%X",
 running->label, running->address, running->size / 1024,
 next->label, next->address);
 } else if (running != nullptr) {
 snprintf(buffer, bufferSize,
 "Running: %s@0x%X (%uKB), Next: N/A",
 running->label, running->address, running->size / 1024);
 } else {
 snprintf(buffer, bufferSize, "Partition info unavailable");
 }
}


void StorageHAL::getDiagnostics(char* buffer, size_t bufferSize) const {
 snprintf(buffer, bufferSize,
 "Storage: nvs=%uKB, app=%uKB, err=0x%02X%s",
 _nvsSize / 1024,
 _appSlotSize / 1024,
 static_cast<uint8_t>(_lastError),
 _nvsRecovered ? " [NVS_RECOVERED]" : "");
}

}

