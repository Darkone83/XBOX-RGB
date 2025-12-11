#include "ColorPicker.h"
#include "input.h"
#include <math.h>

static float g_hue = 120.0f;  // degrees, 0..360 (start near green)
static float g_sat = 0.9f;    // 0..1
static float g_value = 0.9f;    // 0..1

// Simple smoothing so sticks don't jitter the color too hard.
static float g_hueVel = 0.0f;
static float g_valueVel = 0.0f;

// Tiny helper: clamp
static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// HSV → RGB, all in 0..1 space
static void HSVtoRGB(float hDeg, float s, float v,
    float& r, float& g, float& b)
{
    if (s <= 0.0f) {
        r = g = b = v;
        return;
    }

    float h = fmodf(hDeg, 360.0f);
    if (h < 0.0f) h += 360.0f;

    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;

    float rr = 0.0f, gg = 0.0f, bb = 0.0f;

    if (h < 60.0f) { rr = c; gg = x; bb = 0.0f; }
    else if (h < 120.0f) { rr = x; gg = c; bb = 0.0f; }
    else if (h < 180.0f) { rr = 0.0f; gg = c; bb = x; }
    else if (h < 240.0f) { rr = 0.0f; gg = x; bb = c; }
    else if (h < 300.0f) { rr = x; gg = 0.0f; bb = c; }
    else { rr = c; gg = 0.0f; bb = x; }

    r = rr + m;
    g = gg + m;
    b = bb + m;
}

void ColorPicker_Init()
{
    g_hue = 120.0f;
    g_sat = 0.9f;
    g_value = 0.9f;
    g_hueVel = 0.0f;
    g_valueVel = 0.0f;
}

// Core idea:
//  - Left stick X controls hue sweep
//  - Left stick Y controls value (up = brighter, down = darker)
//  - We can later gate this on menu.uiState / mode if we want.
void ColorPicker_Update(const MenuState& menu, float dt)
{
    (void)menu; // unused for now; kept for future gating

    int lx = 0, ly = 0, rx = 0, ry = 0;
    GetSticks(lx, ly, rx, ry);

    // Normalize from -32768..32767 to -1..1
    const float invMax = 1.0f / 32768.0f;
    float nx = (float)lx * invMax;
    float ny = (float)ly * invMax;

    // Deadzone to avoid drift
    const float dead = 0.15f;
    if (fabsf(nx) < dead) nx = 0.0f;
    if (fabsf(ny) < dead) ny = 0.0f;

    // Sensitivity scales: tweak to taste
    const float hueSpeed = 180.0f;  // degrees per second at full deflection
    const float valueSpeed = 0.6f;    // value per second at full deflection

    // Integrate with a bit of smoothing
    float targetHueVel = nx * hueSpeed;
    float targetValueVel = -ny * valueSpeed; // up = increase value

    // Simple lerp smoothing on velocities
    const float smooth = 0.25f;
    g_hueVel = g_hueVel + (targetHueVel - g_hueVel) * smooth;
    g_valueVel = g_valueVel + (targetValueVel - g_valueVel) * smooth;

    g_hue += g_hueVel * dt;
    g_value += g_valueVel * dt;

    // Wrap / clamp
    if (g_hue >= 360.0f) g_hue -= 360.0f;
    if (g_hue < 0.0f)    g_hue += 360.0f;
    g_value = clampf(g_value, 0.05f, 1.0f);   // keep a bit above black

    // Optionally we could let right-stick X drive saturation later:
    // float ns = (float)rx * invMax;
    // if (fabsf(ns) < dead) ns = 0.0f;
    // g_sat = clampf(0.5f + ns * 0.5f, 0.2f, 1.0f);
}

void ColorPicker_GetOrbColor(OrbColor& out)
{
    float r, g, b;
    HSVtoRGB(g_hue, g_sat, g_value, r, g, b);

    // Inner is full, outer is dimmer for a gradient ring.
    out.innerR = r;
    out.innerG = g;
    out.innerB = b;

    const float outerScale = 0.4f;
    out.outerR = r * outerScale;
    out.outerG = g * outerScale;
    out.outerB = b * outerScale;
}
