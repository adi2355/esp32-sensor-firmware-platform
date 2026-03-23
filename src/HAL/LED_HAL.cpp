
#include "LED_HAL.h"
#include "driver/gpio.h"

#define FASTLED_RMT_MAX_CHANNELS 1
#include <FastLED.h>

static const char* TAG = "LED_HAL";

LED_HAL* LED_HAL::_instance = nullptr;


#define LED_GPIO_PIN 43
#define LED_TYPE WS2812B
#define LED_COLOR_ORDER GRB

static CRGB _leds[LEDConfig::NUM_LEDS];

static constexpr uint32_t SLOW_BLINK_PERIOD = 1000;
static constexpr uint32_t FAST_BLINK_PERIOD = 250;
static constexpr uint32_t DOUBLE_BLINK_ON = 100;
static constexpr uint32_t DOUBLE_BLINK_OFF = 100;
static constexpr uint32_t DOUBLE_BLINK_PAUSE = 500;


LED_HAL::LED_HAL 
 : _initialized(false)
 , _currentPattern(BlinkPattern::OFF)
 , _currentColor(0)
 , _lastBlinkTime(0)
 , _ledOn(false)
 , _blinkPhase(0)
 , _mux(portMUX_INITIALIZER_UNLOCKED)
{
}


HalError LED_HAL::init {
 if (_initialized) {
 return HalError::OK;
 }
 
 HAL_LOG_I(TAG, "Initializing FastLED on GPIO%d (WS2812B NeoPixel, D6=GPIO43)", LED_GPIO_PIN);
 
 gpio_hold_dis((gpio_num_t)LED_GPIO_PIN);
 HAL_LOG_D(TAG, "GPIO hold released for LED pin");
 
 FastLED.addLeds<LED_TYPE, LED_GPIO_PIN, LED_COLOR_ORDER>(_leds, LEDConfig::NUM_LEDS);
 
 FastLED.setBrightness(LEDConfig::BRIGHTNESS);
 
 _leds[0] = CRGB::Black;
 FastLED.show;
 
 _initialized = true;
 _currentPattern = BlinkPattern::OFF;
 _currentColor = 0;
 _ledOn = false;
 _lastBlinkTime = millis;
 _blinkPhase = 0;
 
 HAL_LOG_I(TAG, "LED HAL initialized successfully (FastLED WS2812B)");
 return HalError::OK;
}


void LED_HAL::writeColor(uint32_t color) {
 if (!_initialized) return;
 
 static uint32_t lastWrittenColor = 0xFFFFFFFF;
 
 if (color == lastWrittenColor) {
 return;
 }
 
 _leds[0] = CRGB(color);
 FastLED.show;
 
 lastWrittenColor = color;
 _ledOn = (color != 0);
}


void LED_HAL::setColor(uint32_t color) {
 if (!_initialized) {
 HAL_LOG_E(TAG, "setColor called before init");
 return;
 }
 
 portENTER_CRITICAL(&_mux);
 _currentPattern = (color != 0) ? BlinkPattern::SOLID : BlinkPattern::OFF;
 _currentColor = color;
 _lastBlinkTime = millis;
 _blinkPhase = 0;
 portEXIT_CRITICAL(&_mux);
 
 writeColor(color);
}

void LED_HAL::setPattern(uint32_t color, BlinkPattern pattern) {
 if (!_initialized) {
 HAL_LOG_E(TAG, "setPattern called before init");
 return;
 }
 
 portENTER_CRITICAL(&_mux);
 _currentPattern = pattern;
 _currentColor = (color != 0) ? color : 0xFFFFFF;
 _lastBlinkTime = millis;
 _blinkPhase = 0;
 portEXIT_CRITICAL(&_mux);
 
 if (pattern == BlinkPattern::SOLID) {
 writeColor(_currentColor);
 } else if (pattern == BlinkPattern::OFF) {
 writeColor(0);
 } else {
 writeColor(_currentColor);
 }
}

void LED_HAL::off {
 if (!_initialized) return;
 
 portENTER_CRITICAL(&_mux);
 _currentPattern = BlinkPattern::OFF;
 _currentColor = 0;
 _blinkPhase = 0;
 portEXIT_CRITICAL(&_mux);
 
 writeColor(0);
}

void LED_HAL::flash(uint32_t color, uint16_t durationMs) {
 if (!_initialized) return;
 
 BlinkPattern savedPattern;
 uint32_t savedColor;
 
 portENTER_CRITICAL(&_mux);
 savedPattern = _currentPattern;
 savedColor = _currentColor;
 portEXIT_CRITICAL(&_mux);
 
 writeColor(color);
 delay(durationMs);
 
 writeColor(0);
 
 portENTER_CRITICAL(&_mux);
 _currentPattern = savedPattern;
 _currentColor = savedColor;
 _lastBlinkTime = millis;
 _blinkPhase = 0;
 portEXIT_CRITICAL(&_mux);
}


void LED_HAL::update {
 if (!_initialized) return;
 
 unsigned long now = millis;
 
 BlinkPattern pattern;
 uint32_t color;
 unsigned long lastTime;
 uint8_t phase;
 
 portENTER_CRITICAL(&_mux);
 pattern = _currentPattern;
 color = _currentColor;
 lastTime = _lastBlinkTime;
 phase = _blinkPhase;
 portEXIT_CRITICAL(&_mux);
 
 if (pattern == BlinkPattern::SOLID || pattern == BlinkPattern::OFF) {
 return;
 }
 
 bool shouldBeOn = _ledOn;
 bool stateChanged = false;
 uint8_t newPhase = phase;
 
 switch (pattern) {
 case BlinkPattern::SLOW_BLINK: {
 if (now - lastTime >= SLOW_BLINK_PERIOD) {
 shouldBeOn = !_ledOn;
 stateChanged = true;
 }
 break;
 }
 
 case BlinkPattern::FAST_BLINK: {
 if (now - lastTime >= FAST_BLINK_PERIOD) {
 shouldBeOn = !_ledOn;
 stateChanged = true;
 }
 break;
 }
 
 case BlinkPattern::DOUBLE_BLINK: {
 
 uint32_t phaseDuration;
 switch (phase) {
 case 0: phaseDuration = DOUBLE_BLINK_ON; break;
 case 1: phaseDuration = DOUBLE_BLINK_OFF; break;
 case 2: phaseDuration = DOUBLE_BLINK_ON; break;
 case 3: phaseDuration = DOUBLE_BLINK_OFF; break;
 case 4: phaseDuration = DOUBLE_BLINK_PAUSE; break;
 default: phaseDuration = DOUBLE_BLINK_ON; break;
 }
 
 if (now - lastTime >= phaseDuration) {
 newPhase = (phase + 1) % 5;
 shouldBeOn = (newPhase == 0 || newPhase == 2);
 stateChanged = true;
 }
 break;
 }

 case BlinkPattern::BREATHE: {
 float t = (float)now / 2000.0f;
 float brightnessFactor = (exp(sin(t * PI)) - 0.36787944f) * 0.42545906f; 
 
 uint8_t r = (color >> 16) & 0xFF;
 uint8_t g = (color >> 8) & 0xFF;
 uint8_t b = color & 0xFF;
 
 uint32_t breathColor = 
 ((uint8_t)(r * brightnessFactor) << 16) |
 ((uint8_t)(g * brightnessFactor) << 8) |
 ((uint8_t)(b * brightnessFactor));
 
 writeColor(breathColor);
 return; 
 }

 case BlinkPattern::RAINBOW_BREATHE: {
 
 float t = (float)now / 2000.0f;
 float hue_t = (float)now / 5000.0f;
 
 float brightnessFactor = (exp(sin(t * PI)) - 0.36787944f) * 0.42545906f;
 
 uint8_t hue = (uint8_t)(hue_t * 255.0f);
 
 CHSV hsv(hue, 255, (uint8_t)(brightnessFactor * 255.0f));
 
 CRGB rgb;
 hsv2rgb_rainbow(hsv, rgb);
 
 _leds[0] = rgb;
 FastLED.show;
 _ledOn = (rgb.r > 0 || rgb.g > 0 || rgb.b > 0);
 return;
 }
 
 default:
 break;
 }
 
 if (stateChanged) {
 portENTER_CRITICAL(&_mux);
 _lastBlinkTime = now;
 _blinkPhase = newPhase;
 portEXIT_CRITICAL(&_mux);
 
 writeColor(shouldBeOn ? color : 0);
 }
}


void LED_HAL::prepareForSleep {
 if (!_initialized) {
 HAL_LOG_W(TAG, "prepareForSleep called before init - proceeding anyway");
 }
 
 HAL_LOG_I(TAG, "Preparing LED for deep sleep (enhanced)");
 
 _leds[0] = CRGB::Black;
 FastLED.show;
 
 delayMicroseconds(300);
 
 
 pinMode(LED_GPIO_PIN, OUTPUT);
 digitalWrite(LED_GPIO_PIN, LOW);
 
 delayMicroseconds(10);
 
 gpio_hold_en((gpio_num_t)LED_GPIO_PIN);
 
 HAL_LOG_I(TAG, "GPIO%d held LOW for deep sleep", LED_GPIO_PIN);
 
 portENTER_CRITICAL(&_mux);
 _currentPattern = BlinkPattern::OFF;
 _currentColor = 0;
 _ledOn = false;
 _blinkPhase = 0;
 portEXIT_CRITICAL(&_mux);
 
 HAL_LOG_I(TAG, "LED blackout complete, GPIO hold enabled");
}
