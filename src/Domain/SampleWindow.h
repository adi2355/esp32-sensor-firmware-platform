
#ifndef SAMPLE_WINDOW_H
#define SAMPLE_WINDOW_H

#include <Arduino.h>
#include "../Config.h"

enum class GestureLabel : uint8_t {
 IDLE = 0,
 APPROACH = 1,
 HOVER = 2,
 QUICK_PASS = 3,
 COVER_HOLD = 4,
 CONFIRMED_EVENT = 5,
 SURFACE_REFLECTION = 6,
 NOISE = 7,
 UNKNOWN = 8,
 UNLABELED = 254,
 INVALID = 255
};

enum class LoggerState : uint8_t {
 IDLE = 0,
 ARMED = 1,
 RECORDING = 2,
 ERROR = 3
};

#pragma pack(push, 1)

struct SampleRecord {
 uint16_t proximity;
 uint16_t dt_ms;
};

#pragma pack(pop)

static_assert(sizeof(SampleRecord) == 4, "SampleRecord must be exactly 4 bytes");

#pragma pack(push, 1)

struct WindowHeader {
 uint16_t magic;
 uint8_t version;
 uint8_t sampleCount;
 uint16_t sessionId;
 uint16_t windowId;

 uint8_t label;
 uint8_t reserved0;

 uint32_t startUptimeMs;
 uint32_t startEpoch;

 uint16_t wakeThreshold;
 uint16_t detectionThreshold;
 uint16_t criticalDetectionThreshold;

 uint8_t sensitivity;
 uint8_t flags;
 uint8_t bootCount;
 uint8_t reserved1;

 uint32_t crc32;


 static constexpr uint8_t FLAG_WARMUP_ACTIVE = 0x01;
 static constexpr uint8_t FLAG_BLE_CONNECTED = 0x02;
 static constexpr uint8_t FLAG_USB_POWERED = 0x04;
 static constexpr uint8_t FLAG_CALIBRATION_VALID = 0x08;
};

#pragma pack(pop)

static_assert(sizeof(WindowHeader) == 32, "WindowHeader must be exactly 32 bytes");

#pragma pack(push, 1)

struct SampleWindow {
 WindowHeader header;
 SampleRecord samples[MLConfig::WINDOW_SIZE];

 uint32_t calculateCRC const;

 bool isValid const;

 GestureLabel getLabel const {
 return static_cast<GestureLabel>(header.label);
 }

 static constexpr size_t serializedSize {
 return sizeof(SampleWindow);
 }
};

#pragma pack(pop)

static_assert(sizeof(SampleWindow) == 32 + (4 * MLConfig::WINDOW_SIZE),
 "SampleWindow size mismatch");

#pragma pack(push, 1)

struct InferenceResult {
 uint8_t predictedLabel;
 uint8_t confidence;
 uint16_t latencyUs;
 uint32_t timestampMs;

 uint8_t globalConfidence;
 uint8_t localConfidence;
 uint8_t adaptationQuality;
 uint8_t flags;

 float rawGlobalLogit;
 float rawLocalDistance;

 GestureLabel getLabel const {
 return static_cast<GestureLabel>(predictedLabel);
 }

 static constexpr uint8_t FLAG_PERSONALIZED = 0x01;
 static constexpr uint8_t FLAG_BELOW_THRESHOLD = 0x02;
 static constexpr uint8_t FLAG_CALIBRATED = 0x04;
};

#pragma pack(pop)

static_assert(sizeof(InferenceResult) == 20, "InferenceResult must be exactly 20 bytes");

enum class LoggerCmd : uint8_t {
 WRITE_WINDOW,
 START_SESSION,
 STOP_SESSION,
 DELETE_ALL,
 GET_STATUS
};

struct LoggerMessage {
 LoggerCmd cmd;
 uint8_t label;
 uint16_t reserved;
};

static_assert(sizeof(LoggerMessage) == 4, "LoggerMessage must be exactly 4 bytes");

inline const char* gestureLabelToString(GestureLabel label) {
 switch (label) {
 case GestureLabel::IDLE: return "idle";
 case GestureLabel::APPROACH: return "approach";
 case GestureLabel::HOVER: return "hover";
 case GestureLabel::QUICK_PASS: return "quick_pass";
 case GestureLabel::COVER_HOLD: return "cover_hold";
 case GestureLabel::CONFIRMED_EVENT: return "confirmed_event";
 case GestureLabel::SURFACE_REFLECTION: return "surface_reflection";
 case GestureLabel::NOISE: return "noise";
 case GestureLabel::UNKNOWN: return "unknown";
 case GestureLabel::UNLABELED: return "unlabeled";
 case GestureLabel::INVALID: return "invalid";
 default: return "unrecognized";
 }
}

inline GestureLabel stringToGestureLabel(const char* str) {
 if (!str) return GestureLabel::INVALID;
 if (strcasecmp(str, "idle") == 0) return GestureLabel::IDLE;
 if (strcasecmp(str, "approach") == 0) return GestureLabel::APPROACH;
 if (strcasecmp(str, "hover") == 0) return GestureLabel::HOVER;
 if (strcasecmp(str, "quick_pass") == 0) return GestureLabel::QUICK_PASS;
 if (strcasecmp(str, "cover_hold") == 0) return GestureLabel::COVER_HOLD;
 if (strcasecmp(str, "confirmed_event") == 0) return GestureLabel::CONFIRMED_EVENT;
 if (strcasecmp(str, "surface_reflection") == 0) return GestureLabel::SURFACE_REFLECTION;
 if (strcasecmp(str, "noise") == 0) return GestureLabel::NOISE;
 if (strcasecmp(str, "unknown") == 0) return GestureLabel::UNKNOWN;
 if (strcasecmp(str, "unlabeled") == 0) return GestureLabel::UNLABELED;
 return GestureLabel::INVALID;
}

#endif
