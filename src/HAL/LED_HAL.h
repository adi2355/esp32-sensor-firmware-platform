
#ifndef LED_HAL_H
#define LED_HAL_H

#include <Arduino.h>
#include "HAL_Base.h"

class LED_HAL;

enum class BlinkPattern : uint8_t {
 SOLID,
 SLOW_BLINK,
 FAST_BLINK,
 DOUBLE_BLINK,
 BREATHE,
 RAINBOW_BREATHE,
 OFF
};

class LED_HAL {
private:
 static LED_HAL* _instance;
 
 bool _initialized;
 BlinkPattern _currentPattern;
 uint32_t _currentColor;
 
 unsigned long _lastBlinkTime;
 bool _ledOn;
 uint8_t _blinkPhase;
 
 portMUX_TYPE _mux;
 
 LED_HAL;
 
 LED_HAL(const LED_HAL&) = delete;
 LED_HAL& operator=(const LED_HAL&) = delete;
 
 void writeColor(uint32_t color);
 
public:
 static LED_HAL* getInstance;
 
 HalError init;
 
 void setColor(uint32_t color);
 
 void setPattern(uint32_t color, BlinkPattern pattern);
 
 void off;
 
 void update;
 
 void flash(uint32_t color, uint16_t durationMs = 100);
 
 void prepareForSleep;
 
 bool isOn const { return _ledOn; }
 
 BlinkPattern getPattern const { return _currentPattern; }
 
 uint32_t getColor const { return _currentColor; }
};


inline LED_HAL* LED_HAL::getInstance {
 static LED_HAL instance;
 _instance = &instance;
 return _instance;
}

#endif
