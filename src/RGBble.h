#pragma once

#include <Arduino.h>

namespace RGBble {

// Initialize BLE (call from setup())
void begin();

// Periodic tasks (call from loop())
void loop();

} // namespace RGBble
