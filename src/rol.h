#pragma once
#include <Arduino.h>

namespace ROL {

// Initialize the CH7 mirror ring.
// Defaults: GPIO 10, 20 LEDs.
void begin(uint8_t pin = 10, uint16_t count = 20);

// Call every loop() to mirror RGBCtrl’s current animation.
void loop();

// Optional kill switch if you ever need to blank CH7 only (independent of RGBCtrl::masterOff).
void setEnabled(bool on);

// Feed external RGB data to the mirror ring (used by Kratos passthrough)
void kratosFeed(const uint8_t* rgb, uint16_t count);

} // namespace ROL
