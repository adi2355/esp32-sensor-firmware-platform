
#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include <Arduino.h>


#pragma pack(push, 1)

struct UserConfig {
 uint8_t version;
 uint8_t sensitivity;
 uint8_t ledBrightness;
 uint8_t reserved;
 uint32_t signature;
 uint32_t lastKnownEpoch;
 uint32_t crc32;
};

#pragma pack(pop)

static_assert(sizeof(UserConfig) == 16, "UserConfig must be exactly 16 bytes");


namespace UserConfigDefaults {
 constexpr uint8_t VERSION = 3;
 
 constexpr uint8_t SENSITIVITY = 50;
 
 constexpr uint8_t LED_BRIGHTNESS = 128;
 
 constexpr uint8_t RESERVED = 0;
 
 
 constexpr uint32_t LAST_KNOWN_EPOCH = 0;
}


namespace UserConfigNvs {
 constexpr const char* NAMESPACE = "userconfig";
 
 constexpr const char* KEY_DATA = "data";
 
 constexpr size_t CRC_DATA_SIZE = sizeof(UserConfig) - sizeof(uint32_t);
}


inline void initUserConfigDefaults(UserConfig* config) {
 if (config == nullptr) return;
 
 config->version = UserConfigDefaults::VERSION;
 config->sensitivity = UserConfigDefaults::SENSITIVITY;
 config->ledBrightness = UserConfigDefaults::LED_BRIGHTNESS;
 config->reserved = UserConfigDefaults::RESERVED;
 
 config->signature = esp_random;
 
 config->lastKnownEpoch = UserConfigDefaults::LAST_KNOWN_EPOCH;
 
 config->crc32 = 0;
}

inline uint8_t validateSensitivity(uint8_t sensitivity) {
 return (sensitivity > 100) ? 100 : sensitivity;
}

inline float sensitivityToMultiplier(uint8_t sensitivity) {
 // [REMOVED] Piecewise sensitivity mapping curve
 return 1.0f;
}

#endif

