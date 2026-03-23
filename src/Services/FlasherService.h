
#ifndef FLASHER_SERVICE_H
#define FLASHER_SERVICE_H

#include <Arduino.h>
#include <stdint.h>

enum class FlasherPhase : uint8_t {
 INIT = 0,
 BATTERY_CHECK,
 WIFI_START,
 READY,
 ERASING,
 RECEIVING,
 VERIFYING,
 COMPLETE,
 SUCCESS,
 ERROR
};

const char* flasherPhaseToString(FlasherPhase phase);

class FlasherService {
public:
 static void run;
 
private:
 static uint32_t readBatteryVoltage;
 
 static void errorAndReboot(uint8_t errorCode, const char* message);
 
 static void setLedColor(uint32_t color);
 
 static void flashLed(uint8_t count, uint32_t color);
};

#endif
