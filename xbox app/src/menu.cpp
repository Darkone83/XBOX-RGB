#include "menu.h"
#include <string.h>

// -----------------------------------------------------------------------------
// Internal tables
// -----------------------------------------------------------------------------

struct ModeDesc
{
    int         value;
    const char* name;
};

static const ModeDesc g_modes[] =
{
    {  0, "SOLID"          },
    {  1, "BREATHE"        },
    {  2, "COLOR WIPE"     },
    {  3, "LARSON"         },
    {  4, "RAINBOW"        },
    {  5, "THEATER"        },
    {  6, "TWINKLE"        },
    {  7, "COMET"          },
    {  8, "METEOR"         },
    {  9, "CLOCK SPIN"     },
    { 10, "PLASMA"         },
    { 11, "FIRE"           },
    { 12, "PALETTE CYCLE"  },
    { 13, "PALETTE CHASE"  },
    { 14, "CUSTOM"         },
    { 15, "UNSC COVENANT"  }
};

static const int g_modeCount = sizeof(g_modes) / sizeof(g_modes[0]);

static const char* g_menuLabels[MENU_COUNT] =
{
    "MODES",
    "BRIGHTNESS",
    "CHANNELS",
    "PALETTE",
    "SYSTEM"
};

// -----------------------------------------------------------------------------
// UI helpers
// -----------------------------------------------------------------------------

const char* GetMenuLabel(int index)
{
    if (index < 0 || index >= MENU_COUNT)
        return "";
    return g_menuLabels[index];
}

const char* GetModeName(int modeValue)
{
    for (int i = 0; i < g_modeCount; ++i)
    {
        if (g_modes[i].value == modeValue)
            return g_modes[i].name;
    }
    return "UNKNOWN";
}

// -----------------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------------

void Menu_Init(MenuState& s)
{
    memset(&s, 0, sizeof(s));

    s.uiState = UI_ROOT;
    s.rootIndex = MENU_MODES;

    // Core config (roughly matches RGBCtrl defaults)
    s.mode = 4;      // RAINBOW
    s.brightness = 128;    // RGBCtrl defaults to 180; mid is nicer for CRT
    s.speed = 128;
    s.paletteCnt = 2;
    s.masterOff = false;
    s.resumeOn = true;

    // Extended config
    s.intensity = 128;
    s.width = 4;

    s.enableCpu = true;
    s.enableFan = true;

    // Per-channel LED counts (matches AppConfig::count[] defaults)
    s.chCount[0] = 50;
    s.chCount[1] = 50;
    s.chCount[2] = 50;
    s.chCount[3] = 50;

    // Per-channel reverse (matches REVERSE_DEFAULTS shape)
    s.reverse[0] = true;
    s.reverse[1] = false;
    s.reverse[2] = false;
    s.reverse[3] = true;

    s.customLoop = true;
    s.kratosMode = false;

    // Palette default colors (XBOX-ish green + some extras)
    s.colorA = 0x00FF00; // green
    s.colorB = 0x00A0FF; // blue-ish
    s.colorC = 0xFF8000; // orange
    s.colorD = 0xFF00FF; // magenta

    s.channelIndex = 0;
    s.subIndex = 0;
    s.paletteIndex = 0;     // editing slot 0 by default

    // Color-edit state: default to a green-ish orb
    s.colorEditActive = false;
    s.colorHue = 120.0f; // green
    s.colorSat = 1.0f;
    s.colorVal = (float)s.brightness / 255.0f;
}

// -----------------------------------------------------------------------------
// Core update
// -----------------------------------------------------------------------------

MenuAction Menu_Update(MenuState& s, WORD buttons, WORD newly)
{
    MenuAction action = ACT_NONE;

    // Static state for BRIGHTNESS long-press coarse adjustment
    // (local to this translation unit, no header changes needed)
    static int s_brightHoldFramesL = 0;
    static int s_brightHoldFramesR = 0;

    // START: discover broadcast from anywhere
    if (newly & BTN_START)
    {
        action = ACT_DISCOVER;
    }

    // -------------------------------------------------------------------------
    // ROOT
    // -------------------------------------------------------------------------
    if (s.uiState == UI_ROOT)
    {
        // D-Pad U/D: navigate blades
        if (newly & BTN_DPAD_UP)
        {
            if (s.rootIndex > 0)
                s.rootIndex--;
        }
        if (newly & BTN_DPAD_DOWN)
        {
            if (s.rootIndex < MENU_COUNT - 1)
                s.rootIndex++;
        }

        // A: enter submenu for selected blade
        if (newly & BTN_A)
        {
            s.subIndex = 0;

            switch (s.rootIndex)
            {
            case MENU_MODES:
                s.uiState = UI_SUB_MODES;
                break;
            case MENU_BRIGHTNESS:
                s.uiState = UI_SUB_BRIGHTNESS;
                break;
            case MENU_CHANNELS:
                s.uiState = UI_SUB_CHANNELS;
                s.channelIndex = 0;
                break;
            case MENU_PALETTE:
                s.uiState = UI_SUB_PALETTE;
                break;
            case MENU_SYSTEM:
                s.uiState = UI_SUB_SYSTEM;
                break;
            default:
                s.uiState = UI_ROOT;
                break;
            }
        }

        // B: exit app from root only
        if (newly & BTN_B)
        {
            action = ACT_EXIT;
        }

        // X/Y: save/load stubs (main.cpp wires these up)
        if (newly & BTN_X)
        {
            action = ACT_SAVE;
        }
        if (newly & BTN_Y)
        {
            action = ACT_LOAD;
        }

        // At root, not color-editing
        s.colorEditActive = false;
        s.colorVal = (float)s.brightness / 255.0f;

        // Reset brightness hold counters when not in brightness submenu
        s_brightHoldFramesL = 0;
        s_brightHoldFramesR = 0;

        return action;
    }

    // -------------------------------------------------------------------------
    // SUB-MENUS
    // -------------------------------------------------------------------------

    // B: back to root from any submenu
    if (newly & BTN_B)
    {
        s.uiState = UI_ROOT;
        s.colorEditActive = false;
        s.colorVal = (float)s.brightness / 255.0f;

        // Reset brightness hold counters when leaving submenus
        s_brightHoldFramesL = 0;
        s_brightHoldFramesR = 0;

        return action;
    }

    // A in any submenu: trigger a preview
    if (newly & BTN_A)
    {
        action = ACT_PREVIEW;
    }

    // Row count per submenu (for cursor clamp)
    int maxRows = 1;
    switch (s.uiState)
    {
    case UI_SUB_MODES:      maxRows = 4;        break; // Mode, Speed, Intensity, Width
    case UI_SUB_BRIGHTNESS: maxRows = 1;        break; // Brightness
    case UI_SUB_CHANNELS:   maxRows = 8;        break; // CH1..4 count, CH1..4 reverse
    case UI_SUB_PALETTE:    maxRows = 2;        break; // [0]=count, [1]=slot selector
    case UI_SUB_SYSTEM:     maxRows = 5;        break; // Master, Resume, CPU, FAN, Kratos
    default:                maxRows = 1;        break;
    }

    if (s.subIndex < 0) s.subIndex = 0;
    if (s.subIndex >= maxRows) s.subIndex = maxRows - 1;

    // U/D: move row cursor
    if (newly & BTN_DPAD_UP)
    {
        s.subIndex--;
        if (s.subIndex < 0) s.subIndex = 0;
    }
    if (newly & BTN_DPAD_DOWN)
    {
        s.subIndex++;
        if (s.subIndex >= maxRows) s.subIndex = maxRows - 1;
    }

    // Keep channelIndex in sync for CHANNELS submenu
    if (s.uiState == UI_SUB_CHANNELS)
    {
        int row = s.subIndex;
        if (row < 0) row = 0;
        if (row > 7) row = 7;

        // Rows 0..3 -> CH1..4 counts, 4..7 -> CH1..4 reverse
        int ch = (row < 4) ? row : (row - 4);
        if (ch < 0) ch = 0;
        if (ch > 3) ch = 3;

        s.channelIndex = ch;
    }

    // L/R: adjust currently-highlighted field (no CRT float calls!)
    if (newly & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))
    {
        int dir = (newly & BTN_DPAD_RIGHT) ? +1 : -1;

        switch (s.uiState)
        {
            // -----------------------------------------------------------------
            // MODES: mode, speed, intensity, width
            // -----------------------------------------------------------------
        case UI_SUB_MODES:
        {
            switch (s.subIndex)
            {
            case 0: // Mode
                if (dir < 0)
                {
                    s.mode--;
                    if (s.mode < 0)
                        s.mode = g_modes[g_modeCount - 1].value;
                }
                else
                {
                    s.mode++;
                    if (s.mode > g_modes[g_modeCount - 1].value)
                        s.mode = g_modes[0].value;
                }
                break;

            case 1: // Speed
                s.speed += dir * 8;
                if (s.speed < 0)   s.speed = 0;
                if (s.speed > 255) s.speed = 255;
                break;

            case 2: // Intensity
                s.intensity += dir * 8;
                if (s.intensity < 0)   s.intensity = 0;
                if (s.intensity > 255) s.intensity = 255;
                break;

            case 3: // Width
                s.width += dir;
                if (s.width < 1)   s.width = 1;
                if (s.width > 255) s.width = 255;
                break;

            default:
                break;
            }
            break;
        }

        // -----------------------------------------------------------------
        // BRIGHTNESS
        //   Tap = fine (±1)
        //   Long-press = coarse auto-repeat (handled below)
        // -----------------------------------------------------------------
        case UI_SUB_BRIGHTNESS:
        {
            // Fine step for a single tap
            s.brightness += dir * 1;
            if (s.brightness < 0)   s.brightness = 0;
            if (s.brightness > 255) s.brightness = 255;
            break;
        }

        // -----------------------------------------------------------------
        // CHANNELS: CH1..4 LED counts + reverse flags
        //   Rows 0..3:  CHn Count  (NOW: ±1 LED per step)
        //   Rows 4..7:  CHn Reverse
        // -----------------------------------------------------------------
        case UI_SUB_CHANNELS:
        {
            int row = s.subIndex;
            if (row < 0) row = 0;
            if (row > 7) row = 7;

            if (row < 4)
            {
                // Count rows (fine: 1 LED at a time)
                int ch = row; // 0..3 = CH1..4
                int step = 1; // *** changed from 5 to 1 ***

                s.chCount[ch] += dir * step;

                if (s.chCount[ch] < 0)
                    s.chCount[ch] = 0;

                if (s.chCount[ch] > 1024)
                    s.chCount[ch] = 1024;
            }
            else
            {
                // Reverse rows
                int ch = row - 4; // 4..7 -> CH1..4
                if (ch < 0) ch = 0;
                if (ch > 3) ch = 3;

                s.reverse[ch] = !s.reverse[ch];
            }
            break;
        }

        // -----------------------------------------------------------------
        // PALETTE:
        //   subIndex 0 : paletteCnt 1..4
        //   subIndex 1 : paletteIndex (0..paletteCnt-1)
        // -----------------------------------------------------------------
        case UI_SUB_PALETTE:
        {
            if (s.subIndex == 0)
            {
                s.paletteCnt += dir;
                if (s.paletteCnt < 1) s.paletteCnt = 1;
                if (s.paletteCnt > 4) s.paletteCnt = 4;

                if (s.paletteIndex >= s.paletteCnt)
                    s.paletteIndex = s.paletteCnt - 1;
                if (s.paletteIndex < 0)
                    s.paletteIndex = 0;
            }
            else if (s.subIndex == 1)
            {
                s.paletteIndex += dir;
                if (s.paletteIndex < 0) s.paletteIndex = 0;
                if (s.paletteIndex >= s.paletteCnt)
                    s.paletteIndex = s.paletteCnt - 1;
            }
            break;
        }

        // -----------------------------------------------------------------
        // SYSTEM: master, resume, CPU, FAN, Kratos
        // -----------------------------------------------------------------
        case UI_SUB_SYSTEM:
        {
            switch (s.subIndex)
            {
            case 0: // Master power
                s.masterOff = !s.masterOff;
                break;
            case 1: // Resume on boot
                s.resumeOn = !s.resumeOn;
                break;
            case 2: // CPU indicator
                s.enableCpu = !s.enableCpu;
                break;
            case 3: // FAN indicator
                s.enableFan = !s.enableFan;
                break;
            case 4: // Kratos mode
                s.kratosMode = !s.kratosMode;
                break;
            default:
                break;
            }
            break;
        }

        default:
            break;
        }
    }

    // ---------------------------------------------------------------------
    // BRIGHTNESS long-press coarse adjustment (D-Pad L/R held)
    //   - Only active in UI_SUB_BRIGHTNESS
    //   - After a short delay, auto-repeat ±8
    // ---------------------------------------------------------------------
    if (s.uiState == UI_SUB_BRIGHTNESS)
    {
        const int coarseStep = 8;   // coarse delta per repeat
        const int holdStartFrames = 15;  // frames before auto-repeat kicks in
        const int repeatEvery = 2;   // repeat every N frames after start

        // RIGHT (brighter)
        if (buttons & BTN_DPAD_RIGHT)
        {
            if (s_brightHoldFramesR < 1000)
                ++s_brightHoldFramesR;

            if (s_brightHoldFramesR > holdStartFrames &&
                ((s_brightHoldFramesR - holdStartFrames) % repeatEvery) == 0)
            {
                s.brightness += coarseStep;
                if (s.brightness < 0)   s.brightness = 0;
                if (s.brightness > 255) s.brightness = 255;
            }
        }
        else
        {
            s_brightHoldFramesR = 0;
        }

        // LEFT (dimmer)
        if (buttons & BTN_DPAD_LEFT)
        {
            if (s_brightHoldFramesL < 1000)
                ++s_brightHoldFramesL;

            if (s_brightHoldFramesL > holdStartFrames &&
                ((s_brightHoldFramesL - holdStartFrames) % repeatEvery) == 0)
            {
                s.brightness -= coarseStep;
                if (s.brightness < 0)   s.brightness = 0;
                if (s.brightness > 255) s.brightness = 255;
            }
        }
        else
        {
            s_brightHoldFramesL = 0;
        }
    }
    else
    {
        // Not in brightness submenu -> no auto-repeat
        s_brightHoldFramesL = 0;
        s_brightHoldFramesR = 0;
    }

    // -------------------------------------------------------------------------
    // Analog color editing for orb / palette slots
    //
    // Active ONLY when:
    //   - In PALETTE submenu
    //   - subIndex == 1 (the "Palette slot" row)
    //
    // Mapping:
    //   LEFT STICK X/Y : color (hue + saturation)
    //   RIGHT STICK Y  : brightness (value)
    //
    // This both drives the orb preview (colorHue/Sat/Val) AND commits
    // the resulting RGB into the active palette slot (colorA..D).
    // -------------------------------------------------------------------------
    if (s.uiState == UI_SUB_PALETTE && s.subIndex == 1)
    {
        s.colorEditActive = true;

        int lx = 0, ly = 0, rx = 0, ry = 0;
        GetSticks(lx, ly, rx, ry);

        const int deadzone = 8000;

        float fxL = 0.0f, fyL = 0.0f;
        float fyR = 0.0f;

        if (lx > deadzone || lx < -deadzone)
            fxL = (float)lx / 32768.0f;
        if (ly > deadzone || ly < -deadzone)
            fyL = (float)ly / 32768.0f;
        if (ry > deadzone || ry < -deadzone)
            fyR = (float)ry / 32768.0f;

        // Hue (wrap 0..360) from LEFT stick X  -----------------------------
        if (fxL != 0.0f)
        {
            s.colorHue += fxL * 4.0f; // tweak factor for speed
            if (s.colorHue < 0.0f)    s.colorHue += 360.0f;
            if (s.colorHue >= 360.0f) s.colorHue -= 360.0f;
        }

        // Saturation 0..1 from LEFT stick Y (up = more saturated) ----------
        if (fyL != 0.0f)
        {
            s.colorSat += -fyL * 0.04f;
            if (s.colorSat < 0.0f) s.colorSat = 0.0f;
            if (s.colorSat > 1.0f) s.colorSat = 1.0f;
        }

        // Value 0..1 from RIGHT stick Y (up = brighter) --------------------
        if (fyR != 0.0f)
        {
            s.colorVal += -fyR * 0.04f;
            if (s.colorVal < 0.0f) s.colorVal = 0.0f;
            if (s.colorVal > 1.0f) s.colorVal = 1.0f;
        }
        else
        {
            // When not actively pushing, tie to global brightness
            s.colorVal = (float)s.brightness / 255.0f;
        }

        // -----------------------------------------------------------------
        // Commit HSV → RGB into the active palette slot (colorA..D)
        // -----------------------------------------------------------------
        float h = s.colorHue;
        if (h < 0.0f)   h += 360.0f;
        if (h >= 360.0f) h -= 360.0f;

        float sSat = s.colorSat;
        float vVal = s.colorVal;

        float r, g, b;

        if (sSat <= 0.0f)
        {
            // Achromatic (grey)
            r = g = b = vVal;
        }
        else
        {
            float hh = h / 60.0f;
            int    i = (int)hh;
            float  ff = hh - (float)i;

            float p = vVal * (1.0f - sSat);
            float q = vVal * (1.0f - (sSat * ff));
            float t = vVal * (1.0f - (sSat * (1.0f - ff)));

            switch (i)
            {
            case 0:
                r = vVal; g = t;    b = p;
                break;
            case 1:
                r = q;    g = vVal; b = p;
                break;
            case 2:
                r = p;    g = vVal; b = t;
                break;
            case 3:
                r = p;    g = q;    b = vVal;
                break;
            case 4:
                r = t;    g = p;    b = vVal;
                break;
            default:
                r = vVal; g = p;    b = q;
                break;
            }
        }

        unsigned int rgb =
            (((unsigned int)(r * 255.0f)) << 16) |
            (((unsigned int)(g * 255.0f)) << 8) |
            ((unsigned int)(b * 255.0f));

        switch (s.paletteIndex)
        {
        case 0: s.colorA = rgb; break;
        case 1: s.colorB = rgb; break;
        case 2: s.colorC = rgb; break;
        case 3: s.colorD = rgb; break;
        default:
            break;
        }
    }
    else
    {
        s.colorEditActive = false;
        // Keep orb value in sync with brightness when not editing
        s.colorVal = (float)s.brightness / 255.0f;
    }

    return action;
}
