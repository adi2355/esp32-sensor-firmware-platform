
#ifndef BOOT_RECOVERY_TRACKER_H
#define BOOT_RECOVERY_TRACKER_H

#include <stdint.h>
#include "esp_system.h"
#include "nvs.h"

class BootRecoveryTracker {
public:
 static BootRecoveryTracker& getInstance;

 bool init;
 bool isAvailable const;
 uint32_t getCounter const;

 uint32_t incrementIfEligible(esp_reset_reason_t reason);
 void resetCounter;

private:
 BootRecoveryTracker = default;

 bool shouldIncrement(esp_reset_reason_t reason) const;
 void persistCounter(const char* context);

 bool _initialized = false;
 bool _available = false;
 uint32_t _counter = 0;
 nvs_handle_t _handle = 0;
};

#endif
