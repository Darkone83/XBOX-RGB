#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// ---- Public API -------------------------------------------------------------
struct RGBCtrlPins {
  uint8_t ch1, ch2, ch3, ch4; // WS2812 data pins for CH1..CH4
};

namespace RGBCtrl {

// Call once in setup() after your pins are known.
void begin(const RGBCtrlPins& pins);

// Registers the web UI and JSON API on your existing server (WiFiMgr).
// Default mount is /config  → page + /api/* (ledconfig/preview/save/reset).
void attachWeb(AsyncWebServer& server, const char* basePath = "/config");
// Convenience overload that grabs the server from WiFiMgr::getServer().
void attachWeb(const char* basePath = "/config");

// Call every loop() to render animations.
void loop();

// Optional helpers (used rarely, but exposed for convenience)
void setCounts(uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4); // 0..50 each
void forceSave();   // persist current config to NVS
void forceLoad();   // reload config from NVS, re-applies

// SMBus gating flags (so your RGBsmbus module can check them)
bool smbusCpuEnabled();
bool smbusFanEnabled();

} // namespace RGBCtrl
