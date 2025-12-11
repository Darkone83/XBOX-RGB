// main.cpp – RXDK OG Xbox XBOX RGB launcher with 3D-style menu + submenus
//
// Root Controls:
//   D-PAD U/D -> move blades
//   A         -> ENTER submenu
//   B         -> EXIT app (clean shutdown)
//   START     -> DISCOVER
//   X         -> SAVE (send persistent config to device)
//   Y         -> LOAD (pull config from device)
//
// SAVE / LOAD protocol is 1:1 with RGBCtrl.cpp on the ESP32 firmware.

#include <xtl.h>
#include <winsockx.h>
#include <string.h>
#include <stdlib.h>

#include "menu.h"
#include "renderer.h"
#include "input.h"

// -----------------------------------------------------------------------------
// Status HUD hook (implemented in renderer.cpp)
// -----------------------------------------------------------------------------
void Renderer_SetStatus(const char* msg, int frames);

// -----------------------------------------------------------------------------
// Helpers: tiny JSON builders
// -----------------------------------------------------------------------------

static void append_str(char* buf, int bufSize, int& pos, const char* s)
{
    if (!buf || bufSize <= 0 || !s) return;
    while (*s && pos < bufSize - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
}

static void append_int(char* buf, int bufSize, int& pos, int v)
{
    if (!buf || bufSize <= 0) return;

    if (v == 0) {
        if (pos < bufSize - 1) { buf[pos++] = '0'; buf[pos] = '\0'; }
        return;
    }

    bool neg = (v < 0);
    if (neg) v = -v;

    char tmp[16];
    int len = 0;

    while (v > 0 && len < 16) { tmp[len++] = char('0' + (v % 10)); v /= 10; }
    if (neg) tmp[len++] = '-';

    for (int i = len - 1; i >= 0 && pos < bufSize - 1; --i)
        buf[pos++] = tmp[i];

    buf[pos] = '\0';
}

// -----------------------------------------------------------------------------
// Integer HSV -> RGB helper (24-bit 0xRRGGBB)
//   h: 0..359 (degrees)
//   s: 0..255
//   v: 0..255
// -----------------------------------------------------------------------------

static int HSVtoRGB24(int h, int s, int v)
{
    int r, g, b;

    if (s <= 0) {
        r = g = b = v;
    }
    else {
        if (h < 0)   h = 0;
        if (h > 359) h = 359;

        int region = h / 60;                      // 0..5
        int rem = h - (region * 60);              // 0..59
        int p = (v * (255 - s)) / 255;
        int q = (v * (255 * 60 - s * rem) / (255 * 60));
        int t = (v * (255 * 60 - s * (60 - rem)) / (255 * 60));

        switch (region) {
        default:
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        }
    }

    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;

    return (r << 16) | (g << 8) | b;
}

// -----------------------------------------------------------------------------
// Networking
// -----------------------------------------------------------------------------

static bool   g_netOK = false;
static bool   g_sockOK = false;
static bool   g_bindOK = false;
static bool   g_devFound = false;

static SOCKET g_sock = INVALID_SOCKET;
static IN_ADDR g_devIp = { 0 };
static const USHORT g_port = 7777;

static bool InitNetwork()
{
    XNetStartupParams xnsp;
    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;

    if (XNetStartup(&xnsp) != NO_ERROR) return false;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        XNetCleanup();
        return false;
    }

    XNADDR a;
    ZeroMemory(&a, sizeof(a));
    DWORD st = XNetGetTitleXnAddr(&a);
    while (st == XNET_GET_XNADDR_PENDING) {
        Sleep(100);
        st = XNetGetTitleXnAddr(&a);
    }
    if (st & XNET_GET_XNADDR_NONE) return false;

    g_netOK = true;
    return true;
}

static void InitSocket()
{
    if (!g_netOK) return;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return;

    g_sockOK = true;

    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);

    if (bind(g_sock, (sockaddr*)&addr, sizeof(addr)) == 0)
        g_bindOK = true;
}

static void SendDiscover()
{
    if (!g_sockOK) return;

    const char* payload = "{\"op\":\"discover\"}";
    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    sendto(g_sock, payload, (int)strlen(payload), 0,
        (sockaddr*)&to, sizeof(to));
}

// Forward decl so PumpDiscovery can auto-load on first device detection
static void SendLoad();

static bool TryApplyLoadedConfig(MenuState& menu, const char* json);

static void PumpDiscovery(MenuState& menu)
{
    if (!g_bindOK) return;

    char        buf[768];
    sockaddr_in from;
    int         fromlen = sizeof(from);

    for (;;)
    {
        int n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0,
            (sockaddr*)&from, &fromlen);
        if (n <= 0)
            break;

        buf[n] = '\0';

        // 1) DISCOVER replies: find XBOX RGB + auto-load once per IP
        if (strstr(buf, "\"op\":\"discover\"") &&
            strstr(buf, "\"name\":\"XBOX RGB"))
        {
            bool newDevice = (!g_devFound) || (g_devIp.s_addr != from.sin_addr.s_addr);

            g_devFound = true;
            g_devIp = from.sin_addr;

            if (newDevice)
            {
                // Auto-poll config once per newly seen device
                SendLoad();
            }
        }
        // 2) GET replies: apply cfg into MenuState so renderer sees it
        else if (strstr(buf, "\"op\":\"get\"") && strstr(buf, "\"cfg\""))
        {
            TryApplyLoadedConfig(menu, buf);
        }
    }
}



// -----------------------------------------------------------------------------
// BUILD & SEND PREVIEW / SAVE JSON
// -----------------------------------------------------------------------------

static void BuildCfgJSON(char* json, int size, int& pos, const MenuState& menu)
{
    // Open cfg object
    append_str(json, size, pos, "\"cfg\":{");

    // numerics
    append_str(json, size, pos, "\"mode\":");
    append_int(json, size, pos, menu.mode);

    append_str(json, size, pos, ",\"brightness\":");
    append_int(json, size, pos, menu.brightness);

    append_str(json, size, pos, ",\"speed\":");
    append_int(json, size, pos, menu.speed);

    append_str(json, size, pos, ",\"intensity\":");
    append_int(json, size, pos, menu.intensity);

    append_str(json, size, pos, ",\"width\":");
    append_int(json, size, pos, menu.width);

    append_str(json, size, pos, ",\"paletteCount\":");
    append_int(json, size, pos, menu.paletteCnt);

    // -------------------------------------------------------------------------
    // Palette colors as colorA..D (0xRRGGBB), taken directly from MenuState.
    // These are updated in menu.cpp when editing a palette slot.
    // -------------------------------------------------------------------------
    append_str(json, size, pos, ",\"colorA\":");
    append_int(json, size, pos, (int)menu.colorA);

    append_str(json, size, pos, ",\"colorB\":");
    append_int(json, size, pos, (int)menu.colorB);

    append_str(json, size, pos, ",\"colorC\":");
    append_int(json, size, pos, (int)menu.colorC);

    append_str(json, size, pos, ",\"colorD\":");
    append_int(json, size, pos, (int)menu.colorD);

    // Per-channel counts (CFG.count[])
    append_str(json, size, pos, ",\"count\":[");
    for (int i = 0; i < 4; ++i)
    {
        append_int(json, size, pos, menu.chCount[i]);
        if (i < 3)
            append_str(json, size, pos, ",");
    }
    append_str(json, size, pos, "]");

    // booleans
    append_str(json, size, pos, ",\"resumeOnBoot\":");
    append_str(json, size, pos, menu.resumeOn ? "true" : "false");

    append_str(json, size, pos, ",\"enableCpu\":");
    append_str(json, size, pos, menu.enableCpu ? "true" : "false");

    append_str(json, size, pos, ",\"enableFan\":");
    append_str(json, size, pos, menu.enableFan ? "true" : "false");

    append_str(json, size, pos, ",\"masterOff\":");
    append_str(json, size, pos, menu.masterOff ? "true" : "false");

    append_str(json, size, pos, ",\"kratosMode\":");
    append_str(json, size, pos, menu.kratosMode ? "true" : "false");

    // reverse[4]
    append_str(json, size, pos, ",\"reverse\":[");
    for (int i = 0; i < 4; ++i) {
        append_str(json, size, pos, menu.reverse[i] ? "true" : "false");
        if (i < 3) append_str(json, size, pos, ",");
    }
    append_str(json, size, pos, "]");

    // custom seq + loop
    append_str(json, size, pos, ",\"customSeq\":[]");

    append_str(json, size, pos, ",\"customLoop\":");
    append_str(json, size, pos, menu.customLoop ? "true" : "false");

    append_str(json, size, pos, "}"); // close "cfg"
}

static void SendPreview(const MenuState& menu)
{
    if (!g_sockOK || !g_devFound) return;

    char json[768];
    int  pos = 0;
    json[0] = '\0';

    append_str(json, sizeof(json), pos, "{\"op\":\"preview\",");
    BuildCfgJSON(json, sizeof(json), pos, menu);
    append_str(json, sizeof(json), pos, "}"); // close root object

    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_port);
    to.sin_addr = g_devIp;

    sendto(g_sock, json, (int)strlen(json), 0, (sockaddr*)&to, sizeof(to));
}

static void SendSave(const MenuState& menu)
{
    if (!g_sockOK || !g_devFound) return;

    char json[768];
    int  pos = 0;
    json[0] = '\0';

    append_str(json, sizeof(json), pos, "{\"op\":\"save\",");
    BuildCfgJSON(json, sizeof(json), pos, menu);
    append_str(json, sizeof(json), pos, "}"); // close root object

    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_port);
    to.sin_addr = g_devIp;

    sendto(g_sock, json, (int)strlen(json), 0, (sockaddr*)&to, sizeof(to));
}

// -----------------------------------------------------------------------------
// LOAD REQUEST + RESPONSE PARSER
// -----------------------------------------------------------------------------

static void SendLoad()
{
    if (!g_sockOK || !g_devFound) return;

    // ESP32 side uses "get" to return the current CFG as JSON.
    const char* payload = "{\"op\":\"get\"}";

    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_port);
    to.sin_addr = g_devIp;

    sendto(g_sock, payload, (int)strlen(payload), 0, (sockaddr*)&to, sizeof(to));
}


// VERY small JSON extractor based on strstr + manual stepping.
// Only extracts fields we know exist (for now).
// VERY small JSON extractor based on strstr + manual stepping.
// Only reads from the existing {"ok":true,"op":"get","cfg":{...}} shape.
static bool TryApplyLoadedConfig(MenuState& menu, const char* json)
{
    if (!json)
        return false;

    auto getInt = [&](const char* key, int& out) {
        const char* p = strstr(json, key);
        if (!p) return;
        p = strchr(p, ':'); if (!p) return;
        out = atoi(p + 1);
        };

    auto getBool = [&](const char* key, bool& out) {
        const char* p = strstr(json, key);
        if (!p) return;
        p = strchr(p, ':'); if (!p) return;
        const char* v = p + 1;
        if (strncmp(v, "true", 4) == 0) out = true;
        else out = false;
        };

    // Core numerics (inside cfg)
    getInt("\"mode\"", menu.mode);
    getInt("\"brightness\"", menu.brightness);
    getInt("\"speed\"", menu.speed);
    getInt("\"intensity\"", menu.intensity);
    getInt("\"width\"", menu.width);
    getInt("\"paletteCount\"", menu.paletteCnt);

    // Core booleans
    getBool("\"resumeOnBoot\"", menu.resumeOn);
    getBool("\"enableCpu\"", menu.enableCpu);
    getBool("\"enableFan\"", menu.enableFan);
    getBool("\"masterOff\"", menu.masterOff);
    getBool("\"kratosMode\"", menu.kratosMode);
    getBool("\"customLoop\"", menu.customLoop);

    // reverse[]
    const char* r = strstr(json, "\"reverse\"");
    if (r) {
        const char* b = strchr(r, '[');
        if (b) {
            for (int i = 0; i < 4; i++) {
                const char* t = strchr(b, 't');
                const char* f = strchr(b, 'f');
                if (!t && !f) break;
                if (t && (!f || t < f)) { menu.reverse[i] = true;  b = t + 1; }
                else { menu.reverse[i] = false; b = f + 1; }
            }
        }
    }

    // Palette colors colorA..D (0xRRGGBB stored as integer in cfg)
    auto getColor = [&](const char* key, unsigned int& out) {
        int tmp = (int)out;
        getInt(key, tmp);
        out = (unsigned int)tmp;
        };

    getColor("\"colorA\"", menu.colorA);
    getColor("\"colorB\"", menu.colorB);
    getColor("\"colorC\"", menu.colorC);
    getColor("\"colorD\"", menu.colorD);

    // Per-channel counts: count[4]
    {
        const char* c = strstr(json, "\"count\"");
        if (c) {
            const char* b = strchr(c, '[');
            if (b) {
                for (int i = 0; i < 4; ++i) {
                    // Skip to next digit / minus sign
                    while (*b && !((*b >= '0' && *b <= '9') || *b == '-'))
                        ++b;
                    if (!*b) break;

                    menu.chCount[i] = atoi(b);

                    // Move to next comma or ']'
                    const char* comma = strchr(b, ',');
                    const char* rb = strchr(b, ']');
                    if (!comma || (rb && rb < comma)) {
                        if (rb) b = rb;
                        break;
                    }
                    else {
                        b = comma + 1;
                    }
                }
            }
        }
    }

    // Keep palette + orb state sane for renderer
    if (menu.paletteCnt < 1) menu.paletteCnt = 1;
    if (menu.paletteCnt > 4) menu.paletteCnt = 4;
    if (menu.paletteIndex >= menu.paletteCnt) menu.paletteIndex = menu.paletteCnt - 1;
    if (menu.paletteIndex < 0) menu.paletteIndex = 0;

    // Tie orb brightness to config brightness so the glow matches
    menu.colorVal = (float)menu.brightness / 255.0f;
    menu.colorEditActive = false; // just loaded from device

    return true;
}


static void PumpLoadResponse(MenuState& menu)
{
    if (!g_bindOK) return;

    char        buf[768];
    sockaddr_in from;
    int         fromlen = sizeof(from);

    int n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0,
        (sockaddr*)&from, &fromlen);
    if (n <= 0) return;

    buf[n] = '\0';

    // Only apply if it looks like a config-ish payload.
    TryApplyLoadedConfig(menu, buf);
}

// -----------------------------------------------------------------------------
// Entry
// -----------------------------------------------------------------------------

extern "C" void __cdecl main()
{
    if (FAILED(InitD3D())) { while (1) {} }

    InitInput();
    InitNetwork();
    InitSocket();
    SendDiscover();

    MenuState menu;
    Menu_Init(menu);

    WORD          lastButtons = 0;
    unsigned long frame = 0;

    while (1)
    {
        PumpInput();
        PumpDiscovery(menu);  // handle load responses

        WORD buttons = GetButtons();
        WORD newly = (WORD)(buttons & ~lastButtons);
        lastButtons = buttons;

        // Handle SAVE (X) using our synthesized BTN_* mask
        if (newly & BTN_X) {
            SendSave(menu);
            Renderer_SetStatus("Config saved", 60);   // <--- STATUS HUD
        }

        // Handle LOAD (Y)
        if (newly & BTN_Y)
            SendLoad();

        // MENU action
        MenuAction act = Menu_Update(menu, buttons, newly);

        if (act == ACT_EXIT) {
            XLaunchNewImage(NULL, NULL);
            return;
        }

        switch (act)
        {
        case ACT_DISCOVER:
            SendDiscover();
            break;

        case ACT_PREVIEW:
            SendPreview(menu);
            break;

        case ACT_SAVE:
            SendSave(menu);
            Renderer_SetStatus("Config saved", 60);   // <--- STATUS HUD
            break;

        case ACT_LOAD:
            SendLoad();
            break;

        default:
            break;
        }

        RenderFrame(frame, g_devFound, buttons, menu);
        frame++;
    }
}
