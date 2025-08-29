#include "RGBCtrl.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <math.h>
#include <esp_system.h>  // esp_random

// Use the existing WiFiMgr server; no separate server objects needed.
namespace WiFiMgr { AsyncWebServer& getServer(); }

// -------------------- Build / Branding --------------------
static const char* APP_VERSION   = "1.5.0"; // shown in footer (internal only)
static const char* COPYRIGHT_TXT = "© Darkone Customs 2025";

// -------------------- Limits / Types --------------------
static const uint16_t MAX_PER_CH = 50;        // per requirement
static const uint8_t  NUM_CH     = 4;         // CH1..CH4 only
static const uint16_t MAX_RING   = MAX_PER_CH * NUM_CH; // 200

namespace RGBCtrl {

// -------------------- Minimal RGB types --------------------
struct RgbColor {
  uint8_t R, G, B;
  RgbColor() : R(0), G(0), B(0) {}
  RgbColor(uint8_t r, uint8_t g, uint8_t b) : R(r), G(g), B(b) {}
};

// Linear color (0..1)
struct RgbLin {
  float r, g, b;
  RgbLin() : r(0), g(0), b(0) {}
  RgbLin(float R, float G, float B) : r(R), g(G), b(B) {}
};

// -------------------- Globals --------------------
// Orientation (clockwise ring):
//   CH1 = Front, CH2 = Left, CH3 = Rear, CH4 = Right
// Ring order is CH1 -> CH2 -> CH3 -> CH4; animations start at CH1 index 0.
static RGBCtrlPins PINS{1,2,3,4};

// If a strip is physically reversed, flip here (CH1..CH4).
static bool REVERSE[NUM_CH] = { false, false, false, false };

// Adafruit_NeoPixel strips (GRB, 800kHz)
static Adafruit_NeoPixel strip1(MAX_PER_CH, 1, NEO_GRB + NEO_KHZ800);
static Adafruit_NeoPixel strip2(MAX_PER_CH, 2, NEO_GRB + NEO_KHZ800);
static Adafruit_NeoPixel strip3(MAX_PER_CH, 3, NEO_GRB + NEO_KHZ800);
static Adafruit_NeoPixel strip4(MAX_PER_CH, 4, NEO_GRB + NEO_KHZ800);
static Adafruit_NeoPixel* STRIPS[NUM_CH] = { &strip1, &strip2, &strip3, &strip4 };

static Preferences prefs;
static String gBase = "/config";  // mount path

// ---- Config ----
struct AppConfig {
  uint16_t count[NUM_CH] = {50,50,50,50};
  uint8_t  brightness    = 180;  // 0..255 (global brightness)
  uint8_t  mode          = 4;    // default Rainbow
  uint8_t  speed         = 128;  // 0..255 (higher=faster)
  uint8_t  intensity     = 128;  // meaning depends on mode
  uint8_t  width         = 4;    // “segment width” / “gap”

  // --- Colors / Palette ---
  uint32_t colorA        = 0xFF0000; // #RRGGBB
  uint32_t colorB        = 0xFFA000; // #RRGGBB
  uint32_t colorC        = 0x00FF00; // optional
  uint32_t colorD        = 0x0000FF; // optional
  uint8_t  paletteCount  = 2;        // 1..4 -> how many of A..D are used

  bool     resumeOnBoot  = true;

  // SMBus toggles (UI checkboxes)
  bool     enableCpu     = true;
  bool     enableFan     = true;
} CFG;

static bool inPreview = false;

enum : uint8_t {
  MODE_SOLID = 0,
  MODE_BREATHE,
  MODE_COLOR_WIPE,
  MODE_LARSON,
  MODE_RAINBOW,
  MODE_THEATER,
  MODE_TWINKLE,
  MODE_COMET,
  MODE_METEOR,
  MODE_CLOCK_SPIN,
  MODE_PLASMA,
  MODE_FIRE,

  // palette-aware
  MODE_PALETTE_CYCLE,
  MODE_PALETTE_CHASE,

  MODE_COUNT
};

struct Seg { uint8_t ch; uint16_t count; };
static Seg segs[NUM_CH];

// timing
static uint32_t msPrev = 0;
static uint32_t frameCount = 0;
static float    dtSec = 0.016f;   // smoothed delta-t

// Fire state (max ring length)
static uint8_t heat[MAX_RING]; // 0..255 heat map

// track last applied brightness
static uint8_t lastAppliedBrightness = 0xFF;

// -------------------- Visual pipeline constants --------------------

// White balance (RGB gains, 1.0 = neutral)
static float WB_R = 1.00f, WB_G = 1.00f, WB_B = 1.00f;

// Global saturation (1.0 = neutral). We keep it close to 1 for authenticity.
static float SATURATION = 1.00f;

// Motion smoothing (0..1) — how quickly to follow new values. Lower = smoother.
static float SMOOTH_ALPHA = 0.20f;

// Night mode threshold (uses brightness slider)
static const uint8_t NIGHT_BRIGHT_THRESH = 32;

// -------------------- Linear shadow buffers --------------------
// Work buffer for current frame (linear 0..1)
static RgbLin bufCur[MAX_RING];
// Previous frame after output (for modes that fade from last frame)
static RgbLin bufPrev[MAX_RING];
// Smoothed previous (for motion smoothing)
static RgbLin bufSmoothPrev[MAX_RING];

// -------------------- Helpers --------------------
inline RgbColor rgbFrom24(uint32_t rgb) {
  return RgbColor((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF);
}
static inline RgbColor rgbFrom32(uint32_t c) {
  return RgbColor(uint8_t((c>>16)&0xFF), uint8_t((c>>8)&0xFF), uint8_t(c&0xFF));
}

static uint16_t ringLen() {
  return CFG.count[0] + CFG.count[1] + CFG.count[2] + CFG.count[3];
}
static void rebuildRingMap() {
  // Strict order CH1 -> CH2 -> CH3 -> CH4 (Front -> Left -> Rear -> Right)
  segs[0] = {0, CFG.count[0]};
  segs[1] = {1, CFG.count[1]};
  segs[2] = {2, CFG.count[2]};
  segs[3] = {3, CFG.count[3]};
}

// --- sRGB <-> linear helpers ---
static inline float srgbToLinear1(float c) {
  // c in 0..1
  if (c <= 0.04045f) return c / 12.92f;
  return powf((c + 0.055f) / 1.055f, 2.4f);
}
static inline float linearToSrgb1(float x) {
  // x in 0..1
  if (x <= 0.0031308f) return 12.92f * x;
  return 1.055f * powf(x, 1.0f/2.4f) - 0.055f;
}
static inline RgbLin toLin(const RgbColor& c){
  return RgbLin(
    srgbToLinear1(c.R/255.0f),
    srgbToLinear1(c.G/255.0f),
    srgbToLinear1(c.B/255.0f)
  );
}
static inline RgbLin toLin24(uint32_t rgb24){
  return toLin(rgbFrom24(rgb24));
}
static inline RgbColor toSRGB8(const RgbLin& c){
  float r = linearToSrgb1(fmaxf(0.f, fminf(1.f, c.r)));
  float g = linearToSrgb1(fmaxf(0.f, fminf(1.f, c.g)));
  float b = linearToSrgb1(fmaxf(0.f, fminf(1.f, c.b)));
  return RgbColor(uint8_t(r*255.f + 0.5f), uint8_t(g*255.f + 0.5f), uint8_t(b*255.f + 0.5f));
}

// Temporal dither (small +/- 0.5 LSB based on pixel index + frame)
static inline float tDither(uint16_t idx){
  // integer hash → [0,1)
  uint32_t h = (uint32_t)idx * 1664525u + frameCount * 1013904223u;
  h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
  // convert to -0.5..+0.5 of 1/255
  float u = (h & 0xFFFFFF) / float(0x1000000);
  return (u - 0.5f) * (1.0f/255.0f);
}

// Luma for saturation ops (linear Rec.709)
static inline float luma(const RgbLin& c){
  return 0.2126f*c.r + 0.7152f*c.g + 0.0722f*c.b;
}

// White balance + saturation + tone map (in linear)
static inline RgbLin postProcessLin(RgbLin c, bool nightMode){
  // white balance
  c.r *= WB_R; c.g *= WB_G; c.b *= WB_B;

  // saturation
  float Y = luma(c);
  float sat = SATURATION;
  if (nightMode) sat *= 0.95f; // tiny desat at night
  c.r = Y + (c.r - Y)*sat;
  c.g = Y + (c.g - Y)*sat;
  c.b = Y + (c.b - Y)*sat;

  // gentle tone map (per-channel Reinhard), keeps highlights pleasing
  c.r = c.r / (1.0f + c.r);
  c.g = c.g / (1.0f + c.g);
  c.b = c.b / (1.0f + c.b);

  // tiny black lift in night mode so ultra dark isn’t crushed
  if (nightMode){
    const float lift = 0.010f;
    c.r = fmaxf(0.f, c.r - lift) / (1.f - lift);
    c.g = fmaxf(0.f, c.g - lift) / (1.f - lift);
    c.b = fmaxf(0.f, c.b - lift) / (1.f - lift);
  }
  return c;
}

// Utility clamping / mix
static inline RgbLin mix(const RgbLin& a, const RgbLin& b, float t){
  float s = 1.f - t;
  return RgbLin(a.r*s + b.r*t, a.g*s + b.g*t, a.b*s + b.b*t);
}
static inline RgbLin addClamp(const RgbLin& a, const RgbLin& b){
  return RgbLin(fminf(1.f, a.r+b.r), fminf(1.f, a.g+b.g), fminf(1.f, a.b+b.b));
}
static inline RgbLin mul(const RgbLin& a, float k){ return RgbLin(a.r*k, a.g*k, a.b*k); }

// Wheel (rainbow) in linear-light
static RgbLin wheelLin(uint8_t pos){
  RgbColor srgb;
  if (pos < 85)   srgb = RgbColor(255 - pos*3, pos*3, 0);
  else if (pos < 170){ pos -= 85; srgb = RgbColor(0, 255 - pos*3, pos*3); }
  else            { pos -= 170;   srgb = RgbColor(pos*3, 0, 255 - pos*3); }
  return toLin(srgb);
}

// ------------- Shadow buffer helpers -------------
static inline void clearBuf(RgbLin* buf){
  uint16_t L = ringLen();
  for (uint16_t i=0;i<L;++i) buf[i] = RgbLin(0,0,0);
}

static inline void dimAll(RgbLin* buf, float factor){
  uint16_t L = ringLen();
  for (uint16_t i=0;i<L;++i){
    buf[i].r *= factor;
    buf[i].g *= factor;
    buf[i].b *= factor;
  }
}

static void setRingLin(uint16_t idx, const RgbLin& c){
  uint16_t base = 0;
  for (uint8_t s=0; s<NUM_CH; ++s) {
    if (idx < base + segs[s].count) {
      uint16_t within = idx - base;
      uint16_t px = (REVERSE[segs[s].ch] && segs[s].count)
                    ? (segs[s].count - 1 - within)
                    : within;
      // write into shadow buffer (bufCur), *not* the strip yet:
      // We address ring index; actual strip index mapping handled in showRing().
      // Here we just store to bufCur by ring index directly:
      // (No-op here; actual set done outside because we keep a ring-linear buffer)
      // We'll just store into bufCur[idx].
      bufCur[idx] = c;
      return;
    }
    base += segs[s].count;
  }
}

// Add with max
static inline void paintMax(uint16_t idx, const RgbLin& c){
  RgbLin &d = bufCur[idx];
  d.r = fmaxf(d.r, c.r);
  d.g = fmaxf(d.g, c.g);
  d.b = fmaxf(d.b, c.b);
}

// Soft dot (anti-aliased) distributed to nearest 2 pixels
static void drawSoftDot(float pos, const RgbLin& c){
  uint16_t L = ringLen(); if (!L) return;
  while (pos < 0) pos += L;
  while (pos >= L) pos -= L;
  uint16_t i0 = (uint16_t)floorf(pos);
  uint16_t i1 = (i0 + 1) % L;
  float t = pos - floorf(pos); // 0..1
  RgbLin c0 = mul(c, 1.0f - t);
  RgbLin c1 = mul(c, t);
  paintMax(i0, c0);
  paintMax(i1, c1);
}

// Linear palette handling
static inline uint8_t clampPaletteCount(uint8_t n){
  if (n < 1) return 1; if (n > 4) return 4; return n;
}

static inline RgbLin lerpLin(const RgbLin& a, const RgbLin& b, float t){
  return RgbLin(a.r + (b.r - a.r)*t, a.g + (b.g - a.g)*t, a.b + (b.b - a.b)*t);
}

static void loadPaletteLin(uint8_t& n, RgbLin p[4]){
  n = clampPaletteCount(CFG.paletteCount);
  uint32_t src[4] = { CFG.colorA, CFG.colorB, CFG.colorC, CFG.colorD };
  for (uint8_t i=0;i<4;i++) p[i] = toLin24(src[i]);
}

// derive motion palette if only A is provided
static inline float fclampf(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }
static void rgb2hsv_lin(const RgbLin& c, float& h, float& s, float& v){
  // Approx via sRGB path (ok for companion derivation)
  RgbColor srgb = toSRGB8(c);
  float r = srgb.R/255.f, g = srgb.G/255.f, b = srgb.B/255.f;
  float mx = fmaxf(r, fmaxf(g,b)), mn = fminf(r, fminf(g,b));
  float d = mx - mn; v = mx; s = (mx <= 0.f) ? 0.f : d / mx;
  if (d == 0.f) { h = 0.f; return; }
  if (mx == r)      h = fmodf(((g-b)/d), 6.f);
  else if (mx == g) h = ((b-r)/d) + 2.f;
  else              h = ((r-g)/d) + 4.f;
  h /= 6.f; if (h < 0.f) h += 1.f;
}
static RgbLin hsv2rgb_lin(float h, float s, float v){
  float r,g,b;
  int i = int(h*6.f);
  float f = h*6.f - i;
  float p = v*(1.f - s);
  float q = v*(1.f - f*s);
  float t = v*(1.f - (1.f - f)*s);
  switch(i % 6){
    case 0: r=v; g=t; b=p; break;
    case 1: r=q; g=v; b=p; break;
    case 2: r=p; g=v; b=t; break;
    case 3: r=p; g=q; b=v; break;
    case 4: r=t; g=p; b=v; break;
    default:r=v; g=p; b=q; break;
  }
  return toLin(RgbColor(uint8_t(r*255), uint8_t(g*255), uint8_t(b*255)));
}
static void loadMotionPaletteLin(uint8_t& n, RgbLin p[4]){
  loadPaletteLin(n, p);
  if (n >= 2) return;
  float h,s,v; rgb2hsv_lin(p[0], h, s, v);
  float s1 = fclampf(s * 1.05f, 0.f, 1.f);
  float s2 = fclampf(s * 0.85f, 0.f, 1.f);
  float v1 = fclampf(v * 1.05f, 0.f, 1.f);
  float v2 = fclampf(v * 0.92f, 0.f, 1.f);
  p[0] = hsv2rgb_lin(h,                 s,  v);
  p[1] = hsv2rgb_lin(fmodf(h + 0.08f,1), s1, v1);
  p[2] = hsv2rgb_lin(fmodf(h + 0.33f,1), s2, v1);
  p[3] = hsv2rgb_lin(fmodf(h + 0.58f,1), s,  v2);
  n = 4;
}

static RgbLin samplePaletteLin(float x /*0..1*/, uint8_t n, const RgbLin p[4], uint8_t blend){
  if (n == 1) return p[0];
  float fx = fmodf(x, 1.0f); if (fx < 0) fx += 1.0f;
  float pos = fx * n;
  int i0 = (int)floorf(pos) % n;
  int i1 = (i0 + 1) % n;
  float t = pos - floorf(pos);
  if (blend == 0) return p[i0];
  float bw = (blend / 255.0f);
  return lerpLin(p[i0], p[i1], t*bw);
}

// -------------------- Speed mapping --------------------
static inline float smooth01(float t){ return t*t*(3.f - 2.f*t); } // smoothstep
static float speedToHz(uint8_t s, float minHz, float maxHz){
  float t = smooth01(s / 255.f);
  // exponential mapping for perceptual feel
  return minHz * powf(maxHz / minHz, t);
}

// -------------------- Output (strip write) --------------------
static void showRing() {
  uint16_t L = ringLen(); if (!L) return;

  bool night = (CFG.brightness <= NIGHT_BRIGHT_THRESH);

  // Motion smoothing
  for (uint16_t i=0;i<L;++i){
    bufSmoothPrev[i] = mix(bufSmoothPrev[i], bufCur[i], SMOOTH_ALPHA);
  }

  if (lastAppliedBrightness != CFG.brightness) {
    for (uint8_t s=0; s<NUM_CH; ++s) STRIPS[s]->setBrightness(CFG.brightness);
    lastAppliedBrightness = CFG.brightness;
  }

  // Write strips from smoothed buffer with post-process, gamma, dithering
  uint16_t base = 0;
  for (uint8_t s=0; s<NUM_CH; ++s){
    for (uint16_t i=0;i<segs[s].count;++i){
      uint16_t ringIdx = base + (REVERSE[segs[s].ch] ? (segs[s].count-1-i) : i);
      RgbLin c = postProcessLin(bufSmoothPrev[ringIdx], night);

      // sRGB encode + temporal dither (pre-quantization)
      float r = linearToSrgb1(fmaxf(0.f, fminf(1.f, c.r))) + tDither(ringIdx);
      float g = linearToSrgb1(fmaxf(0.f, fminf(1.f, c.g))) + tDither(ringIdx*3u+1u);
      float b = linearToSrgb1(fmaxf(0.f, fminf(1.f, c.b))) + tDither(ringIdx*7u+2u);

      uint8_t R = (uint8_t)fmaxf(0.f, fminf(255.f, r*255.f + 0.5f));
      uint8_t G = (uint8_t)fmaxf(0.f, fminf(255.f, g*255.f + 0.5f));
      uint8_t B = (uint8_t)fmaxf(0.f, fminf(255.f, b*255.f + 0.5f));

      STRIPS[segs[s].ch]->setPixelColor(i, R, G, B);
    }
    STRIPS[segs[s].ch]->show();
    base += segs[s].count;
  }

  // Keep a copy for next-frame trails if a mode relies on it
  for (uint16_t i=0;i<L;++i) bufPrev[i] = bufSmoothPrev[i];
}

// -------------------- Animations (write to bufCur) --------------------
static void animSolid() {
  uint16_t L = ringLen(); if (!L) return;
  RgbLin c = toLin24(CFG.colorA);
  for (uint16_t i=0;i<L;++i) bufCur[i] = c;
}

static void animBreathe() {
  uint16_t L = ringLen(); if (!L) return;
  float hz = speedToHz(CFG.speed, 0.05f, 1.5f);
  static float phase = 0.f; phase += dtSec * hz;
  float s = 0.5f + 0.5f * sinf(phase * 6.2831853f);
  float lvl = 0.12f + s * 0.88f; // keep some floor

  RgbLin base = toLin24(CFG.colorA);
  base = mul(base, lvl);
  for (uint16_t i=0;i<L;++i) bufCur[i] = base;
}

static void animColorWipe(bool forward=true) {
  uint16_t L = ringLen(); if (!L) return;
  clearBuf(bufCur);
  uint8_t n; RgbLin pal[4]; loadMotionPaletteLin(n, pal);

  float hz = speedToHz(CFG.speed, 0.2f, 8.0f);
  static float pos = 0.f;
  pos += dtSec * hz * L;
  if (!forward) pos = -pos;

  // colorful head
  float phase = frameCount * 0.003f + (CFG.speed/255.f)*0.4f;
  float head = fmodf(pos, (float)L);
  RgbLin c = samplePaletteLin((head/(float)L) + phase, n, pal, CFG.intensity);
  drawSoftDot(head, c);
}

static void animLarson() {
  uint16_t L = ringLen(); if (!L) return;
  // fade from previous to create trail
  memcpy(bufCur, bufPrev, sizeof(RgbLin)*L);
  float trail = 0.65f + (CFG.intensity/255.f)*0.30f; // 0.65..0.95 retain
  dimAll(bufCur, trail);

  uint8_t n; RgbLin pal[4]; loadMotionPaletteLin(n, pal);
  float hz = speedToHz(CFG.speed, 0.2f, 4.0f);
  static float t = 0.f; t += dtSec*hz;
  float pingpong = 0.5f - 0.5f*cosf(2.f*3.1415926f*t); // 0..1..0
  float head = pingpong * (L-1);

  float ph = frameCount * 0.006f;
  RgbLin c = samplePaletteLin((head/(float)L)+ph, n, pal, CFG.intensity);
  drawSoftDot(head, c);
}

static void animRainbow() {
  uint16_t L = ringLen(); if (!L) return;
  float hz = speedToHz(CFG.speed, 0.05f, 2.0f);
  static float off = 0.f; off += dtSec * hz * 256.f;
  for (uint16_t i=0;i<L;++i){
    uint8_t idx = (uint8_t)((i*256/L + (int)off) & 255);
    bufCur[i] = wheelLin(idx);
  }
}

static void animTheater() {
  uint16_t L = ringLen(); if (!L) return;
  // trail
  memcpy(bufCur, bufPrev, sizeof(RgbLin)*L);
  float retain = 0.60f + (CFG.intensity/255.f)*0.35f; // 0.60..0.95
  dimAll(bufCur, retain);

  uint8_t gap = (CFG.width < 1) ? 1 : CFG.width;
  float hz = speedToHz(CFG.speed, 0.5f, 12.0f);
  static float t = 0.f; t += dtSec*hz;
  float offset = fmodf(t*gap, (float)gap); // 0..gap

  uint8_t n; RgbLin pal[4]; loadMotionPaletteLin(n, pal);
  float phase = frameCount * 0.0045f;

  for (uint16_t i=0;i<L;i++){
    float cell = fmodf((i - offset + 1000.f*gap), (float)gap);
    if (cell < 0.5f){
      RgbLin c = samplePaletteLin((i/(float)L)+phase, n, pal, CFG.intensity);
      // soften dot a bit
      drawSoftDot(i, c);
    }
  }
}

static void animTwinkle() {
  uint16_t L = ringLen(); if (!L) return;
  memcpy(bufCur, bufPrev, sizeof(RgbLin)*L);
  float retain = 0.70f; // steady fade
  dimAll(bufCur, retain);

  uint16_t pops = 1 + (uint16_t)((CFG.intensity/255.0f) * (L/3 + 1));
  uint8_t n; RgbLin pal[4]; loadMotionPaletteLin(n, pal);
  float phase = frameCount * 0.0025f;

  for (uint16_t i=0; i<pops; ++i) {
    uint16_t k = esp_random()%L;
    float x = (k/(float)L) + phase + (float)(esp_random() & 0xFF)/255.0f * 0.1f;
    RgbLin c = samplePaletteLin(x, n, pal, CFG.intensity);
    paintMax(k, c);
  }
}

static void animComet() {
  uint16_t L = ringLen(); if (!L) return;
  memcpy(bufCur, bufPrev, sizeof(RgbLin)*L);
  float retain = 0.55f + (CFG.intensity/255.f)*0.35f; // 0.55..0.90
  dimAll(bufCur, retain);

  float hz = speedToHz(CFG.speed, 0.2f, 8.0f);
  static float pos = 0.f; pos += dtSec*hz*L;
  float head = fmodf(pos, (float)L);

  uint8_t n; RgbLin pal[4]; loadMotionPaletteLin(n, pal);
  float phase = frameCount * 0.0055f;
  RgbLin headC = samplePaletteLin((head/(float)L)+phase, n, pal, CFG.intensity);
  drawSoftDot(head, headC);

  uint8_t w = (CFG.width < 1) ? 1 : CFG.width;
  for (uint8_t k=1;k<=w;++k){
    float t = (float)k / (float)w;
    // smoothstep-ish tail: (1-t)^2
    float amp = (1.f - t); amp *= amp;
    drawSoftDot(head - k, mul(headC, amp));
  }
}

static void animMeteor() {
  uint16_t L = ringLen(); if (!L) return;
  clearBuf(bufCur);
  float hz = speedToHz(CFG.speed, 0.2f, 8.0f);
  static float pos = 0.f; pos += dtSec*hz*L;
  float p = fmodf(pos, (float)L);

  RgbLin a = toLin24(CFG.colorA);
  RgbLin b = toLin24(CFG.colorB);
  uint8_t wmax = (CFG.width < 1) ? 1 : CFG.width;
  for (uint8_t w=0; w<wmax; ++w) {
    float t = w / float(wmax);
    RgbLin mixc( a.r*(1-t)+b.r*t, a.g*(1-t)+b.g*t, a.b*(1-t)+b.b*t );
    drawSoftDot(p + w, mixc);
  }
}

static void animClockSpin() {
  uint16_t L = ringLen(); if (!L) return;
  clearBuf(bufCur);
  float hz = speedToHz(CFG.speed, 0.3f, 6.0f);
  static float pos = 0.f; pos += dtSec*hz*L;
  float head = fmodf(pos, (float)L);

  RgbLin bg = toLin24(CFG.colorB);
  RgbLin fg = toLin24(CFG.colorA);
  for (uint16_t i=0;i<L;++i) bufCur[i] = bg;

  uint8_t span = (uint8_t)(CFG.width*2+1);
  if (span < 1) span = 1;
  for (uint8_t w=0; w<span; ++w) drawSoftDot(head + w, fg);
}

static void animPlasma() {
  uint16_t L = ringLen(); if (!L) return;
  static float t = 0.f; t += dtSec * (0.7f + (CFG.speed/255.0f)*1.8f);
  // A bit more organic: one term = low-frequency noise-ish drift
  float drift = sinf(t*0.23f) * 0.35f + sinf(t*0.11f + 1.3f) * 0.15f;

  for (uint16_t i=0; i<L; ++i) {
    float a = (float)i / (float)L * 6.2831853f; // 0..2π
    float v = 0.5f + 0.5f * (sinf(3*a + t) * 0.6f + sinf(5*a - t*0.8f + drift) * 0.4f);
    float sat = 0.4f + (CFG.intensity/255.0f)*0.6f;
    float con = 0.7f + (CFG.width/20.0f)*0.6f;
    float hue = fmodf((v*con + t*0.03f), 1.0f);
    // hsv2rgb_lin already returns linear
    bufCur[i] = hsv2rgb_lin(hue, sat, 1.0f);
  }
}

static void animFire() {
  uint16_t L = ringLen(); if (!L) return;

  // Tunables (brighten slightly, add mild vertical variability)
  const uint8_t COOL_BASE = 50;   // less cooling -> brighter
  const uint8_t COOL_SPAN = 36;
  const uint8_t SPARK_ADD_BASE = 180;
  const uint8_t HEAT_BIAS = 20;

  uint8_t cool = COOL_BASE - (uint8_t)((uint16_t)CFG.intensity * COOL_SPAN / 255);
  for (uint16_t i = 0; i < L; ++i) {
    uint8_t dec = esp_random() % (cool + 1);
    heat[i] = (heat[i] > dec) ? (heat[i] - dec) : 0;
  }

  // slight directional diffusion biases (more heat upward: toward CH1->CH4 order)
  for (uint16_t i = 0; i < L; ++i) {
    uint16_t i1 = (i + L - 1) % L;
    uint16_t i2 = (i + 1) % L;
    uint16_t sum = (uint16_t)heat[i] + heat[i1] + heat[i2];
    // tiny asymmetry
    heat[i] = (uint8_t)((sum + ((i & 1) ? 1 : 0)) / 3);
  }

  uint8_t sparks = 1 + (CFG.speed / 64); // 1..5
  for (uint8_t s = 0; s < sparks; ++s) {
    uint16_t p = esp_random() % L;
    uint16_t add = SPARK_ADD_BASE + (esp_random() % 96); // 180..275
    uint16_t v = (uint16_t)heat[p] + add;
    heat[p] = (v > 255) ? 255 : (uint8_t)v;
  }

  const uint8_t TH1 = 72;
  const uint8_t TH2 = 160;

  for (uint16_t i = 0; i < L; ++i) {
    uint16_t q16 = (uint16_t)heat[i] + HEAT_BIAS;
    uint8_t  t8  = (q16 > 255) ? 255 : (uint8_t)q16;

    RgbColor srgb;
    if (t8 < TH1) {
      uint8_t r = (uint16_t)t8 * 255 / TH1;
      srgb = RgbColor(r, 0, 0);
    } else if (t8 < TH2) {
      uint8_t g = (uint16_t)(t8 - TH1) * 255 / (TH2 - TH1);
      srgb = RgbColor(255, g, 0);
    } else {
      uint8_t b = (uint16_t)(t8 - TH2) * 255 / (255 - TH2);
      srgb = RgbColor(255, 255, b);
    }
    bufCur[i] = toLin(srgb);
  }
}

// --- Palette Cycle ---
static void animPaletteCycle() {
  uint16_t L = ringLen(); if (!L) return;
  uint8_t n; RgbLin pal[4]; loadPaletteLin(n, pal);

  float hz = speedToHz(CFG.speed, 0.1f, 4.0f);
  static float off = 0.f; off += dtSec*hz;

  for (uint16_t i=0;i<L;++i) {
    float x = (i / (float)L) + off * 0.25f;
    bufCur[i] = samplePaletteLin(x, n, pal, CFG.intensity);
  }
}

// --- Palette Chase ---
static void animPaletteChase() {
  uint16_t L = ringLen(); if (!L) return;
  uint8_t n; RgbLin pal[4]; loadPaletteLin(n, pal);

  uint16_t block = (CFG.width < 1) ? 1 : CFG.width;
  float hz = speedToHz(CFG.speed, 0.2f, 8.0f);
  static float pos = 0.f; pos += dtSec*hz*L;

  for (uint16_t i=0;i<L;++i) {
    uint16_t k = (i + L - ((uint16_t)pos % L)) % L;
    uint16_t which = (block == 0) ? 0 : ((k / block) % n);
    RgbLin base = pal[which];

    if (CFG.intensity == 0) { bufCur[i] = base; continue; }

    uint16_t edge = (block==0) ? 0 : (k % block);
    float tEdge = (block==0) ? 0.f :
      fabsf((edge - (block-1)/2.0f)) / fmaxf(1.f, (block/2.0f)); // 0 center .. 1 edge
    float soft = 1.0f - (CFG.intensity/255.0f) * tEdge;
    if (soft < 0.f) soft = 0.f;
    bufCur[i] = mul(base, soft);
  }
}

// -------------------- Frame selection --------------------
static void renderFrame() {
  // default clear unless mode needs previous trails
  clearBuf(bufCur);

  switch (CFG.mode) {
    case MODE_SOLID:         animSolid();         break;
    case MODE_BREATHE:       animBreathe();       break;
    case MODE_COLOR_WIPE:    animColorWipe();     break;
    case MODE_LARSON:        animLarson();        break;
    case MODE_RAINBOW:       animRainbow();       break;
    case MODE_THEATER:       animTheater();       break;
    case MODE_TWINKLE:       animTwinkle();       break;
    case MODE_COMET:         animComet();         break;
    case MODE_METEOR:        animMeteor();        break;
    case MODE_CLOCK_SPIN:    animClockSpin();     break;
    case MODE_PLASMA:        animPlasma();        break;
    case MODE_FIRE:          animFire();          break;

    case MODE_PALETTE_CYCLE: animPaletteCycle();  break;
    case MODE_PALETTE_CHASE: animPaletteChase();  break;

    default: break;
  }

  showRing();
}

// -------------------- Persistence --------------------
static const char* NVS_NS = "rgbctrl";
static const char* NVS_KEY= "config";

static void defaults() { CFG = AppConfig(); }

static String configToJson() {
  StaticJsonDocument<1024> doc;
  JsonArray counts = doc.createNestedArray("count");
  for (uint8_t i=0;i<NUM_CH;++i) counts.add(CFG.count[i]);
  doc["brightness"]   = CFG.brightness;
  doc["mode"]         = CFG.mode;
  doc["speed"]        = CFG.speed;
  doc["intensity"]    = CFG.intensity;
  doc["width"]        = CFG.width;
  doc["colorA"]       = CFG.colorA;
  doc["colorB"]       = CFG.colorB;
  doc["colorC"]       = CFG.colorC;
  doc["colorD"]       = CFG.colorD;
  doc["paletteCount"] = CFG.paletteCount;
  doc["resumeOnBoot"] = CFG.resumeOnBoot;
  doc["enableCpu"]    = CFG.enableCpu;
  doc["enableFan"]    = CFG.enableFan;
  doc["inPreview"]    = inPreview;

  // Non-persistent display info
  doc["buildVersion"] = APP_VERSION;
  doc["copyright"]    = COPYRIGHT_TXT;

  String js; serializeJson(doc, js); return js;
}
static bool parseConfig(const String& body, AppConfig& out) {
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body)) return false;

  if (doc.containsKey("count")) {
    for (uint8_t i=0;i<NUM_CH;++i) {
      uint16_t v = doc["count"][i].as<uint16_t>();
      if (v > MAX_PER_CH) v = MAX_PER_CH;
      out.count[i] = v;
    }
  }
  if (doc.containsKey("brightness"))  out.brightness  = doc["brightness"].as<uint8_t>();

  if (doc.containsKey("mode")) {
    int m = doc["mode"].as<int>();
    if (m < 0) m = 0;
    if (m > (int)MODE_COUNT-1) m = MODE_COUNT-1;
    out.mode = (uint8_t)m;
  }

  if (doc.containsKey("speed"))       out.speed       = doc["speed"].as<uint8_t>();
  if (doc.containsKey("intensity"))   out.intensity   = doc["intensity"].as<uint8_t>();

  if (doc.containsKey("width")) {
    int w = doc["width"].as<int>();
    if (w < 1) w = 1; if (w > 255) w = 255;
    out.width = (uint8_t)w;
  }

  if (doc.containsKey("colorA"))      out.colorA      = doc["colorA"].as<uint32_t>();
  if (doc.containsKey("colorB"))      out.colorB      = doc["colorB"].as<uint32_t>();
  if (doc.containsKey("colorC"))      out.colorC      = doc["colorC"].as<uint32_t>();
  if (doc.containsKey("colorD"))      out.colorD      = doc["colorD"].as<uint32_t>();

  if (doc.containsKey("paletteCount")) {
    uint8_t pc = doc["paletteCount"].as<uint8_t>();
    out.paletteCount = (pc < 1) ? 1 : ((pc > 4) ? 4 : pc);
  }

  if (doc.containsKey("resumeOnBoot"))out.resumeOnBoot= doc["resumeOnBoot"].as<bool>();
  if (doc.containsKey("enableCpu"))   out.enableCpu   = doc["enableCpu"].as<bool>();
  if (doc.containsKey("enableFan"))   out.enableFan   = doc["enableFan"].as<bool>();
  return true;
}
static void applyConfig() {
  rebuildRingMap();
  for (uint8_t s=0; s<NUM_CH; ++s) STRIPS[s]->setBrightness(CFG.brightness);
  lastAppliedBrightness = CFG.brightness;
}
static void loadConfig() {
  prefs.begin(NVS_NS, true);
  String js = prefs.getString(NVS_KEY, "");
  prefs.end();
  if (!js.length()) { defaults(); return; }

  StaticJsonDocument<1024> doc;
  if (!deserializeJson(doc, js)) {
    AppConfig tmp = CFG;
    if (doc.containsKey("count")) {
      for (uint8_t i=0;i<NUM_CH;++i) {
        uint16_t v = doc["count"][i].as<uint16_t>();
        if (v > MAX_PER_CH) v = MAX_PER_CH;
        tmp.count[i] = v;
      }
    }
    if (doc.containsKey("brightness"))  tmp.brightness  = doc["brightness"].as<uint8_t>();
    if (doc.containsKey("mode")) {
      int m = doc["mode"].as<int>();
      if (m < 0) m = 0;
      if (m > (int)MODE_COUNT-1) m = MODE_COUNT-1;
      tmp.mode = (uint8_t)m;
    }
    if (doc.containsKey("speed"))       tmp.speed       = doc["speed"].as<uint8_t>();
    if (doc.containsKey("intensity"))   tmp.intensity   = doc["intensity"].as<uint8_t>();
    if (doc.containsKey("width")) {
      int w = doc["width"].as<int>();
      if (w < 1) w = 1; if (w > 255) w = 255;
      tmp.width = (uint8_t)w;
    }
    if (doc.containsKey("colorA"))      tmp.colorA      = doc["colorA"].as<uint32_t>();
    if (doc.containsKey("colorB"))      tmp.colorB      = doc["colorB"].as<uint32_t>();
    if (doc.containsKey("colorC"))      tmp.colorC      = doc["colorC"].as<uint32_t>();
    if (doc.containsKey("colorD"))      tmp.colorD      = doc["colorD"].as<uint32_t>();
    if (doc.containsKey("paletteCount")){
      uint8_t pc = doc["paletteCount"].as<uint8_t>();
      tmp.paletteCount = (pc < 1) ? 1 : ((pc > 4) ? 4 : pc);
    }
    if (doc.containsKey("resumeOnBoot"))tmp.resumeOnBoot= doc["resumeOnBoot"].as<bool>();
    if (doc.containsKey("enableCpu"))   tmp.enableCpu   = doc["enableCpu"].as<bool>();
    if (doc.containsKey("enableFan"))   tmp.enableFan   = doc["enableFan"].as<bool>();
    CFG = tmp;
  }
}
static void saveConfig() {
  StaticJsonDocument<1024> doc;
  JsonArray counts = doc.createNestedArray("count");
  for (uint8_t i=0;i<NUM_CH;++i) counts.add(CFG.count[i]);
  doc["brightness"]   = CFG.brightness;
  doc["mode"]         = CFG.mode;
  doc["speed"]        = CFG.speed;
  doc["intensity"]    = CFG.intensity;
  doc["width"]        = CFG.width;
  doc["colorA"]       = CFG.colorA;
  doc["colorB"]       = CFG.colorB;
  doc["colorC"]       = CFG.colorC;
  doc["colorD"]       = CFG.colorD;
  doc["paletteCount"] = CFG.paletteCount;
  doc["resumeOnBoot"] = CFG.resumeOnBoot;
  doc["enableCpu"]    = CFG.enableCpu;
  doc["enableFan"]    = CFG.enableFan;

  String js; serializeJson(doc, js);
  prefs.begin(NVS_NS, false);
  prefs.putString(NVS_KEY, js);
  prefs.end();
}

// -------------------- Web UI (unchanged) --------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>RGB Controller</title>
<style>
:root{--bg:#0f1115;--card:#161a22;--a:#6aa9ff;--t:#d6e1ff;--muted:#94a3b8;}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--t);font-family:Inter,system-ui,Segoe UI,Roboto,Arial}
.container{max-width:980px;margin:24px auto;padding:0 16px}
.card{background:var(--card);border-radius:16px;padding:18px 16px;box-shadow:0 6px 24px #0008;margin-bottom:18px}
.row{display:grid;grid-template-columns:repeat(12,1fr);gap:12px}
.h{font-size:22px;margin:0 0 12px}label{font-size:13px;color:var(--muted);display:block;margin-bottom:6px}
input,select,button{width:100%;padding:10px 12px;border-radius:10px;border:1px solid #2a3142;background:#0b0e14;color:#d6e1ff}
input[type=color]{padding:0;height:40px}button{background:#0f172a;border:1px solid #35425b;cursor:pointer}
button.primary{background:#2563eb;border:0}
.row>div{grid-column:span 12}@media(min-width:700px){.md-6{grid-column:span 6}.md-4{grid-column:span 4}.md-3{grid-column:span 3}}
.badge{display:inline-block;background:#0b1220;border:1px solid #273657;color:#9ec1ff;padding:2px 8px;border-radius:999px;font-size:12px;margin-left:8px}
.hint{color:#90a4c9;font-size:12px}
.hide{display:none}
.toggle{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.footer{color:var(--muted);font-size:12px;text-align:center;padding:8px 0 24px}
.sep{margin:0 8px}
</style></head><body><div class="container">
<div class="card">
  <h2 class="h">RGB Controller (CH1–CH4)<span id="status" class="badge">loading…</span></h2>
  <div class="row">
    <div class="md-4"><label>Mode</label>
      <select id="mode">
        <option value="0">Solid</option>
        <option value="1">Breathe</option>
        <option value="2">Color Wipe</option>
        <option value="3">Larson</option>
        <option value="4">Rainbow</option>
        <option value="5">Theater Chase</option>
        <option value="6">Twinkle</option>
        <option value="7">Comet</option>
        <option value="8">Meteor</option>
        <option value="9">Clock Spin</option>
        <option value="10">Plasma</option>
        <option value="11">Fire / Flicker</option>
        <option value="12">Palette Cycle</option>
        <option value="13">Palette Chase</option>
      </select>
    </div>
    <div class="md-4"><label>Brightness</label><input id="brightness" type="range" min="1" max="255"></div>
    <div class="md-4"><label>Speed</label><input id="speed" type="range" min="0" max="255"></div>

    <div class="md-3 opt opt-intensity"><label>Intensity</label><input id="intensity" type="range" min="0" max="255"></div>
    <div class="md-3 opt opt-width"><label>Width / Gap</label><input id="width" type="range" min="1" max="20"></div>
    <div class="md-3 opt opt-colorA"><label>Primary Color</label><input id="colorA" type="color"></div>
    <div class="md-3 opt opt-colorB"><label>Secondary Color</label><input id="colorB" type="color"></div>
    <div class="md-3 opt opt-colorC"><label>Color C</label><input id="colorC" type="color"></div>
    <div class="md-3 opt opt-colorD"><label>Color D</label><input id="colorD" type="color"></div>
    <div class="md-3 opt opt-palette"><label>Palette Size</label>
      <select id="paletteCount">
        <option value="1">1 color</option>
        <option value="2" selected>2 colors</option>
        <option value="3">3 colors</option>
        <option value="4">4 colors</option>
      </select>
    </div>

    <div class="md-3"><label>CH1 (Front) Count</label><input id="c0" type="number" min="0" max="50"></div>
    <div class="md-3"><label>CH2 (Left) Count</label><input id="c1" type="number" min="0" max="50"></div>
    <div class="md-3"><label>CH3 (Rear) Count</label><input id="c2" type="number" min="0" max="50"></div>
    <div class="md-3"><label>CH4 (Right) Count</label><input id="c3" type="number" min="0" max="50"></div>

    <div class="md-6"><label>Resume last mode on boot</label>
      <select id="resume"><option value="true">Yes</option><option value="false">No</option></select>
    </div>

    <div class="md-6"><label>Xbox SMBus LEDs</label>
      <div class="toggle">
        <input id="smbusCpu" type="checkbox"> <span>Enable CPU temp LEDs (CH5)</span>
      </div>
      <div class="toggle">
        <input id="smbusFan" type="checkbox"> <span>Enable Fan speed LEDs (CH6)</span>
      </div>
      <span class="hint">Disable to avoid SMBus polling by the other module.</span>
    </div>

    <div class="md-6"><button class="primary" id="save">Save</button></div>
    <div class="md-6"><button id="revert">Reload</button></div>
    <div class="md-6"><button id="reset">Reset Defaults</button></div>
    <div class="md-12"><span class="hint">All changes preview live. Click Save to persist to flash.</span></div>
  </div>
</div>

<!-- Footer (always visible) -->
<div id="footer" class="footer">
  <span id="cpy">%%COPYRIGHT%%</span><span class="sep">•</span><span id="ver">v%%VERSION%%</span>
</div>

</div>
<script>
const el=id=>document.getElementById(id);
const hex24=n=>'#'+('000000'+n.toString(16)).slice(-6);
const to24=hex=>parseInt(hex.replace('#',''),16);

// ---- Injected at render time so controls are filled immediately ----
const BOOT = %%BOOTJSON%%;

let state=null, syncing=false;

function showOptsFor(mode){
  const vis = {
    colorA:   [0,1,2,3,4,5,6,7,8,9,10,11,12,13],
    colorB:   [8,9,10,12,13],
    colorC:   [12,13],
    colorD:   [12,13],
    palette:  [12,13],
    width:    [3,5,7,8,9,13],        // Palette Chase uses width (block size)
    intensity:[3,5,6,7,8,11,12,13],  // palette blend / soft edges
  };

  const on = k => (vis[k]||[]).includes(mode);
  const toggle = (cls, yes)=>document.querySelectorAll(cls).forEach(n=>n.classList.toggle('hide',!yes));

  // base visibility by mode
  toggle('.opt-colorA', on('colorA'));
  toggle('.opt-colorB', on('colorB'));
  toggle('.opt-colorC', on('colorC'));
  toggle('.opt-colorD', on('colorD'));
  toggle('.opt-palette', on('palette'));
  toggle('.opt-width',  on('width'));
  toggle('.opt-intensity', on('intensity'));

  // further trim Color C/D by palette size when palette modes are active
  if (on('palette')) {
    const pc = +document.getElementById('paletteCount').value || 2;
    document.querySelectorAll('.opt-colorC').forEach(n=>n.classList.toggle('hide', pc < 3));
    document.querySelectorAll('.opt-colorD').forEach(n=>n.classList.toggle('hide', pc < 4));
  } else {
    document.querySelectorAll('.opt-colorC').forEach(n=>n.classList.add('hide'));
    document.querySelectorAll('.opt-colorD').forEach(n=>n.classList.add('hide'));
  }
}

function fillForm(s){
  el('mode').value      = s.mode;
  el('brightness').value= s.brightness;
  el('speed').value     = s.speed;
  el('intensity').value = s.intensity;
  el('width').value     = s.width;
  el('colorA').value    = hex24(s.colorA);
  el('colorB').value    = hex24(s.colorB);
  el('colorC').value    = hex24(s.colorC || 0);
  el('colorD').value    = hex24(s.colorD || 0);
  el('paletteCount').value = s.paletteCount || 2;
  for(let i=0;i<4;i++) el('c'+i).value = s.count[i];
  el('resume').value    = s.resumeOnBoot ? 'true' : 'false';
  el('smbusCpu').checked= !!s.enableCpu;
  el('smbusFan').checked= !!s.enableFan;

  // footer text (always shown)
  el('ver').textContent = 'v' + (s.buildVersion || '—');
  el('cpy').textContent = s.copyright || '© Darkone Customs 2025';

  showOptsFor(s.mode|0);
}

function gather(){
  return {
    mode:+el('mode').value,
    brightness:+el('brightness').value,
    speed:+el('speed').value,
    intensity:+el('intensity').value,
    width:+el('width').value,
    colorA:to24(el('colorA').value),
    colorB:to24(el('colorB').value),
    colorC:to24(el('colorC').value),
    colorD:to24(el('colorD').value),
    paletteCount:+el('paletteCount').value,
    count:[+el('c0').value,+el('c1').value,+el('c2').value,+el('c3').value],
    resumeOnBoot:(el('resume').value==='true'),
    enableCpu:el('smbusCpu').checked,
    enableFan:el('smbusFan').checked
  };
}

async function load(){
  syncing=true;
  // 1) Fill instantly from injected JSON (last-saved prefs)
  state = BOOT || {};
  fillForm(state);
  el('status').textContent='ready';

  // 2) Also fetch live from API (in case config changed elsewhere)
  try{
    const j = await fetch('%%BASE%%/api/ledconfig').then(r=>r.json());
    state = j; fillForm(state);
  }catch(e){
    console.log('load config fetch failed', e);
  }
  syncing=false;
}

async function preview(){
  if(syncing) return;
  const res = await fetch('%%BASE%%/api/ledpreview',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(gather())});
  el('status').textContent = res.ok ? 'live' : 'error';
}
async function save(){
  const res = await fetch('%%BASE%%/api/ledsave',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(gather())});
  el('status').textContent = res.ok ? 'saved' : 'error';
}
async function resetDefaults(){
  const res = await fetch('%%BASE%%/api/ledreset',{method:'POST'});
  el('status').textContent = res.ok ? 'reset' : 'error';
  load();
}

// ---- Event binding (fix: use 'change' for selects) ----
function bind(id, handler) {
  const n = document.getElementById(id);
  if (!n) return;
  const ev = (n.tagName === 'SELECT' || n.type === 'checkbox') ? 'change' : 'input';
  n.addEventListener(ev, handler);
}

// On mode change: update option visibility then preview
bind('mode', () => {
  showOptsFor(+document.getElementById('mode').value);
  preview();
});

// Palette size can change which color pickers are shown (C/D)
bind('paletteCount', () => {
  showOptsFor(+document.getElementById('mode').value);
  preview();
});

// Live preview for the rest
['brightness','speed','intensity','width','colorA','colorB','colorC','colorD','resume','smbusCpu','smbusFan']
  .forEach(id => bind(id, preview));

// Count inputs preview on change
for (let i = 0; i < 4; i++) {
  const n = document.getElementById('c' + i);
  if (n) n.addEventListener('change', preview);
}

// Buttons
document.getElementById('save').addEventListener('click',save);
document.getElementById('revert').addEventListener('click',load);
document.getElementById('reset').addEventListener('click',resetDefaults);

load();
</script></body></html>
)HTML";

// -------------------- Public API --------------------
void begin(const RGBCtrlPins& pins) {
  PINS = pins;

  // Bind pins/length, then init
  strip1.updateLength(MAX_PER_CH); strip1.setPin(PINS.ch1);
  strip2.updateLength(MAX_PER_CH); strip2.setPin(PINS.ch2);
  strip3.updateLength(MAX_PER_CH); strip3.setPin(PINS.ch3);
  strip4.updateLength(MAX_PER_CH); strip4.setPin(PINS.ch4);

  for (uint8_t s=0;s<NUM_CH;++s) {
    STRIPS[s]->begin();
    STRIPS[s]->clear();
    STRIPS[s]->setBrightness(CFG.brightness);
    STRIPS[s]->show();
  }
  lastAppliedBrightness = CFG.brightness;

  memset(heat, 0, sizeof(heat));
  clearBuf(bufCur);
  clearBuf(bufPrev);
  clearBuf(bufSmoothPrev);

  // Load the LAST SAVED preferences from NVS on boot
  loadConfig();
  applyConfig();       // apply brightness and counts
  renderFrame();       // show the active mode immediately
}

void attachWeb(AsyncWebServer& server, const char* basePath) {
  gBase = basePath && *basePath ? basePath : "/config";

  // Serve UI (embed last-saved config as BOOT JSON for instant fill)
  server.on(gBase.c_str(), HTTP_GET, [](AsyncWebServerRequest *request){
    // ensure we reflect latest saved state
    loadConfig();
    applyConfig();
    String html = FPSTR(INDEX_HTML);
    html.replace("%%BASE%%", gBase);
    html.replace("%%BOOTJSON%%", configToJson());
    html.replace("%%VERSION%%", APP_VERSION);
    html.replace("%%COPYRIGHT%%", COPYRIGHT_TXT);
    // Explicitly disable caching for the main HTML
    AsyncWebServerResponse* resp = request->beginResponse(200, "text/html", html);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
  });

  // Also add a default no-store for safety
  DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");

  // GET config (also reload from NVS so page sees LAST SAVED prefs)
  server.on(String(gBase + "/api/ledconfig").c_str(), HTTP_GET, [](AsyncWebServerRequest *request){
    loadConfig();        // pull from flash
    applyConfig();       // apply to strips (counts/brightness)
    String body = configToJson();
    AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", body);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
  });

  // POST preview (apply but don't save) + render immediately
  server.on(String(gBase + "/api/ledpreview").c_str(), HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0) {
        req->_tempObject = new String();
        ((String*)req->_tempObject)->reserve(total ? total : 256);
      }
      String* body = (String*)req->_tempObject;
      body->concat((const char*)data, len);
      if (index + len == total) {
        AppConfig tmp = CFG;
        if (!parseConfig(*body, tmp)) {
          req->send(400, "text/plain", "Bad JSON");
        } else {
          CFG = tmp; inPreview = true; applyConfig();
          renderFrame();
          req->send(200, "application/json", "{\"ok\":true}");
        }
        delete body; req->_tempObject = nullptr;
      }
    }
  );

  // POST save (apply + persist) + render immediately
  server.on(String(gBase + "/api/ledsave").c_str(), HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0) {
        req->_tempObject = new String();
        ((String*)req->_tempObject)->reserve(total ? total : 256);
      }
      String* body = (String*)req->_tempObject;
      body->concat((const char*)data, len);
      if (index + len == total) {
        AppConfig tmp = CFG;
        if (!parseConfig(*body, tmp)) {
          req->send(400, "text/plain", "Bad JSON");
        } else {
          CFG = tmp; inPreview = false; applyConfig(); saveConfig();
          renderFrame();
          req->send(200, "application/json", "{\"ok\":true}");
        }
        delete body; req->_tempObject = nullptr;
      }
    }
  );

  // POST reset (defaults) + render immediately
  server.on(String(gBase + "/api/ledreset").c_str(), HTTP_POST, [](AsyncWebServerRequest *request){
    prefs.begin(NVS_NS, false); prefs.remove(NVS_KEY); prefs.end();
    defaults(); inPreview = false; applyConfig();
    renderFrame();
    request->send(200, "application/json", "{\"ok\":true}");
  });
}

// Convenience overload: uses WiFiMgr::getServer()
void attachWeb(const char* basePath) {
  attachWeb(WiFiMgr::getServer(), basePath && *basePath ? basePath : "/config");
}

void loop() {
  // frame pacing: use previous scheme but compute dt
  uint8_t frameMs = 10 + (uint8_t)((255 - CFG.speed)/2); // faster speed -> shorter frame
  uint32_t now = millis();
  if (now - msPrev >= frameMs) {
    uint32_t delta = (now - msPrev);
    msPrev = now;
    // Exponential smoothing for dt
    float inst = delta / 1000.0f;
    dtSec = dtSec*0.8f + inst*0.2f;

    ++frameCount;
    renderFrame();
  }
}

void setCounts(uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4) {
  CFG.count[0] = (c1 > MAX_PER_CH) ? MAX_PER_CH : c1;
  CFG.count[1] = (c2 > MAX_PER_CH) ? MAX_PER_CH : c2;
  CFG.count[2] = (c3 > MAX_PER_CH) ? MAX_PER_CH : c3;
  CFG.count[3] = (c4 > MAX_PER_CH) ? MAX_PER_CH : c4;
  rebuildRingMap();
  // Clear buffers to avoid index artifacts when lengths change
  clearBuf(bufCur); clearBuf(bufPrev); clearBuf(bufSmoothPrev);
}

void forceSave() { saveConfig(); }
void forceLoad() { loadConfig(); applyConfig(); }

// SMBus flags accessors (for RGBsmbus to check)
bool smbusCpuEnabled() { return CFG.enableCpu; }
bool smbusFanEnabled() { return CFG.enableFan; }

String getConfigJson() {
  // uses existing internal serializer
  return configToJson();
}

bool applyJsonPreview(const String& body) {
  AppConfig tmp = CFG;
  if (!parseConfig(body, tmp)) return false;
  CFG = tmp;
  inPreview = true;
  applyConfig();
  renderFrame();
  return true;
}

bool applyJsonSave(const String& body) {
  AppConfig tmp = CFG;
  if (!parseConfig(body, tmp)) return false;
  CFG = tmp;
  inPreview = false;
  applyConfig();
  saveConfig();
  renderFrame();
  return true;
}

void resetToDefaults() {
  // wipe NVS key so defaults persist like your /api/ledreset handler
  prefs.begin(NVS_NS, false);
  prefs.remove(NVS_KEY);
  prefs.end();

  defaults();
  inPreview = false;
  applyConfig();
  renderFrame();
}

} // namespace RGBCtrl
