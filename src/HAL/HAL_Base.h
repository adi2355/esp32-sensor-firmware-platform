
#ifndef HAL_BASE_H
#define HAL_BASE_H

#include <Arduino.h>
#include "esp_log.h"
#include "../Config.h"

enum class HalError : uint8_t {
 OK = 0,
 NOT_INITIALIZED,
 INIT_FAILED,
 TIMEOUT,
 INVALID_PARAM,
 BUS_ERROR,
 DEVICE_NOT_FOUND,
 BUSY,
 HARDWARE_FAULT,
 RESOURCE_EXHAUSTED,
 INTEGRITY_ERROR
};

template<typename T>
struct HalResult {
 T value;
 HalError error;
 
 inline bool ok const { return error == HalError::OK; }
 inline bool failed const { return error != HalError::OK; }
 
 static HalResult success(T val) {
 return HalResult{val, HalError::OK};
 }
 
 static HalResult failure(HalError err) {
 return HalResult{T{}, err};
 }
};


#define HAL_ENTER_CRITICAL portENTER_CRITICAL(&_halMux)
#define HAL_EXIT_CRITICAL portEXIT_CRITICAL(&_halMux)

extern portMUX_TYPE _halMux;


#define HAL_LOG_E(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define HAL_LOG_W(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define HAL_LOG_I(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define HAL_LOG_D(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)

namespace HAL {
 uint32_t getSystemUptime;
}

#endif

