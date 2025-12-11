#include "net.h"
#include <string.h>

// Network globals (were static in main.cpp, now exported via net.h)

bool   g_netOK = false;
bool   g_sockOK = false;
bool   g_bindOK = false;
bool   g_devFound = false;

SOCKET g_sock = INVALID_SOCKET;
IN_ADDR g_devIp = { 0 };

// UDP port for RGB device
static const USHORT g_port = 7777;

// Config we send in preview
int  g_cfgMode = 4;   // default: Rainbow
int  g_cfgBrightness = 128;
int  g_cfgSpeed = 128;
int  g_flashFrames = 0;

// Extra cfg (mirroring RGBCtrl.cpp)
int  g_cfgPaletteCount = 2;   // 1..4
bool g_cfgMasterOff = false;
bool g_cfgResumeOnBoot = true;

bool InitNetwork()
{
    XNetStartupParams xnsp;
    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;

    if (XNetStartup(&xnsp) != NO_ERROR)
        return false;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        XNetCleanup();
        return false;
    }

    // Wait for IP
    XNADDR a;
    ZeroMemory(&a, sizeof(a));
    DWORD st = XNetGetTitleXnAddr(&a);
    while (st == XNET_GET_XNADDR_PENDING)
    {
        Sleep(100);
        st = XNetGetTitleXnAddr(&a);
    }
    if (st & XNET_GET_XNADDR_NONE)
        return false;

    g_netOK = true;
    return true;
}

void InitSocket()
{
    if (!g_netOK) return;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET)
        return;

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

void SendDiscover()
{
    if (!g_sockOK) return;

    const char* payload = "{\"op\":\"discover\"}";

    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    sendto(g_sock, payload, (int)strlen(payload), 0, (sockaddr*)&to, sizeof(to));
}

void PumpDiscovery()
{
    if (!g_bindOK) return;

    char buf[512];
    sockaddr_in from;
    int fromlen = sizeof(from);

    for (;;)
    {
        int n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromlen);
        if (n <= 0) break;

        buf[n] = '\0';

        if (strstr(buf, "\"op\":\"discover\"") &&
            strstr(buf, "\"name\":\"XBOX RGB"))
        {
            g_devFound = true;
            g_devIp = from.sin_addr;
        }
    }
}

void SendPreview()
{
    if (!g_sockOK || !g_devFound) return;

    char json[256];
    int  pos = 0;
    json[0] = '\0';

    auto append_str = [](char* buf, int bufSize, int& pos, const char* s)
        {
            while (*s && pos < bufSize - 1)
                buf[pos++] = *s++;
            buf[pos] = '\0';
        };

    auto append_int = [](char* buf, int bufSize, int& pos, int v)
        {
            if (v == 0)
            {
                if (pos < bufSize - 1)
                {
                    buf[pos++] = '0';
                    buf[pos] = '\0';
                }
                return;
            }

            bool neg = false;
            if (v < 0)
            {
                neg = true;
                v = -v;
            }

            char tmp[16];
            int  len = 0;

            while (v > 0 && len < (int)sizeof(tmp))
            {
                tmp[len++] = char('0' + (v % 10));
                v /= 10;
            }
            if (neg && len < (int)sizeof(tmp))
                tmp[len++] = '-';

            for (int i = len - 1; i >= 0 && pos < bufSize - 1; ++i)
                buf[pos++] = tmp[i];

            buf[pos] = '\0';
        };

    append_str(json, sizeof(json), pos, "{\"op\":\"preview\",\"cfg\":{");
    append_str(json, sizeof(json), pos, "\"mode\":");
    append_int(json, sizeof(json), pos, g_cfgMode);
    append_str(json, sizeof(json), pos, ",\"brightness\":");
    append_int(json, sizeof(json), pos, g_cfgBrightness);
    append_str(json, sizeof(json), pos, ",\"speed\":");
    append_int(json, sizeof(json), pos, g_cfgSpeed);
    append_str(json, sizeof(json), pos, "}}");

    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_port);
    to.sin_addr = g_devIp;

    sendto(g_sock, json, (int)strlen(json), 0, (sockaddr*)&to, sizeof(to));

    g_flashFrames = 20;
}
