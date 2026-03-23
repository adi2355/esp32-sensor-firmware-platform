
#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <Arduino.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "../Config.h"
#include "../Domain/ProtocolDefs.h"

class BLEManager;
class ProtocolHandler;


enum class OTAState : uint8_t {
 IDLE,
 RECEIVING,
 VALIDATING,
 COMMITTING,
 REBOOTING,
 FAILED
};

enum class OTAError : uint8_t {
 NONE = 0x00,
 LOW_BATTERY = 0x01,
 NO_PARTITION = 0x02,
 TOO_LARGE = 0x03,
 BEGIN_FAILED = 0x04,
 WRITE_FAILED = 0x05,
 SIZE_MISMATCH = 0x06,
 CRC_MISMATCH = 0x07,
 COMMIT_FAILED = 0x08,
 TIMEOUT = 0x09,
 INVALID_STATE = 0x0A,
 CHUNK_OUT_OF_ORDER = 0x0B,
 SELF_TEST_FAILED = 0x0C,
 VERSION_REJECTED = 0x0D
};


#pragma pack(push, 1)

struct FirmwareInfo {
 uint8_t major;
 uint8_t minor;
 uint8_t patch;
 uint8_t buildType;
 uint32_t buildNumber;
 uint32_t flashSize;
 uint8_t batteryPercent;
 uint8_t reserved[3];
};

#pragma pack(pop)

static_assert(sizeof(FirmwareInfo) == 16, "FirmwareInfo must be 16 bytes");


class OTAManager {
private:
 BLEManager* _bleManager;
 ProtocolHandler* _protocolHandler;
 
 OTAState _state;
 OTAError _lastError;
 bool _initialized;
 
 uint32_t _expectedSize;
 uint32_t _receivedSize;
 uint32_t _expectedCRC;
 uint32_t _runningCRC;
 uint16_t _expectedChunks;
 uint16_t _receivedChunks;
 uint8_t _newMajor;
 uint8_t _newMinor;
 uint8_t _newPatch;
 
 unsigned long _lastChunkTime;
 unsigned long _startTime;
 
 portMUX_TYPE _mux;
 
 bool checkBatteryLevel;
 
 const esp_partition_t* getNextPartition;
 
 void sendStatus(uint8_t status, OTAError error);
 
 void sendChunkAck(uint16_t chunkIndex, bool success);
 
 void updateCRC(const uint8_t* data, size_t length);
 
 bool validateVersion(uint8_t major, uint8_t minor, uint8_t patch);
 
public:
 OTAManager;
 
 bool init(BLEManager* bleManager, ProtocolHandler* protocolHandler);
 
 
 bool validateBootedFirmware;
 
 bool performSelfTest;
 
 
 void handleInfoRequest(uint16_t seqNum);
 
 bool handleStart(const uint8_t* payload, size_t length);
 
 bool handleData(uint16_t chunkIndex, const uint8_t* payload, size_t length);
 
 bool handleCommit;
 
 void handleAbort;
 
 
 OTAState getState const { return _state; }
 
 const char* getStateName const;
 
 OTAError getLastError const { return _lastError; }
 
 uint8_t getProgress const;
 
 bool isInProgress const { return _state == OTAState::RECEIVING; }
 
 uint16_t getReceivedChunks const { return _receivedChunks; }
 
 uint16_t getExpectedChunks const { return _expectedChunks; }
 
 
 void update;
 
 bool checkTimeout;
 
 
 void getDiagnostics(char* buffer, size_t bufferSize) const;
 
 void getPartitionInfo(char* buffer, size_t bufferSize) const;
};

#endif

