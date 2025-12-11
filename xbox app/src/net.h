#pragma once
#include <xtl.h>
#include <winsockx.h>

// Expose the bits main.cpp uses.

// Network / discovery state
extern bool   g_netOK;
extern bool   g_sockOK;
extern bool   g_bindOK;
extern bool   g_devFound;
extern SOCKET g_sock;
extern IN_ADDR g_devIp;

// Config we send in preview (kept global so main can sync them)
extern int  g_cfgMode;          // default: Rainbow
extern int  g_cfgBrightness;
extern int  g_cfgSpeed;
extern int  g_flashFrames;

extern int  g_cfgPaletteCount;  // 1..4
extern bool g_cfgMasterOff;
extern bool g_cfgResumeOnBoot;

// API
bool InitNetwork();
void InitSocket();
void SendDiscover();
void PumpDiscovery();
void SendPreview();
