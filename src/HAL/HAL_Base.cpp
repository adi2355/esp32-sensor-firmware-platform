
#include "HAL_Base.h"

portMUX_TYPE _halMux = portMUX_INITIALIZER_UNLOCKED;


#include <sys/time.h>

namespace HAL {
 uint32_t getSystemUptime {
 struct timeval now;
 gettimeofday(&now, NULL);
 
 return (uint32_t)((uint32_t)now.tv_sec * 1000UL + (uint32_t)(now.tv_usec / 1000));
 }
}

