#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>

struct RGBsmbusPins {
  uint8_t ch5;   // WS2812 data pin for CH5 (CPU temp bar)
  uint8_t ch6;   // WS2812 data pin for CH6 (Fan speed bar)
  uint8_t sda;   // XSDA pin to Xbox
  uint8_t scl;   // XSCL pin to Xbox
};

namespace RGBsmbus {

// Initialize with pins and LED counts (max 10 per channel).
void begin(const RGBsmbusPins& pins, uint8_t ch5Count = 10, uint8_t ch6Count = 10);

// Call frequently in loop(); handles SMBus polling & LED updates.
void loop();

// Optional: force an immediate SMBus poll & redraw.
void refreshNow();

// Runtime enables
void setCpuEnabled(bool en);
void setFanEnabled(bool en);
bool cpuEnabled();
bool fanEnabled();

// Tiny API for the UI (mount wherever you like; e.g., "/config/smbus")
// GET  <base>/api/flags   -> {"cpu":bool,"fan":bool}
// POST <base>/api/flags   body {"cpu":bool,"fan":bool}
void attachWeb(AsyncWebServer& server, const char* basePath = "/config/smbus");

} // namespace RGBsmbus
