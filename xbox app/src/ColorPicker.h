#pragma once

#include <xtl.h>
#include "menu.h"

// Simple RGB triple for orb colors (0..1 floats)
struct OrbColor
{
    float innerR, innerG, innerB;
    float outerR, outerG, outerB;
};

// Call once at startup.
void ColorPicker_Init();

// Call once per frame to update the picker from analog sticks.
// dt is frame delta in seconds (you can just pass a fixed 1/60.0f for now).
// menu is read-only for now; later we can use it to gate when picker is active
// (e.g. only in CUSTOM mode or a COLOR submenu).
void ColorPicker_Update(const MenuState& menu, float dt);

// Get the current orb colors to use in DrawOrb.
// If color-picker is "idle", it still returns a sane default greenish orb.
void ColorPicker_GetOrbColor(OrbColor& out);
