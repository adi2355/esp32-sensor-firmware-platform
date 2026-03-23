
#ifndef PROTOCOL_DEFS_H
#define PROTOCOL_DEFS_H

#include <Arduino.h>

namespace Protocol {
 constexpr uint8_t SOF = 0xA5;
 
 constexpr uint8_t VERSION = 0x01;
 
 constexpr size_t HEADER_SIZE = 7;
 
 constexpr size_t CRC_SIZE = 2;
 
 constexpr size_t MAX_PAYLOAD_SIZE = 500;
 
 constexpr size_t MIN_FRAME_SIZE = HEADER_SIZE + CRC_SIZE;
 
 constexpr uint8_t MAX_PENDING_MESSAGES = 64;
 
 
 constexpr uint8_t INVALID_ACK_THRESHOLD = 10;
 
 constexpr uint32_t SECURITY_RESET_WINDOW_MS = 60000;
 
 
 constexpr uint8_t MAX_DETECTIONS_PER_WINDOW = 5;
 
 constexpr uint32_t DETECTION_RATE_WINDOW_MS = 1000;
}

enum class MessageType : uint8_t {
 
 MSG_HELLO = 0x00,
 
 MSG_HELLO_ACK = 0x01,
 
 MSG_ACK = 0x02,
 
 MSG_NACK = 0x03,
 
 MSG_HEARTBEAT = 0x04,
 
 MSG_PING = 0x05,
 
 MSG_PONG = 0x06,
 
 MSG_SLEEP = 0x07,
 
 
 MSG_DETECTION_EVENT = 0x10,
 
 MSG_BATTERY_STATUS = 0x11,
 
 MSG_DEVICE_STATUS = 0x12,
 
 MSG_SYNC_REQUEST = 0x13,
 
 MSG_SYNC_DATA = 0x14,
 
 
 MSG_SET_CONFIG = 0x20,
 
 MSG_GET_CONFIG = 0x21,
 
 MSG_CONFIG_DATA = 0x22,
 
 MSG_CALIBRATE = 0x23,
 
 MSG_TIME_SYNC = 0x24,
 
 MSG_SET_COLORS = 0x25,
 
 MSG_SET_WIFI = 0x26,
 
 
 MSG_OTA_INFO = 0x30,
 
 MSG_OTA_INFO_RESP = 0x31,
 
 MSG_OTA_START = 0x32,
 
 MSG_OTA_DATA = 0x33,
 
 MSG_OTA_DATA_ACK = 0x34,
 
 MSG_OTA_COMMIT = 0x35,
 
 MSG_OTA_ABORT = 0x36,
 
 MSG_OTA_STATUS = 0x37,
 
 MSG_ENTER_OTA_MODE = 0x38,
 
 
 MSG_LOG_REQUEST = 0x40,
 
 MSG_LOG_DATA = 0x41,
 
 MSG_FACTORY_RESET = 0x42,
 
 MSG_PAIRING_MODE = 0x43,
 
 MSG_CLEAR_BONDS = 0x44
};

enum class ErrorCode : uint8_t {
 ERR_NONE = 0x00,
 ERR_CRC_MISMATCH = 0x01,
 ERR_SEQ_OUT_OF_ORDER = 0x02,
 ERR_INVALID_LENGTH = 0x03,
 ERR_UNKNOWN_MSG = 0x04,
 ERR_INVALID_STATE = 0x05,
 ERR_BUSY = 0x06,
 ERR_FLASH_WRITE = 0x07,
 ERR_LOW_BATTERY = 0x08,
 ERR_NOT_BONDED = 0x09,
 ERR_INVALID_PAYLOAD = 0x0A,
 ERR_TIMEOUT = 0x0B,
 ERR_OTA_FAILED = 0x0C,
 ERR_CALIBRATION = 0x0D,
 ERR_SENSOR = 0x0E,
 ERR_INTERNAL = 0xFF
};

#pragma pack(push, 1)

struct MessageHeader {
 uint8_t sof;
 uint8_t msgType;
 uint8_t version;
 uint16_t seqNum;
 uint16_t length;
 
 inline bool isValid const {
 return (sof == Protocol::SOF) && 
 (version == Protocol::VERSION) &&
 (length <= Protocol::MAX_PAYLOAD_SIZE);
 }
};

#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 7, "MessageHeader must be exactly 7 bytes");

#pragma pack(push, 1)

struct AckPayload {
 uint16_t ackedSeqNum;
};

#pragma pack(pop)

static_assert(sizeof(AckPayload) == 2, "AckPayload must be exactly 2 bytes");

#pragma pack(push, 1)

struct NackPayload {
 uint16_t nackedSeqNum;
 uint8_t errorCode;
};

#pragma pack(pop)

static_assert(sizeof(NackPayload) == 3, "NackPayload must be exactly 3 bytes");

#pragma pack(push, 1)

struct HelloAckPayload {
 uint8_t fwMajor;
 uint8_t fwMinor;
 uint8_t fwPatch;
 uint8_t buildType;
 uint32_t buildNumber;
 uint8_t batteryPercent;
 uint8_t isCharging;
 uint32_t lastEventId;
 uint32_t currentMillis;
 uint8_t bondedDevices;
 uint8_t sensitivity;
};

#pragma pack(pop)

static_assert(sizeof(HelloAckPayload) == 36, "HelloAckPayload must be exactly 36 bytes");

#pragma pack(push, 1)

struct BatteryStatusPayload {
 uint8_t percent;
 uint8_t isCharging;
 uint16_t voltageRaw;
};

#pragma pack(pop)

static_assert(sizeof(BatteryStatusPayload) == 4, "BatteryStatusPayload must be exactly 4 bytes");

#pragma pack(push, 1)

struct OtaStartPayload {
 uint32_t totalSize;
 uint32_t expectedCrc;
 uint8_t newMajor;
 uint8_t newMinor;
 uint8_t newPatch;
 uint16_t totalChunks;
 uint8_t reserved;
};

#pragma pack(pop)

static_assert(sizeof(OtaStartPayload) == 14, "OtaStartPayload must be exactly 14 bytes");

#pragma pack(push, 1)

struct OtaDataHeader {
 uint16_t chunkIndex;
};

#pragma pack(pop)

static_assert(sizeof(OtaDataHeader) == 2, "OtaDataHeader must be exactly 2 bytes");

#pragma pack(push, 1)

struct TimeSyncPayload {
 uint32_t epochSeconds;
 uint16_t tzOffsetMins;
};

#pragma pack(pop)

static_assert(sizeof(TimeSyncPayload) == 6, "TimeSyncPayload must be exactly 6 bytes");

#pragma pack(push, 1)

struct ConfigDataPayload {
 uint8_t sensitivity;
 uint8_t ledBrightness;
 uint8_t reserved[2];
};

#pragma pack(pop)

static_assert(sizeof(ConfigDataPayload) == 4, "ConfigDataPayload must be exactly 4 bytes");

#pragma pack(push, 1)

struct SetConfigPayload {
 uint8_t configId;
 uint8_t value;
 uint8_t reserved[2];
};

#pragma pack(pop)

static_assert(sizeof(SetConfigPayload) == 4, "SetConfigPayload must be exactly 4 bytes");

namespace ConfigId {
 constexpr uint8_t SENSITIVITY = 0x01;
 constexpr uint8_t LED_BRIGHTNESS = 0x02;
}

namespace WifiPayload {
 constexpr size_t SSID_MAX_LENGTH = 32;
 constexpr size_t PASS_MAX_LENGTH = 64;
 constexpr size_t MAX_PAYLOAD_SIZE = 1 + SSID_MAX_LENGTH + 1 + PASS_MAX_LENGTH;
}

inline uint16_t calculateCRC16(const uint8_t* data, size_t length) {
 uint16_t crc = 0xFFFF;
 for (size_t i = 0; i < length; i++) {
 crc ^= ((uint16_t)data[i]) << 8;
 for (uint8_t j = 0; j < 8; j++) {
 if (crc & 0x8000) {
 crc = (crc << 1) ^ 0x1021;
 } else {
 crc <<= 1;
 }
 }
 }
 return crc;
}

inline uint16_t calculateCRC16WithTimestamp(const uint8_t* data, size_t length, uint32_t timestamp) {
 uint16_t crc = calculateCRC16(data, length);
 
 
 crc ^= (uint16_t)(timestamp & 0xFFFF);
 
 uint16_t upper = (uint16_t)((timestamp >> 16) & 0xFFFF);
 crc ^= upper;
 
 crc = (crc << 1) | (crc >> 15);
 crc ^= (uint16_t)(timestamp & 0xFF);
 
 return crc;
}

inline uint32_t calculateCRC32(const uint8_t* data, size_t length) {
 uint32_t crc = 0xFFFFFFFF;
 for (size_t i = 0; i < length; i++) {
 crc ^= data[i];
 for (uint8_t j = 0; j < 8; j++) {
 crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
 }
 }
 return ~crc;
}

inline uint32_t updateCRC32(uint32_t crc, const uint8_t* data, size_t length) {
 for (size_t i = 0; i < length; i++) {
 crc ^= data[i];
 for (uint8_t j = 0; j < 8; j++) {
 crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
 }
 }
 return crc;
}

#endif

