
#ifndef STORAGE_HAL_H
#define STORAGE_HAL_H

#include <Arduino.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "HAL_Base.h"
#include "../Config.h"

namespace HAL {

enum class PartitionError : uint8_t {
 OK = 0x00,
 NVS_MISSING = 0x01,
 NVS_TOO_SMALL = 0x02,
 OTADATA_MISSING = 0x03,
 APP0_MISSING = 0x04,
 APP1_MISSING = 0x05,
 APP_SIZE_MISMATCH = 0x06,
 NVS_INIT_FAILED = 0x07,
 NVS_CORRUPTED = 0x08
};

class StorageHAL {
private:
 static StorageHAL* _instance;
 
 bool _initialized;
 PartitionError _lastError;
 bool _nvsRecovered;
 
 uint32_t _nvsSize;
 uint32_t _appSlotSize;
 
 StorageHAL;
 
 StorageHAL(const StorageHAL&) = delete;
 StorageHAL& operator=(const StorageHAL&) = delete;
 
 static constexpr const char* TAG = "STORAGE_HAL";
 
public:
 static StorageHAL* getInstance;
 
 
 bool validatePartitions;
 
 PartitionError getLastError const { return _lastError; }
 
 static const char* getErrorString(PartitionError error);
 
 
 HalError initNvs;
 
 bool wasNvsRecovered const { return _nvsRecovered; }
 
 HalError eraseNvs;
 
 
 uint32_t getNvsSize const { return _nvsSize; }
 
 uint32_t getAppSlotSize const { return _appSlotSize; }
 
 void getPartitionInfo(char* buffer, size_t bufferSize) const;
 
 
 void getDiagnostics(char* buffer, size_t bufferSize) const;
 
 bool isReady const { return _initialized; }
};


inline StorageHAL* StorageHAL::getInstance {
 static StorageHAL instance;
 _instance = &instance;
 return _instance;
}

}

#endif

