#pragma once
#include <xtl.h>

// -----------------------------------------------------------------------------
// Unified digital mask used everywhere.
//
// Notes:
// - D-Pad, START/BACK, thumb clicks = native XINPUT bits.
// - ABXY = high-bit synthetic flags derived from analog buttons.
// -----------------------------------------------------------------------------

enum
{
    BTN_DPAD_UP = XINPUT_GAMEPAD_DPAD_UP,
    BTN_DPAD_DOWN = XINPUT_GAMEPAD_DPAD_DOWN,
    BTN_DPAD_LEFT = XINPUT_GAMEPAD_DPAD_LEFT,
    BTN_DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT,

    BTN_START = XINPUT_GAMEPAD_START,
    BTN_BACK = XINPUT_GAMEPAD_BACK,
    BTN_LTHUMB = XINPUT_GAMEPAD_LEFT_THUMB,
    BTN_RTHUMB = XINPUT_GAMEPAD_RIGHT_THUMB,

    // Synthetic analog-button digital flags
    BTN_A = 0x1000,
    BTN_B = 0x2000,
    BTN_X = 0x4000,
    BTN_Y = 0x8000,
};

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

// Initialize controller ports.
void InitInput();

// Polls & updates button state + analog stick state.
void PumpInput();

// Returns OR of all controller button masks (BTN_* flags above).
WORD GetButtons();

// NEW: returns left/right stick raw 16-bit values.
// If no controller present: all zeros.
//   lx,ly = left stick   (-32768 .. 32767)
//   rx,ry = right stick  (-32768 .. 32767)
void GetSticks(int& lx, int& ly, int& rx, int& ry);
