#pragma once
#include <Arduino.h>

namespace RGBCtrlUDP {

// Start the UDP control server.
//  - port: UDP port to listen on (default 7777)
//  - psk : optional pre-shared key; if non-empty, clients must send {"key":"..."}.
void begin(uint16_t port = 7777, const char* psk = nullptr);

// Poll for packets; call this often from loop().
void loop();

// Optionally broadcast a discovery becon once (useful after WiFi connects).
void sendDiscovery();

// NEW: Process queued heavy ops (apply preview/save/counts/reset) with a time budget (µs).
void processPending(uint32_t budget_us = 1500);

// Ask UDP code to avoid heavy JSON work for at least `dur_us` microseconds.
void enterSmbusQuietUs(uint32_t dur_us);

} // namespace RGBCtrlUDP
