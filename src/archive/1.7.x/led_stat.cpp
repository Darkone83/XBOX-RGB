#include "led_stat.h"
#include <Arduino.h>

// --- Pin config ---
#define RGB_PIN 21
#define RGB_BRIGHTNESS 75 // Reasonable for status; can tweak

// --- Internal state ---
static LedStatus currentStatus = LedStatus::Booting;
static unsigned long lastBlink = 0;
static bool ledOn = true;

extern "C" void neopixelWrite(uint8_t pin, uint8_t r, uint8_t g, uint8_t b);

// Set color using onboard RGB
static void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(RGB_PIN, r, g, b);
}

void LedStat::begin() {
    setStatus(LedStatus::Booting); // Solid white at boot
}

void LedStat::setStatus(LedStatus status) {
    currentStatus = status;
    ledOn = true;
    lastBlink = millis();

    switch (currentStatus) {
        case LedStatus::Booting:        setLedColor(RGB_BRIGHTNESS, RGB_BRIGHTNESS, RGB_BRIGHTNESS); break; // White
        case LedStatus::WifiConnected:  setLedColor(0, RGB_BRIGHTNESS, 0); break;   // Green
        case LedStatus::WifiFailed:     setLedColor(RGB_BRIGHTNESS, 0, 0); break;   // Red
        case LedStatus::Portal:         setLedColor(128, 0, 128); break;            // Purple
        // Removed UDP/other non-relevant statuses
    }
}

// Call this from your main loop!
void LedStat::loop() {
    unsigned long now = millis();

    switch (currentStatus) {
        case LedStatus::Portal: // Blinking purple
            if (now - lastBlink > 400) {
                ledOn = !ledOn;
                setLedColor(ledOn ? 16 : 0, 0, ledOn ? 16 : 0); // Purple/off
                lastBlink = now;
            }
            break;
        case LedStatus::Booting:        // Solid white
        case LedStatus::WifiConnected:  // Solid green
        case LedStatus::WifiFailed:     // Solid red
        default:
            // Solid colors, nothing to do
            break;
    }
}
