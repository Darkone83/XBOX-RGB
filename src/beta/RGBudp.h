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

} // namespace RGBCtrlUDP
