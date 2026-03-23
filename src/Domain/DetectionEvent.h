
#ifndef DETECTION_EVENT_H
#define DETECTION_EVENT_H

#include <Arduino.h>

#pragma pack(push, 1)

struct DetectionEvent {
 
 uint32_t id;
 
 
 uint32_t timestampMs;
 
 uint32_t bootCount;
 
 
 uint16_t durationMs;
 
 
 uint32_t timestamp;
 
 
 uint8_t flags;
 
 uint8_t reserved[5];
 
 
 inline bool isSynced const {
 return (flags & 0x01) != 0;
 }
 
 inline void setSynced {
 flags |= 0x01;
 }
 
 inline void clearSynced {
 flags &= ~0x01;
 }
 
 inline bool wasOverflow const {
 return (flags & 0x02) != 0;
 }
 
 inline void setOverflow {
 flags |= 0x02;
 }
 
 inline bool isTimeEstimated const {
 return (flags & 0x04) != 0;
 }
 
 inline void setTimeEstimated {
 flags |= 0x04;
 }
 
 inline bool isTimeValid const {
 return timestamp != 0;
 }
 
 inline void clearFlags {
 flags = 0;
 }
};

#pragma pack(pop)

static_assert(sizeof(DetectionEvent) == 24, "DetectionEvent must be exactly 24 bytes");

#pragma pack(push, 1)

struct CalibrationData {
 uint16_t wakeThresholdLow;
 uint16_t detectionThresholdLow;
 uint16_t criticalDetectionThreshold;
 uint32_t crc32;
};

#pragma pack(pop)

static_assert(sizeof(CalibrationData) == 10, "CalibrationData must be exactly 10 bytes");

#pragma pack(push, 1)

struct TimeSyncData {
 uint32_t syncedEpoch;
 uint32_t syncedMillis;
 uint8_t isValid;
 uint8_t reserved[3];
};

#pragma pack(pop)

static_assert(sizeof(TimeSyncData) == 12, "TimeSyncData must be exactly 12 bytes");

#endif

