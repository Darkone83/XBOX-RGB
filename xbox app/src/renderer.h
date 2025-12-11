#pragma once

#include <xtl.h>
#include "menu.h"

// D3D globals (used by font.cpp etc.)
extern LPDIRECT3D8        g_pD3D;
extern LPDIRECT3DDEVICE8  g_pDevice;

// Low-level D3D init (main.cpp still calls this)
HRESULT InitD3D(void);

// High-level renderer control
bool  Render_Init();
void  Render_Shutdown();
void  RenderFrame(ULONG frame, bool devFound, WORD buttons, const MenuState& st);
void  Renderer_TriggerFlash(int frames);
