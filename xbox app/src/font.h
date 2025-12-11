#pragma once
#include <xtl.h>

// Simple 5x7 bitmap font renderer.
// Uses the global g_pDevice defined in main.cpp.
void DrawText(float x, float y, const char* text, float scale, DWORD color);
