#include "input.h"
#include <string.h>

#define MAX_PORTS 4
#define ANALOG_THRESHOLD 30       // 0..255 analog-button threshold
#define STICK_DEADZONE  8000      // stick deadzone for GetSticks()

static HANDLE       g_padHandles[MAX_PORTS];
static DWORD        g_padLastPacket[MAX_PORTS];
static XINPUT_STATE g_padStates[MAX_PORTS];
static WORD         g_padButtons[MAX_PORTS];   // synthesized BTN_* mask

// -----------------------------------------------------------------------------
// InitInput
// -----------------------------------------------------------------------------
void InitInput()
{
    XInitDevices(0, 0);
    memset(g_padHandles, 0, sizeof(g_padHandles));
    memset(g_padLastPacket, 0, sizeof(g_padLastPacket));
    memset(g_padStates, 0, sizeof(g_padStates));
    memset(g_padButtons, 0, sizeof(g_padButtons));
}

// -----------------------------------------------------------------------------
// PumpInput  – reads controller state + synthesizes BTN_*
// -----------------------------------------------------------------------------
void PumpInput()
{
    DWORD ins = 0, rem = 0;

    // Hotplug handling
    if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem))
    {
        for (int i = 0; i < MAX_PORTS; ++i)
        {
            if (ins & 1)
            {
                if (!g_padHandles[i])
                {
                    g_padHandles[i] = XInputOpen(
                        XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);
                }
            }
            if (rem & 1)
            {
                if (g_padHandles[i])
                {
                    XInputClose(g_padHandles[i]);
                    g_padHandles[i] = NULL;
                }
            }

            ins >>= 1;
            rem >>= 1;
        }
    }

    // Read pad states
    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (!g_padHandles[i])
        {
            g_padButtons[i] = 0;
            continue;
        }

        XINPUT_STATE st;
        ZeroMemory(&st, sizeof(st));

        if (XInputGetState(g_padHandles[i], &st) == ERROR_SUCCESS)
        {
            if (st.dwPacketNumber != g_padLastPacket[i])
            {
                g_padLastPacket[i] = st.dwPacketNumber;
                g_padStates[i] = st;

                // Begin with native digital bits:
                //  D-Pad / Start / Back / thumb clicks
                WORD mask = st.Gamepad.wButtons;

                // Analog A/B/X/Y buttons -> convert to digital
                const BYTE* a = st.Gamepad.bAnalogButtons;

                if (a[XINPUT_GAMEPAD_A] > ANALOG_THRESHOLD)
                    mask |= BTN_A;
                if (a[XINPUT_GAMEPAD_B] > ANALOG_THRESHOLD)
                    mask |= BTN_B;
                if (a[XINPUT_GAMEPAD_X] > ANALOG_THRESHOLD)
                    mask |= BTN_X;
                if (a[XINPUT_GAMEPAD_Y] > ANALOG_THRESHOLD)
                    mask |= BTN_Y;

                g_padButtons[i] = mask;
            }
        }
        else
        {
            g_padButtons[i] = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// GetButtons – returns synthesized unified mask from first connected pad
// -----------------------------------------------------------------------------
WORD GetButtons()
{
    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (g_padHandles[i])
            return g_padButtons[i];
    }
    return 0;
}

// -----------------------------------------------------------------------------
// GetSticks – returns left/right analog sticks (with deadzones)
// -----------------------------------------------------------------------------
void GetSticks(int& lx, int& ly, int& rx, int& ry)
{
    lx = ly = rx = ry = 0;

    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (!g_padHandles[i])
            continue;

        const XINPUT_GAMEPAD& gp = g_padStates[i].Gamepad;

        lx = gp.sThumbLX;
        ly = gp.sThumbLY;
        rx = gp.sThumbRX;
        ry = gp.sThumbRY;

        // Deadzone filtering
        if (abs(lx) < STICK_DEADZONE) lx = 0;
        if (abs(ly) < STICK_DEADZONE) ly = 0;
        if (abs(rx) < STICK_DEADZONE) rx = 0;
        if (abs(ry) < STICK_DEADZONE) ry = 0;

        return; // first connected pad only
    }
}
