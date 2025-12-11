#pragma once

#include <xtl.h>
#include "input.h"

// -----------------------------------------------------------------------------
// Menu model
// -----------------------------------------------------------------------------

enum MenuRow
{
    MENU_MODES = 0,
    MENU_BRIGHTNESS,
    MENU_CHANNELS,
    MENU_PALETTE,
    MENU_SYSTEM,
    MENU_COUNT
};

enum UiState
{
    UI_ROOT = 0,
    UI_SUB_MODES,
    UI_SUB_BRIGHTNESS,
    UI_SUB_CHANNELS,
    UI_SUB_PALETTE,
    UI_SUB_SYSTEM
};

// High-level actions the menu layer can request.
enum MenuAction
{
    ACT_NONE = 0,
    ACT_DISCOVER,
    ACT_PREVIEW,
    ACT_SAVE,
    ACT_LOAD,
    ACT_EXIT
};

// This mirrors the important bits of RGBCtrl's AppConfig / flags.
struct MenuState
{
    UiState uiState;
    int     rootIndex;      // 0..MENU_COUNT-1 (which blade is highlighted)

    // Core config (matches CFG.mode / brightness / speed / paletteCount / masterOff / resumeOnBoot)
    int     mode;           // 0..15
    int     brightness;     // 0..255
    int     speed;          // 0..255
    int     paletteCnt;     // 1..4
    bool    masterOff;      // "Master power" kill switch
    bool    resumeOn;       // resumeOnBoot

    // Extended RGBCtrl fields
    int     intensity;      // 0..255
    int     width;          // 1..255 (segment width / spacing)

    bool    enableCpu;      // CPU indicator flag
    bool    enableFan;      // Fan indicator flag

    // Per-channel LED counts & reverse flags (CFG.count[] / CFG.reverse[])
    int     chCount[4];     // CH1..CH4 LED counts
    bool    reverse[4];     // CH1..CH4 reverse toggles

    bool    customLoop;     // custom playlist loop flag
    bool    kratosMode;     // Kratos / passthrough toggle

    // UI-only helpers
    int     channelIndex;   // current channel focus in CHANNELS submenu (0..3)
    int     subIndex;       // row cursor in current submenu

    // Palette UI helper: which palette slot (0..3) is being edited
    int     paletteIndex;   // 0-based index into palette slots (0..paletteCnt-1)

    // Palette colors (0xRRGGBB, matches CFG.colorA..D on device)
    unsigned int colorA;
    unsigned int colorB;
    unsigned int colorC;
    unsigned int colorD;

    // -------------------------------------------------------------------------
    // Color-edit state for orb-based color picker
    // -------------------------------------------------------------------------
    bool    colorEditActive; // true while user is in "color edit" interaction
    float   colorHue;        // 0..360
    float   colorSat;        // 0..1
    float   colorVal;        // 0..1
};

// Initialise menu to defaults (Rainbow, mid brightness, etc.).
void Menu_Init(MenuState& s);

// Update menu state from buttons.
//  - buttons = current wButtons or BTN_* mask
//  - newly   = buttons that went from 0->1 this frame
// Returns a single high-level action for main.cpp to handle.
MenuAction Menu_Update(MenuState& s, WORD buttons, WORD newly);

// UI helpers
const char* GetMenuLabel(int index);
const char* GetModeName(int modeValue);
