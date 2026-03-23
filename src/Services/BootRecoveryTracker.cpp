
#include "BootRecoveryTracker.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char* TAG = "BOOT_RECOVERY";
static const char* NVS_NAMESPACE = "bootrec";
static const char* KEY_COUNTER = "counter";

BootRecoveryTracker& BootRecoveryTracker::getInstance {
 static BootRecoveryTracker instance;
 return instance;
}

bool BootRecoveryTracker::init {
 if (_initialized) {
 return _available;
 }

 _initialized = true;

 esp_err_t err = nvs_flash_init;
 if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
 ESP_LOGW(TAG, "NVS requires recovery (%s). Boot recovery persistence disabled.",
 esp_err_to_name(err));
 return false;
 }
 if (err != ESP_OK) {
 ESP_LOGW(TAG, "NVS init failed (%s). Boot recovery persistence disabled.",
 esp_err_to_name(err));
 return false;
 }

 err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &_handle);
 if (err != ESP_OK) {
 ESP_LOGW(TAG, "NVS open failed (%s). Boot recovery persistence disabled.",
 esp_err_to_name(err));
 return false;
 }

 uint32_t counter = 0;
 err = nvs_get_u32(_handle, KEY_COUNTER, &counter);
 if (err == ESP_ERR_NVS_NOT_FOUND) {
 counter = 0;
 } else if (err != ESP_OK) {
 ESP_LOGW(TAG, "NVS read failed (%s). Boot recovery persistence disabled.",
 esp_err_to_name(err));
 return false;
 }

 _counter = counter;
 _available = true;

 ESP_LOGI(TAG, "Boot recovery counter loaded from NVS: %lu", _counter);
 return true;
}

bool BootRecoveryTracker::isAvailable const {
 return _available;
}

uint32_t BootRecoveryTracker::getCounter const {
 return _counter;
}

bool BootRecoveryTracker::shouldIncrement(esp_reset_reason_t reason) const {
 return (reason == ESP_RST_POWERON || reason == ESP_RST_SW);
}

uint32_t BootRecoveryTracker::incrementIfEligible(esp_reset_reason_t reason) {
 if (!_available) {
 return _counter;
 }

 if (!shouldIncrement(reason)) {
 return _counter;
 }

 _counter++;
 persistCounter("increment");
 return _counter;
}

void BootRecoveryTracker::resetCounter {
 if (!_available) {
 _counter = 0;
 return;
 }

 if (_counter == 0) {
 return;
 }

 _counter = 0;
 persistCounter("reset");
}

void BootRecoveryTracker::persistCounter(const char* context) {
 if (!_available) {
 return;
 }

 esp_err_t err = nvs_set_u32(_handle, KEY_COUNTER, _counter);
 if (err != ESP_OK) {
 ESP_LOGW(TAG, "NVS write failed (%s) during %s", esp_err_to_name(err), context);
 return;
 }

 err = nvs_commit(_handle);
 if (err != ESP_OK) {
 ESP_LOGW(TAG, "NVS commit failed (%s) during %s", esp_err_to_name(err), context);
 return;
 }

 ESP_LOGI(TAG, "Boot recovery counter %s: %lu", context, _counter);
}
