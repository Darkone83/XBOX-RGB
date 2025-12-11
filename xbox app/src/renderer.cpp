#include "renderer.h"
#include "font.h"
#include "menu.h"

#include <xtl.h>
#include <math.h>
#include <string.h>

// =====================================================================================
// Globals
// =====================================================================================
LPDIRECT3D8        g_pD3D = nullptr;
LPDIRECT3DDEVICE8  g_pDevice = nullptr;

// Fixed logical resolution; this is known-good for OG Xbox
static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

// Status HUD state
static int  g_statusTimer = 0;
static char g_statusText[64] = { 0 };

void Renderer_SetStatus(const char* msg, int frames)
{
    if (!msg) return;
    strncpy(g_statusText, msg, sizeof(g_statusText) - 1);
    g_statusText[sizeof(g_statusText) - 1] = '\0';
    g_statusTimer = frames;
}


// =====================================================================================
// Basic FVF Vertex
// =====================================================================================
struct VERTEX
{
    float x, y, z, rhw;
    DWORD color;
};
#define FVF_VERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// =====================================================================================
// Trig LUT for circles (shared by plasma + orb)
// =====================================================================================
static const int LUT_SEG = 64;
static float g_cosLUT[LUT_SEG + 1];
static float g_sinLUT[LUT_SEG + 1];
static bool  g_trigInit = false;

static void InitTrigLUT()
{
    if (g_trigInit)
        return;

    for (int i = 0; i <= LUT_SEG; ++i)
    {
        float ang = (float)i / (float)LUT_SEG * 6.28318f; // 2π
        g_cosLUT[i] = cosf(ang);
        g_sinLUT[i] = sinf(ang);
    }
    g_trigInit = true;
}

// Map a local segment index [0..segCount] into our LUT
static inline void GetLUTCosSin(int i, int segCount, float& c, float& s)
{
    if (segCount <= 0)
    {
        c = 1.0f;
        s = 0.0f;
        return;
    }

    int idx = (i * LUT_SEG) / segCount;
    if (idx < 0) idx = 0;
    if (idx > LUT_SEG) idx = LUT_SEG;

    c = g_cosLUT[idx];
    s = g_sinLUT[idx];
}

// =====================================================================================
// Small local string helpers (no CRT vsnprintf)
// =====================================================================================
static void append_str(char* buf, int bufSize, int& pos, const char* s)
{
    if (!buf || bufSize <= 0 || !s) return;

    while (*s && pos < bufSize - 1)
        buf[pos++] = *s++;

    buf[pos] = '\0';
}

static void append_int(char* buf, int bufSize, int& pos, int v)
{
    if (!buf || bufSize <= 0) return;

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

    for (int i = len - 1; i >= 0 && pos < bufSize - 1; --i)
        buf[pos++] = tmp[i];

    buf[pos] = '\0';
}

// =====================================================================================
// Manual Projection
// =====================================================================================
static void ProjectToScreen(float wx, float wy, float wz, DWORD color, VERTEX& out)
{
    const float focal = 420.0f;
    if (wz < 1.0f) wz = 1.0f;

    float invZ = 1.0f / wz;
    out.x = wx * focal * invZ + SCREEN_W * 0.5f;
    out.y = -wy * focal * invZ + SCREEN_H * 0.5f;
    out.z = 0.0f;
    out.rhw = 1.0f;
    out.color = color;
}

// =====================================================================================
// Build [#####     ] bar
// =====================================================================================
static void BuildBar(char* out, int outSize, int value, int max)
{
    if (outSize < 4)
    {
        if (outSize > 0) out[0] = '\0';
        return;
    }

    if (value < 0) value = 0;
    if (value > max) value = max;

    int segCount = outSize - 3;
    if (segCount > 10) segCount = 10;
    if (segCount < 1) segCount = 1;

    out[0] = '[';
    for (int i = 0; i < segCount; ++i)
    {
        int threshold = (i + 1) * max / segCount;
        out[1 + i] = (value >= threshold) ? '#' : ' ';
    }
    out[1 + segCount] = ']';
    out[2 + segCount] = '\0';
}

// =====================================================================================
// HSV → RGB helper (h in degrees 0..360, s/v 0..1, output 0..1)
// =====================================================================================
static void HSVToRGB(float h, float s, float v, float& r, float& g, float& b)
{
    if (s <= 0.0f)
    {
        r = g = b = v;
        return;
    }

    // wrap hue
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;

    float c = v * s;
    float hh = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));

    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;

    if (hh >= 0.0f && hh < 1.0f) { r1 = c; g1 = x; b1 = 0.0f; }
    else if (hh < 2.0f) { r1 = x; g1 = c; b1 = 0.0f; }
    else if (hh < 3.0f) { r1 = 0.0f; g1 = c; b1 = x; }
    else if (hh < 4.0f) { r1 = 0.0f; g1 = x; b1 = c; }
    else if (hh < 5.0f) { r1 = x; g1 = 0.0f; b1 = c; }
    else { r1 = c; g1 = 0.0f; b1 = x; }

    float m = v - c;
    r = r1 + m;
    g = g1 + m;
    b = b1 + m;
}

// =====================================================================================
// Simple text shadow helper
// =====================================================================================
static void DrawTextShadow(float x, float y, const char* s, float scale, DWORD color)
{
    DWORD shadow = D3DCOLOR_ARGB(170, 0, 0, 0);
    DrawText(x + 2.0f, y + 2.0f, s, scale, shadow);
    DrawText(x, y, s, scale, color);
}

// =====================================================================================
// Background plasma
// =====================================================================================
static void DrawBackgroundPlasma(float t)
{
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);

    // Lower SEG saves work, LUT avoids per-vertex sin/cos
    const int SEG = 20;

    auto DrawGlow = [&](float cx, float cy, float radius,
        DWORD centerColor, DWORD edgeColor)
        {
            VERTEX v[22]; // SEG + 2 for SEG=20

            v[0].x = cx;
            v[0].y = cy;
            v[0].z = 0.0f;
            v[0].rhw = 1.0f;
            v[0].color = centerColor;

            for (int i = 0; i <= SEG; ++i)
            {
                float ca, sa;
                GetLUTCosSin(i, SEG, ca, sa);

                float x = cx + ca * radius;
                float y = cy + sa * radius;

                v[i + 1].x = x;
                v[i + 1].y = y;
                v[i + 1].z = 0.0f;
                v[i + 1].rhw = 1.0f;
                v[i + 1].color = edgeColor;
            }

            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEG, v, sizeof(VERTEX));
        };

    float tSlow = t * 0.25f;

    float cx1 = SCREEN_W * 0.20f + sinf(tSlow * 0.7f) * 40.0f;
    float cy1 = SCREEN_H * 0.25f + cosf(tSlow * 0.9f) * 30.0f;
    DrawGlow(
        cx1, cy1, 140.0f,
        D3DCOLOR_ARGB(40, 40, 160, 80),
        D3DCOLOR_ARGB(0, 0, 0, 0)
    );

    float cx2 = SCREEN_W * 0.70f + cosf(tSlow * 0.6f + 1.3f) * 50.0f;
    float cy2 = SCREEN_H * 0.50f + sinf(tSlow * 0.8f + 0.5f) * 40.0f;
    DrawGlow(
        cx2, cy2, 180.0f,
        D3DCOLOR_ARGB(36, 30, 200, 110),
        D3DCOLOR_ARGB(0, 0, 0, 0)
    );

    float cx3 = SCREEN_W * 0.45f + sinf(tSlow * 0.4f + 2.1f) * 60.0f;
    float cy3 = SCREEN_H * 0.85f;
    DrawGlow(
        cx3, cy3, 160.0f,
        D3DCOLOR_ARGB(30, 10, 140, 70),
        D3DCOLOR_ARGB(0, 0, 0, 0)
    );

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// =====================================================================================
// Orb glow (now tinted when color-edit is active in PALETTE menu)
// =====================================================================================
static void DrawOrbGlow(float t, const MenuState& ms)
{
    (void)t; // not used right now, but kept in signature for future

    VERTEX centerProj;
    ProjectToScreen(0.0f, 0.0f, 3.8f, 0, centerProj);
    float cx = centerProj.x;
    float cy = centerProj.y;

    // Slightly reduced segments; still looks smooth, costs less
    const int   SEG = 24;
    const float RADIUS = 220.0f;

    VERTEX v[SEG + 2];

    float r = 0.16f, g = 0.55f, b = 0.16f; // default greenish
    if (ms.colorEditActive && ms.uiState == UI_SUB_PALETTE)
    {
        // Dimmer, slightly desaturated glow based on picker color
        HSVToRGB(ms.colorHue, ms.colorSat * 0.7f, ms.colorVal * 0.5f, r, g, b);
    }

    v[0].x = cx;
    v[0].y = cy;
    v[0].z = 0.0f;
    v[0].rhw = 1.0f;
    v[0].color = D3DCOLOR_COLORVALUE(r, g, b, 1.0f);

    for (int i = 0; i <= SEG; ++i)
    {
        float ca, sa;
        GetLUTCosSin(i, SEG, ca, sa);

        float x = cx + ca * RADIUS;
        float y = cy + sa * RADIUS;

        v[i + 1].x = x;
        v[i + 1].y = y;
        v[i + 1].z = 0.0f;
        v[i + 1].rhw = 1.0f;
        v[i + 1].color = D3DCOLOR_XRGB(0, 0, 0);
    }

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEG, v, sizeof(VERTEX));
}

// =====================================================================================
// Orb (now driven by MenuState HSV when color-edit is active in PALETTE)
// =====================================================================================
static void DrawOrb(float t, const MenuState& ms)
{
    const int   SEG = 40;
    const float innerR = 0.8f;
    const float outerR = 1.6f;
    const float baseZ = 3.8f;

    // Slightly gentler breathing so the orb doesn't "pump" as hard.
    float breathe = 1.0f + 0.03f * sinf(t * 0.9f);
    float innerR_mod = innerR * breathe;
    float outerR_mod = outerR * breathe;

    VERTEX ring[(SEG + 1) * 2];

    // Default "classic" warm-to-cool green
    float inner_r, inner_g, inner_b;

    if (ms.colorEditActive && ms.uiState == UI_SUB_PALETTE)
    {
        // Use HSV picked color, with slight breathing on value
        float vPulse = ms.colorVal * (0.9f + 0.1f * sinf(t * 0.9f));
        HSVToRGB(ms.colorHue, ms.colorSat, vPulse, inner_r, inner_g, inner_b);
    }
    else
    {
        float pulse = 0.5f + 0.5f * sinf(t * 1.3f);

        const float ia_r = 1.00f, ia_g = 0.94f, ia_b = 0.70f; // warm
        const float ib_r = 0.70f, ib_g = 1.00f, ib_b = 0.65f; // cooler green

        inner_r = ia_r * (1.0f - pulse) + ib_r * pulse;
        inner_g = ia_g * (1.0f - pulse) + ib_g * pulse;
        inner_b = ia_b * (1.0f - pulse) + ib_b * pulse;

        // Value still respects brightness so the orb doesn’t blow out
        float scale = ms.colorVal;
        inner_r *= scale;
        inner_g *= scale;
        inner_b *= scale;
    }

    for (int i = 0; i <= SEG; ++i)
    {
        float ang = (float)i / (float)SEG * 6.28318f;

        float wob = 0.25f * sinf(ang * 3.0f + t * 1.2f);
        float wob2 = 0.04f * sinf(ang * 7.0f + t * 2.1f);

        // *** MAIN CHANGE: much lighter squash, closer to a circle ***
        float squash = 1.02f + 0.01f * sinf(t * 0.8f);

        float ca = cosf(ang);
        float sa = sinf(ang);

        // OUTER RING ---------------------------------------------------------
        float xO = outerR_mod * ca;
        float yO = outerR_mod * sa * squash;
        float zO = baseZ + wob * 0.5f + wob2;

        DWORD outerCol;
        if (ms.colorEditActive && ms.uiState == UI_SUB_PALETTE)
        {
            // Outer ring: darker version of inner color with subtle swirl
            float swirl = 0.7f + 0.3f * sinf(ang * 2.0f + t * 0.7f);
            float orr = inner_r * 0.6f * swirl;
            float ogg = inner_g * 0.6f * swirl;
            float obb = inner_b * 0.6f * swirl;
            outerCol = D3DCOLOR_COLORVALUE(orr, ogg, obb, 1.0f);
        }
        else
        {
            float swirl = 0.6f + 0.2f * sinf(ang * 2.0f + t * 0.7f);
            outerCol = D3DCOLOR_COLORVALUE(
                0.16f * swirl,
                0.78f * swirl,
                0.16f * swirl,
                1.0f
            );
        }

        ProjectToScreen(xO, yO, zO, outerCol, ring[i * 2 + 0]);

        // INNER RING ---------------------------------------------------------
        float xI = innerR_mod * ca;
        float yI = innerR_mod * sa * squash;
        float zI = baseZ + wob;

        DWORD innerCol = D3DCOLOR_COLORVALUE(
            inner_r,
            inner_g,
            inner_b,
            1.0f
        );

        ProjectToScreen(xI, yI, zI, innerCol, ring[i * 2 + 1]);
    }

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, SEG * 2, ring, sizeof(VERTEX));
}


// =====================================================================================
// Blades
// =====================================================================================
static void DrawBladeRow(
    int         visualIndex,
    int         selectedIndex,
    float       t,
    const char* label)
{
    int actualRow = MENU_COUNT - 1 - visualIndex;

    float baseX = 2.0f;
    float baseY = (visualIndex - 2) * 0.63f;
    float baseZ = 4.1f + visualIndex * 0.18f;

    bool  selected = (actualRow == selectedIndex);
    float width = selected ? 2.6f : 2.2f;
    float height = 0.35f;

    float yaw = -0.7f + 0.15f * sinf(t * 0.7f + actualRow * 0.4f);
    float wob = 0.10f * sinf(t * 1.3f + actualRow);
    baseY += wob;

    if (selected)
    {
        baseZ -= 0.30f;
        baseY += 0.10f;
    }

    float c = cosf(yaw);
    float s = sinf(yaw);

    // Slight gradient: bright top, darker bottom, plus a hotter color when selected.
    DWORD colTopBase = D3DCOLOR_XRGB(70, 200, 70);
    DWORD colBottomBase = D3DCOLOR_XRGB(5, 25, 5);
    DWORD colTopSelect = D3DCOLOR_XRGB(255, 255, 160);
    DWORD colBottomSelect = D3DCOLOR_XRGB(60, 120, 40);

    float px[4] = { -width * 0.5f,  width * 0.5f, -width * 0.5f,  width * 0.5f };
    float py[4] = { height * 0.5f, height * 0.5f,-height * 0.5f,-height * 0.5f };

    VERTEX v[4];

    for (int i = 0; i < 4; ++i)
    {
        float x = px[i];
        float y = py[i];
        float z = 0.0f;

        float rx = x * c + z * s;
        float rz = -x * s + z * c;

        float wx = baseX + rx;
        float wy = baseY + y;
        float wz = baseZ + rz;

        bool topEdge = (i == 0 || i == 1);

        DWORD col;
        if (selected)
        {
            col = topEdge ? colTopSelect : colBottomSelect;
        }
        else
        {
            col = topEdge ? colTopBase : colBottomBase;
        }

        ProjectToScreen(wx, wy, wz, col, v[i]);
    }

    // Small drop shadow *only* for the selected blade to make it pop
    if (selected)
    {
        VERTEX shadow[4];
        for (int i = 0; i < 4; ++i)
        {
            shadow[i] = v[i];
            shadow[i].x += 4.0f;
            shadow[i].y += 4.0f;
            shadow[i].color = D3DCOLOR_ARGB(120, 0, 0, 0);
        }

        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, shadow, sizeof(VERTEX));
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    }

    // Main blade
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VERTEX));

    if (!label || !label[0])
        return;

    int len = (int)strlen(label);
    if (len <= 0)
        return;

    float ex0 = v[0].x;
    float ey0 = v[0].y;
    float ex1 = v[1].x;
    float ey1 = v[1].y;

    float dx = ex1 - ex0;
    float dy = ey1 - ey0;

    float edgeLen = sqrtf(dx * dx + dy * dy);
    if (edgeLen < 1.0f)
        edgeLen = 1.0f;

    float baseScale = 2.0f;
    float charPixelWidth = 6.0f * baseScale;
    float maxCharsFit = edgeLen / charPixelWidth;
    float scale = baseScale;

    if (maxCharsFit < (float)len)
        scale = baseScale * (maxCharsFit / (float)len);

    DWORD textColor = D3DCOLOR_XRGB(220, 255, 220);

    float ux = dx / edgeLen;
    float uy = dy / edgeLen;

    float textWidth = (float)len * 6.0f * scale;
    float startOffset = (edgeLen - textWidth) * 0.5f;

    for (int i = 0; i < len; ++i)
    {
        char cbuf[2] = { label[i], 0 };

        float offs = startOffset + (i + 0.5f) * 6.0f * scale;
        float cx = ex0 + ux * offs;
        float cy = ey0 + uy * offs;

        float drawX = cx - 3.0f * scale;
        float drawY = cy + 1.0f * scale;

        DrawText(drawX, drawY, cbuf, scale, textColor);
    }
}

// =====================================================================================
// Plasma filaments inside orb (white / blue / violet + pulsing core with forks)
// =====================================================================================
static void DrawOrbPlasma(float t, const MenuState& /*ms*/)
{
    // Match the 3D orb center in screen space
    VERTEX centerProj;
    ProjectToScreen(0.0f, 0.0f, 3.8f, 0, centerProj);
    float cx = centerProj.x;
    float cy = centerProj.y;

    // Geometry / radius settings
    const int   SEGMENTS = 12;    // points per filament
    const int   FILAMENT_COUNT = 18;    // number of filaments
    const float R_CORE = 34.0f; // radius of bright center orb
    const float R_START = 38.0f; // where filaments *begin* (just outside core)
    const float R_END = 82.0f;

    // Enable additive blending for glow
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);

    float tSlow = t * 0.30f;
    float tNoise = t * 0.80f;

    // -------------------------------------------------------------------------
    // Filaments
    // -------------------------------------------------------------------------
    for (int fi = 0; fi < FILAMENT_COUNT; ++fi)
    {
        // Base angle for this filament around the center
        float baseAng = (float)fi / (float)FILAMENT_COUNT * 6.28318f;

        // Small jitter at the root so they don't all overlap
        float rootJitter = 4.0f * sinf(tSlow * 1.1f + fi * 1.7f);

        // Gentle branching offset
        float branchOffset = 0.04f * sinf(tSlow * 0.7f + fi * 2.17f);

        // Choose a cool hue between blue and violet (no warm colors)
        float hueBase = 220.0f + 40.0f * sinf(tSlow * 0.6f + fi * 0.4f); // 220-260°
        float base_r, base_g, base_b;
        HSVToRGB(hueBase, 0.85f, 1.0f, base_r, base_g, base_b);

        // Main filament body
        for (int s = 0; s < SEGMENTS; ++s)
        {
            float u0 = (float)s / (float)SEGMENTS;
            float u1 = (float)(s + 1) / (float)SEGMENTS;

            // Radii start *outside* the core so we never see a center origin
            float r0 = R_START + (R_END - R_START) * u0;
            float r1 = R_START + (R_END - R_START) * u1;

            // Smooth, not-too-big wobble – “plasma curve” not worm
            float wobble0 =
                0.06f * sinf(u0 * 5.0f + tNoise * 1.2f + fi) +
                0.03f * sinf(u0 * 9.0f - tNoise * 0.7f + fi * 1.3f);
            float wobble1 =
                0.06f * sinf(u1 * 5.0f + tNoise * 1.2f + fi) +
                0.03f * sinf(u1 * 9.0f - tNoise * 0.7f + fi * 1.3f);

            float ang0 = baseAng + wobble0 + branchOffset * u0;
            float ang1 = baseAng + wobble1 + branchOffset * u1;

            float x0 = cx + cosf(ang0) * r0;
            float y0 = cy + sinf(ang0) * r0 + rootJitter * (1.0f - u0);

            float x1 = cx + cosf(ang1) * r1;
            float y1 = cy + sinf(ang1) * r1;

            // Intensity: white near center, then fade to blue/violet
            float i0 = sinf(3.14159f * u0);
            float i1 = sinf(3.14159f * u1);
            if (i0 < 0.0f) i0 = 0.0f;
            if (i1 < 0.0f) i1 = 0.0f;

            // Blend toward white close to the core
            float whiteMix0 = (u0 < 0.3f) ? (1.0f - (u0 / 0.3f)) : 0.0f;
            float whiteMix1 = (u1 < 0.3f) ? (1.0f - (u1 / 0.3f)) : 0.0f;

            float fr0 = base_r * (1.0f - whiteMix0) + 1.0f * whiteMix0;
            float fg0 = base_g * (1.0f - whiteMix0) + 1.0f * whiteMix0;
            float fb0 = base_b * (1.0f - whiteMix0) + 1.0f * whiteMix0;

            float fr1 = base_r * (1.0f - whiteMix1) + 1.0f * whiteMix1;
            float fg1 = base_g * (1.0f - whiteMix1) + 1.0f * whiteMix1;
            float fb1 = base_b * (1.0f - whiteMix1) + 1.0f * whiteMix1;

            i0 = 0.4f + 0.7f * i0;
            i1 = 0.4f + 0.7f * i1;

            VERTEX v[2];
            v[0].x = x0;
            v[0].y = y0;
            v[0].z = 0.0f;
            v[0].rhw = 1.0f;
            v[0].color = D3DCOLOR_COLORVALUE(
                fr0 * i0,
                fg0 * i0,
                fb0 * i0,
                1.0f
            );

            v[1].x = x1;
            v[1].y = y1;
            v[1].z = 0.0f;
            v[1].rhw = 1.0f;
            v[1].color = D3DCOLOR_COLORVALUE(
                fr1 * i1,
                fg1 * i1,
                fb1 * i1,
                1.0f
            );

            g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(VERTEX));
        }

        // -----------------------------------------------------------------
        // Simple fork: a short side-branch around mid-radius
        // -----------------------------------------------------------------
        float uMid = 0.55f;
        float rMid = R_START + (R_END - R_START) * uMid;

        float wobMid =
            0.06f * sinf(uMid * 5.0f + tNoise * 1.2f + fi) +
            0.03f * sinf(uMid * 9.0f - tNoise * 0.7f + fi * 1.3f);

        float angMid = baseAng + wobMid + branchOffset * uMid;

        float xMid = cx + cosf(angMid) * rMid;
        float yMid = cy + sinf(angMid) * rMid + rootJitter * 0.5f;

        // Branch goes off at ± angle alternating per filament
        float branchSign = (fi & 1) ? 1.0f : -1.0f;
        float branchAng = angMid + branchSign * 0.55f;
        float branchLen = 18.0f;

        float xFork = xMid + cosf(branchAng) * branchLen;
        float yFork = yMid + sinf(branchAng) * branchLen;

        float br_i0 = 0.9f;
        float br_i1 = 0.2f; // fade out at tip

        VERTEX bf[2];
        bf[0].x = xMid;
        bf[0].y = yMid;
        bf[0].z = 0.0f;
        bf[0].rhw = 1.0f;
        bf[0].color = D3DCOLOR_COLORVALUE(
            base_r * br_i0,
            base_g * br_i0,
            base_b * br_i0,
            1.0f
        );

        bf[1].x = xFork;
        bf[1].y = yFork;
        bf[1].z = 0.0f;
        bf[1].rhw = 1.0f;
        bf[1].color = D3DCOLOR_COLORVALUE(
            base_r * br_i1,
            base_g * br_i1,
            base_b * br_i1,
            1.0f
        );

        g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, 1, bf, sizeof(VERTEX));
    }

    // -------------------------------------------------------------------------
    // Pulsing / flickering central orb on TOP of filament roots (blue/violet)
    // -------------------------------------------------------------------------
    {
        const int CENTER_SEG = 24;

        float pulse = 1.0f + 0.10f * sinf(t * 2.2f);
        float radius = R_CORE * pulse;

        // High-frequency flicker for “electric” look
        float flicker = 0.80f
            + 0.25f * sinf(t * 7.3f)
            + 0.20f * sinf(t * 13.7f);
        if (flicker < 0.4f) flicker = 0.4f;
        if (flicker > 1.4f) flicker = 1.4f;

        // Core color: slightly different hue than filaments but same cool band
        float coreHue = 235.0f + 20.0f * sinf(tSlow * 0.9f);
        float core_r, core_g, core_b;
        HSVToRGB(coreHue, 0.80f, 1.0f, core_r, core_g, core_b);

        core_r *= flicker;
        core_g *= flicker;
        core_b *= flicker;

        VERTEX fan[CENTER_SEG + 2];

        // Center – brightest
        fan[0].x = cx;
        fan[0].y = cy;
        fan[0].z = 0.0f;
        fan[0].rhw = 1.0f;
        fan[0].color = D3DCOLOR_COLORVALUE(core_r, core_g, core_b, 1.0f);

        // Rim – softer, to feather into main orb ring
        for (int i = 0; i <= CENTER_SEG; ++i)
        {
            float ang = (float)i / (float)CENTER_SEG * 6.28318f;
            float x = cx + cosf(ang) * radius;
            float y = cy + sinf(ang) * radius;

            fan[i + 1].x = x;
            fan[i + 1].y = y;
            fan[i + 1].z = 0.0f;
            fan[i + 1].rhw = 1.0f;
            fan[i + 1].color = D3DCOLOR_COLORVALUE(
                core_r * 0.35f,
                core_g * 0.35f,
                core_b * 0.35f,
                1.0f
            );
        }

        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, CENTER_SEG, fan, sizeof(VERTEX));
    }

    // Restore blending
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}


// =====================================================================================
// Left-side panel: title + compact, state-based helper
// =====================================================================================
static void DrawLeftLabels(const MenuState& ms)
{
    float orbCenterY = SCREEN_H * 0.50f;
    float titleX = 40.0f;
    float titleY = orbCenterY - 16.0f;

    DrawTextShadow(
        titleX,
        titleY,
        "XBOX RGB CONTROL",
        2.4f,
        D3DCOLOR_XRGB(255, 255, 255)
    );

    float infoX = titleX;
    float infoY = titleY + 28.0f;
    float lineStep = 18.0f;

    DWORD mainColor = D3DCOLOR_XRGB(200, 240, 255);
    DWORD subtleColor = D3DCOLOR_XRGB(160, 200, 220);

    char  line[128];
    char  bar[32];
    int   pos;

    const char* modeName = GetModeName(ms.mode);

    auto rowPrefix = [&](int rowIdx) -> const char*
        {
            return (ms.subIndex == rowIdx) ? ">" : " ";
        };

    // ROOT
    if (ms.uiState == UI_ROOT)
    {
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, "Mode: ");
        append_str(line, sizeof(line), pos, modeName ? modeName : "UNKNOWN");
        append_str(line, sizeof(line), pos, " (");
        append_int(line, sizeof(line), pos, ms.mode);
        append_str(line, sizeof(line), pos, ")");
        DrawText(infoX, infoY, line, 1.6f, mainColor);

        int brightPct = (ms.brightness * 100 + 127) / 255;
        BuildBar(bar, sizeof(bar), ms.brightness, 255);
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, "Brightness ");
        append_int(line, sizeof(line), pos, brightPct);
        append_str(line, sizeof(line), pos, "% ");
        append_str(line, sizeof(line), pos, bar);
        DrawText(infoX, infoY + lineStep, line, 1.4f, subtleColor);

        DrawText(
            infoX,
            infoY + 3 * lineStep,
            "U/D = select blade   A = open   B = exit",
            1.4f,
            subtleColor
        );
        DrawText(
            infoX,
            infoY + 4 * lineStep,
            "X: save   Y: load   A (submenu): preview B: Exit",
            1.4f,
            subtleColor
        );
        DrawText(
            infoX,
            infoY + 5 * lineStep,
            "CUSTOM modes are edited via Web UI.",
            1.4f,
            subtleColor
        );
        return;
    }

    float ctxY = infoY;

    switch (ms.uiState)
    {
    case UI_SUB_MODES:
    {
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(0));
        append_str(line, sizeof(line), pos, " Mode: ");
        append_str(line, sizeof(line), pos, modeName ? modeName : "UNKNOWN");
        append_str(line, sizeof(line), pos, " (");
        append_int(line, sizeof(line), pos, ms.mode);
        append_str(line, sizeof(line), pos, ")");
        DrawText(infoX, ctxY, line, 1.6f, mainColor);

        int spPct = (ms.speed * 100 + 127) / 255;
        BuildBar(bar, sizeof(bar), ms.speed, 255);
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(1));
        append_str(line, sizeof(line), pos, " Speed: ");
        append_int(line, sizeof(line), pos, spPct);
        append_str(line, sizeof(line), pos, "% ");
        append_str(line, sizeof(line), pos, bar);
        DrawText(infoX, ctxY + lineStep, line, 1.4f, subtleColor);

        int inPct = (ms.intensity * 100 + 127) / 255;
        BuildBar(bar, sizeof(bar), ms.intensity, 255);
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(2));
        append_str(line, sizeof(line), pos, " Intensity: ");
        append_int(line, sizeof(line), pos, inPct);
        append_str(line, sizeof(line), pos, "% ");
        append_str(line, sizeof(line), pos, bar);
        DrawText(infoX, ctxY + 2 * lineStep, line, 1.4f, subtleColor);

        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(3));
        append_str(line, sizeof(line), pos, " Width: ");
        append_int(line, sizeof(line), pos, ms.width);
        DrawText(infoX, ctxY + 3 * lineStep, line, 1.4f, subtleColor);

        DrawText(
            infoX,
            ctxY + 5 * lineStep,
            "MODES: U/D = select row   L/R = change   A = preview   B = back",
            1.4f,
            subtleColor
        );
        // NOTE: color editing now lives in PALETTE submenu, not MODES,
        // so we intentionally do NOT show the right-stick hint here anymore.
        break;
    }

    case UI_SUB_BRIGHTNESS:
    {
        int bpct = (ms.brightness * 100 + 127) / 255;
        BuildBar(bar, sizeof(bar), ms.brightness, 255);
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, "> Brightness: ");
        append_int(line, sizeof(line), pos, bpct);
        append_str(line, sizeof(line), pos, "% ");
        append_str(line, sizeof(line), pos, bar);
        DrawText(infoX, ctxY, line, 1.6f, mainColor);

        DrawText(
            infoX,
            ctxY + 2 * lineStep,
            "D-Pad L/R: tap=fine, hold=coarse   A=preview   B=back",
            1.4f,
            subtleColor
        );
        break;
    }

    case UI_SUB_CHANNELS:
    {
        for (int ch = 0; ch < 4; ++ch)
        {
            pos = 0; line[0] = '\0';
            append_str(line, sizeof(line), pos, rowPrefix(ch));
            append_str(line, sizeof(line), pos, " CH");
            append_int(line, sizeof(line), pos, ch + 1);
            append_str(line, sizeof(line), pos, " Count: ");
            append_int(line, sizeof(line), pos, ms.chCount[ch]);

            DrawText(
                infoX,
                ctxY + ch * lineStep,
                line,
                ch == ms.subIndex ? 1.6f : 1.4f,
                ch == ms.subIndex ? mainColor : subtleColor
            );
        }

        for (int ch = 0; ch < 4; ++ch)
        {
            pos = 0; line[0] = '\0';
            append_str(line, sizeof(line), pos, rowPrefix(4 + ch));
            append_str(line, sizeof(line), pos, " CH");
            append_int(line, sizeof(line), pos, ch + 1);
            append_str(line, sizeof(line), pos, " Reverse: ");
            append_str(line, sizeof(line), pos, ms.reverse[ch] ? "ON" : "OFF");

            DrawText(
                infoX,
                ctxY + (4 + ch) * lineStep,
                line,
                (4 + ch) == ms.subIndex ? 1.6f : 1.4f,
                (4 + ch) == ms.subIndex ? mainColor : subtleColor
            );
        }

        DrawText(
            infoX,
            ctxY + 9 * lineStep,
            "CHANNELS: U/D = row   L/R = change   A = preview   B = back",
            1.4f,
            subtleColor
        );
        DrawText(
            infoX,
            ctxY + 10 * lineStep,
            "Advanced mapping lives in the Web UI.",
            1.4f,
            subtleColor
        );
        break;
    }

    case UI_SUB_PALETTE:
    {
        // Row 0: palette count
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(0));
        append_str(line, sizeof(line), pos, " Palette slots: ");
        append_int(line, sizeof(line), pos, ms.paletteCnt);
        append_str(line, sizeof(line), pos, " (1..4)");
        DrawText(infoX, ctxY, line, 1.6f, mainColor);

        // Row 1: which slot is selected for editing
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(1));
        append_str(line, sizeof(line), pos, " Palette slot: ");
        append_int(line, sizeof(line), pos, ms.paletteIndex + 1);
        append_str(line, sizeof(line), pos, " / ");
        append_int(line, sizeof(line), pos, ms.paletteCnt);
        DrawText(infoX, ctxY + lineStep, line, 1.4f, subtleColor);

        DrawText(
            infoX,
            ctxY + 3 * lineStep,
            "U/D: choose row   L/R: adjust   A: preview   B: back",
            1.4f,
            subtleColor
        );

        DrawText(
            infoX,
            ctxY + 4 * lineStep,
            "Left Stick for color right stick for hue",
            1.4f,
            subtleColor
        );

        DrawText(
            infoX,
            ctxY + 5 * lineStep,
            "Orb color is what will be sent to XBOX-RGB.",
            1.4f,
            subtleColor
        );
        break;
    }

    case UI_SUB_SYSTEM:
    {
        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(0));
        append_str(line, sizeof(line), pos, " Master power: ");
        append_str(line, sizeof(line), pos, ms.masterOff ? "OFF" : "ON");
        DrawText(infoX, ctxY, line, 1.6f, mainColor);

        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(1));
        append_str(line, sizeof(line), pos, " Resume on boot: ");
        append_str(line, sizeof(line), pos, ms.resumeOn ? "ON" : "OFF");
        DrawText(infoX, ctxY + lineStep, line, 1.4f, subtleColor);

        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(2));
        append_str(line, sizeof(line), pos, " CPU indicator: ");
        append_str(line, sizeof(line), pos, ms.enableCpu ? "ON" : "OFF");
        DrawText(infoX, ctxY + 2 * lineStep, line, 1.4f, subtleColor);

        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(3));
        append_str(line, sizeof(line), pos, " FAN indicator: ");
        append_str(line, sizeof(line), pos, ms.enableFan ? "ON" : "OFF");
        DrawText(infoX, ctxY + 3 * lineStep, line, 1.4f, subtleColor);

        pos = 0; line[0] = '\0';
        append_str(line, sizeof(line), pos, rowPrefix(4));
        append_str(line, sizeof(line), pos, " Kratos mode: ");
        append_str(line, sizeof(line), pos, ms.kratosMode ? "ON" : "OFF");
        DrawText(infoX, ctxY + 4 * lineStep, line, 1.4f, subtleColor);

        DrawText(
            infoX,
            ctxY + 6 * lineStep,
            "SYSTEM: U/D = select   L/R = toggle   A = preview   B = back",
            1.4f,
            subtleColor
        );
        break;
    }

    default:
        break;
    }
}

// =====================================================================================
// PUBLIC: RenderFrame
// =====================================================================================
void RenderFrame(unsigned long frame, bool devFound, unsigned short /*buttons*/, const MenuState& ms)
{
    float t = frame * 0.03f;

    DWORD bg = D3DCOLOR_XRGB(0, 0, 0);

    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, bg, 1.0f, 0);
    g_pDevice->BeginScene();

    DrawBackgroundPlasma(t);
    DrawOrbGlow(t, ms);
    DrawOrb(t, ms);
    DrawOrbPlasma(t, ms);

    for (int vr = 0; vr < MENU_COUNT; ++vr)
    {
        const char* label = GetMenuLabel(MENU_COUNT - 1 - vr);
        DrawBladeRow(vr, ms.rootIndex, t, label);
    }

    DrawLeftLabels(ms);

    DrawTextShadow(
        20.0f,
        30.0f,
        devFound ? "DEVICE: XBOX RGB FOUND" : "DEVICE: SEARCHING...",
        1.6f,
        devFound ? D3DCOLOR_XRGB(80, 255, 80) : D3DCOLOR_XRGB(255, 255, 255)
    );

    // Status HUD (e.g., "Config saved")
    if (g_statusTimer > 0)
    {
        g_statusTimer--;

        DrawTextShadow(
            20.0f,
            60.0f,
            g_statusText,
            1.3f,
            D3DCOLOR_XRGB(200, 255, 200)
        );
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// =====================================================================================
// Flash helper – now a no-op (no cyan flash)
// =====================================================================================
void Renderer_TriggerFlash(int /*frames*/)
{
    // Intentionally no-op: visual preview flash removed.
}

// =====================================================================================
// Initialize D3D (fixed 640x480 + vsync)
// =====================================================================================
long InitD3D()
{
    g_pD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_pD3D) return -1;

    D3DPRESENT_PARAMETERS p;
    ZeroMemory(&p, sizeof(p));
    p.BackBufferWidth = (UINT)SCREEN_W;
    p.BackBufferHeight = (UINT)SCREEN_H;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.BackBufferCount = 1;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    p.Windowed = FALSE;
    p.EnableAutoDepthStencil = FALSE;
    p.FullScreen_RefreshRateInHz = 60;
    p.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (FAILED(g_pD3D->CreateDevice(
        0,
        D3DDEVTYPE_HAL,
        NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &p,
        &g_pDevice)))
    {
        return -1;
    }

    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetVertexShader(FVF_VERTEX);

    // One-time trig LUT build for all circular effects
    InitTrigLUT();

    return 0;
}
