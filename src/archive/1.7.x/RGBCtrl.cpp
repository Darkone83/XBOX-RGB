#include "RGBCtrl.h"
#include "RGBudp.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <math.h>
#include <esp_system.h>  // esp_random
#include <vector>

// Use the existing WiFiMgr server; no separate server objects needed.
namespace WiFiMgr { AsyncWebServer& getServer(); }

// -------------------- Build / Branding --------------------
static const char* APP_VERSION = "1.6.1"; // shown in footer
static const char* COPYRIGHT_TXT = "© Darkone Customs 2025";

// -------------------- Limits / Types --------------------
static const uint16_t MAX_PER_CH = 50;        // per requirement
static const uint8_t  NUM_CH     = 4;         // CH1..CH4 only
static const uint16_t MAX_RING   = MAX_PER_CH * NUM_CH; // 200

namespace RGBCtrl {

// -------------------- Minimal RGB type --------------------
struct RgbColor {
  uint8_t R, G, B;
  RgbColor() : R(0), G(0), B(0) {}
  RgbColor(uint8_t r, uint8_t g, uint8_t b) : R(r), G(g), B(b) {}
};

// -------------------- Globals --------------------
// Orientation (clockwise ring):
//   CH1 = Front, CH2 = Left, CH3 = Rear, CH4 = Right
// Ring order is CH1 -> CH2 -> CH3 -> CH4; animations start at CH1 index 0.
static RGBCtrlPins PINS{1,2,3,4};

// Compile-time defaults (kept as initial seeds)
static bool REVERSE_DEFAULTS[NUM_CH] = { true, false, false, true };

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

  // per-channel reverse (runtime toggles; seeded from compile-time defaults)
  bool     reverse[NUM_CH] = { REVERSE_DEFAULTS[0], REVERSE_DEFAULTS[1],
                               REVERSE_DEFAULTS[2], REVERSE_DEFAULTS[3] };

  // --- NEW: Master Off (kill switch) ---
  bool     masterOff     = false;

  // --- NEW: Custom playlist mode ---
  // JSON string of steps; see Custom editor in WebUI for example.
  String   customSeq     = "[]";
  bool     customLoop    = true;
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

  // palette-aware modes:
  MODE_PALETTE_CYCLE,   // colors placed around ring; rotates
  MODE_PALETTE_CHASE,   // blocks of palette colors chase around ring

  // --- NEW: user-defined playlist mode ---
  MODE_CUSTOM,

  MODE_COUNT
};

struct Seg { uint8_t ch; uint16_t count; };
static Seg segs[NUM_CH];
static uint32_t msPrev = 0;
static uint16_t tick   = 0;

// Fire state (max ring length)
static uint8_t heat[MAX_RING]; // 0..255 heat map

// track last applied brightness
static uint8_t lastAppliedBrightness = 0xFF;

// --- Boot fade-in state ---
static bool     bootFadeActive      = false;
static uint32_t bootFadeStartMs     = 0;
static uint16_t bootFadeDurationMs  = 3200;   // fade time (ms) — tweak to taste
static uint8_t  bootFadeTarget      = 0;

// -------------------- Helpers --------------------
inline RgbColor rgbFrom24(uint32_t rgb) {
  return RgbColor((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF);
}
static inline RgbColor unpackGRB(uint32_t c) {
  // Adafruit_NeoPixel packs as 0x00GGRRBB for NEO_GRB
  uint8_t g = (c >> 16) & 0xFF;
  uint8_t r = (c >>  8) & 0xFF;
  uint8_t b = (c      ) & 0xFF;
  return RgbColor(r,g,b);
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
static void setRing(uint16_t idx, const RgbColor& c) {
  uint16_t base = 0;
  for (uint8_t s=0; s<NUM_CH; ++s) {
    if (idx < base + segs[s].count) {
      uint16_t within = idx - base;
      bool rev = CFG.reverse[segs[s].ch] && segs[s].count;
      uint16_t px = rev ? (segs[s].count - 1 - within) : within;
      STRIPS[segs[s].ch]->setPixelColor(px, c.R, c.G, c.B);
      return;
    }
    base += segs[s].count;
  }
}
static void fillRing(const RgbColor& c) {
  uint16_t L = ringLen();
  for (uint16_t i=0; i<L; ++i) setRing(i, c);
}
static void fadeRing(uint8_t amt) {
  for (uint8_t s = 0; s < NUM_CH; ++s) {
    for (uint16_t i = 0; i < segs[s].count; ++i) {
      uint32_t packed = STRIPS[segs[s].ch]->getPixelColor(i); // get packed GRB
      RgbColor c = unpackGRB(packed);                         // unpack to RGB
      uint8_t r = (uint16_t)c.R * (255 - amt) >> 8;
      uint8_t g = (uint16_t)c.G * (255 - amt) >> 8;
      uint8_t b = (uint16_t)c.B * (255 - amt) >> 8;
      STRIPS[segs[s].ch]->setPixelColor(i, r, g, b);
    }
  }
}

static void showRing() {
  // Boot fade: linearly ramp 0 -> target over bootFadeDurationMs
  if (bootFadeActive) {
    bootFadeTarget = CFG.brightness;                     // follow live target
    uint32_t elapsed = millis() - bootFadeStartMs;
    uint8_t cur = (elapsed >= bootFadeDurationMs)
                    ? bootFadeTarget
                    : (uint8_t)((uint32_t)bootFadeTarget * elapsed / bootFadeDurationMs);

    // Ensure at least 1 when target>0 so the user sees early glow
    if (bootFadeTarget && cur == 0) cur = 1;

    if (cur != lastAppliedBrightness) {
      for (uint8_t s=0; s<NUM_CH; ++s) STRIPS[s]->setBrightness(cur);
      lastAppliedBrightness = cur;
    }
    if (elapsed >= bootFadeDurationMs) bootFadeActive = false;
  } else {
    if (lastAppliedBrightness != CFG.brightness) {
      for (uint8_t s=0; s<NUM_CH; ++s) STRIPS[s]->setBrightness(CFG.brightness);
      lastAppliedBrightness = CFG.brightness;
    }
  }

  for (uint8_t s=0; s<NUM_CH; ++s) STRIPS[s]->show();
}

static RgbColor wheel(uint8_t pos) {
  if (pos < 85)   return RgbColor(255 - pos*3, pos*3, 0);
  if (pos < 170)  { pos -= 85;  return RgbColor(0, 255 - pos*3, pos*3); }
  pos -= 170;     return RgbColor(pos*3, 0, 255 - pos*3);
}
static RgbColor hsv2rgb(float h, float s, float v){
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
  return RgbColor(uint8_t(r*255), uint8_t(g*255), uint8_t(b*255));
}
static inline uint8_t clampPaletteCount(uint8_t n){
  if (n < 1) return 1; if (n > 4) return 4; return n;
}
static inline RgbColor lerp(const RgbColor& a, const RgbColor& b, float t){
  if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
  return RgbColor(
    (uint8_t)(a.R + (b.R - a.R)*t),
    (uint8_t)(a.G + (b.G - a.G)*t),
    (uint8_t)(a.B + (b.B - a.B)*t)
  );
}
static void loadPalette(uint8_t& n, RgbColor p[4]){
  n = clampPaletteCount(CFG.paletteCount);
  uint32_t src[4] = { CFG.colorA, CFG.colorB, CFG.colorC, CFG.colorD };
  for (uint8_t i=0;i<4;i++) p[i] = rgbFrom32(src[i]);
}
static RgbColor samplePalette(float x /*0..1*/, uint8_t n, const RgbColor p[4], uint8_t blend){
  // Hard step vs smooth blend controlled by 'intensity'/blend (0=steps, 255=full)
  if (n == 1) return p[0];
  float fx = fmodf(x, 1.0f); if (fx < 0) fx += 1.0f;
  float pos = fx * n;
  int i0 = (int)floorf(pos) % n;
  int i1 = (i0 + 1) % n;
  float t = pos - floorf(pos);
  if (blend == 0) return p[i0];
  float bw = (blend / 255.0f); // 0..1
  return lerp(p[i0], p[i1], t * bw);
}

// ---------- Extra helpers for richer color when only Color A is set ----------
static inline float fclampf(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }
static void rgb2hsv(const RgbColor& c, float& h, float& s, float& v){
  float r = c.R/255.f, g = c.G/255.f, b = c.B/255.f;
  float mx = fmaxf(r, fmaxf(g,b)), mn = fminf(r, fminf(g,b));
  float d = mx - mn; v = mx; s = (mx <= 0.f) ? 0.f : d / mx;
  if (d == 0.f) { h = 0.f; return; }
  if (mx == r)      h = fmodf(((g-b)/d), 6.f);
  else if (mx == g) h = ((b-r)/d) + 2.f;
  else              h = ((r-g)/d) + 4.f;
  h /= 6.f; if (h < 0.f) h += 1.f;
}
static void loadMotionPalette(uint8_t& n, RgbColor p[4]){
  loadPalette(n, p);
  if (n >= 2) return; // already multi-color
  // derive companions from A
  float h,s,v; rgb2hsv(p[0], h, s, v);
  float s1 = fclampf(s * 1.05f, 0.f, 1.f);
  float s2 = fclampf(s * 0.85f, 0.f, 1.f);
  float v1 = fclampf(v * 1.05f, 0.f, 1.f);
  float v2 = fclampf(v * 0.92f, 0.f, 1.f);
  p[0] = hsv2rgb(h,                 s,  v);
  p[1] = hsv2rgb(fmodf(h + 0.08f,1), s1, v1);
  p[2] = hsv2rgb(fmodf(h + 0.33f,1), s2, v1);
  p[3] = hsv2rgb(fmodf(h + 0.58f,1), s,  v2);
  n = 4;
}

// -------------------- Animations --------------------
static void animSolid() { fillRing(rgbFrom24(CFG.colorA)); }

// ---- UPDATED: smoother Breathe (ease + low-pass to remove stepping) ----
static void animBreathe() {
  uint16_t L = ringLen(); if (!L) return;

  // Phase advances with speed; keep independent of tick granularity
  static float phase = 0.f;
  float step = 0.010f + (CFG.speed / 255.0f) * 0.045f; // ~slow → faster
  phase += step;

  // Base waveform 0..1 and eased (smoothstep) to avoid harsh edges
  float s = 0.5f + 0.5f * sinf(phase * 6.2831853f);
  float eased = s*s*(3.f - 2.f*s);

  // Keep a small floor so LEDs never fully black
  float target = 0.10f + 0.90f * eased;

  // Low-pass filter the level to smooth frame pacing artifacts
  static float lvl = 0.0f;
  float alpha = 0.10f;                       // smoothing factor
  lvl = lvl*(1.0f - alpha) + target*alpha;  // 0..1

  RgbColor base = rgbFrom24(CFG.colorA);
  RgbColor cur(
    (uint8_t)(base.R * lvl),
    (uint8_t)(base.G * lvl),
    (uint8_t)(base.B * lvl)
  );
  fillRing(cur);
}

static void animColorWipe(bool forward=true) {
  uint16_t L = ringLen(); if (!L) return;
  RgbColor off(0,0,0); fillRing(off);
  uint16_t idx = (tick/2) % L;
  // colorful head
  uint8_t n; RgbColor pal[4]; loadMotionPalette(n, pal);
  float phase = tick * (0.003f + (CFG.speed/255.0f)*0.008f);
  RgbColor c = samplePalette((idx/(float)L) + phase, n, pal, CFG.intensity);
  setRing(forward ? idx : (L-1-idx), c);
}
static void animLarson() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 6 - (CFG.speed/51); if (denom < 1) denom = 1;
  uint16_t pos = (tick / (uint16_t)denom) % (L*2);
  if (pos >= L) pos = 2*L - 1 - pos;
  int fadeBase = 10 + CFG.intensity; if (fadeBase > 254) fadeBase = 254;
  uint8_t fadeAmt = (uint8_t)(255 - fadeBase);
  fadeRing(fadeAmt);
  uint8_t n; RgbColor pal[4]; loadMotionPalette(n, pal);
  float phase = tick * 0.006f;
  for (int w=-(int)CFG.width; w<=(int)CFG.width; ++w) {
    int p = (int)pos + w;
    if (p>=0 && p<(int)L) {
      RgbColor c = samplePalette((p/(float)L)+phase, n, pal, CFG.intensity);
      setRing((uint16_t)p, c);
    }
  }
}
static void animRainbow() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 6 - (CFG.speed/51); if (denom < 1) denom = 1;
  uint8_t offset = tick / (uint8_t)denom;
  for (uint16_t i=0;i<L;++i) setRing(i, wheel((i*256/L + offset) & 255));
}
static void animTheater() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 10 - (CFG.speed/32); if (denom < 1) denom = 1;
  uint8_t gap = (CFG.width < 1) ? 1 : CFG.width;
  uint8_t q = (tick / (uint8_t)denom) % gap;
  int fadeBase = 10 + CFG.intensity; if (fadeBase > 254) fadeBase = 254;
  uint8_t fadeAmt = (uint8_t)(255 - fadeBase);
  fadeRing(fadeAmt);
  uint8_t n; RgbColor pal[4]; loadMotionPalette(n, pal);
  float phase = tick * 0.0045f;
  for (uint16_t i=q; i<L; i+=gap) {
    RgbColor c = samplePalette((i/(float)L)+phase, n, pal, CFG.intensity);
    setRing(i, c);
  }
}

// ---- UPDATED: Twinkle with per-pixel glints (rise & fall) ----
static void animTwinkle() {
  uint16_t L = ringLen(); if (!L) return;

  // Fade background (speed controls fade speed)
  int f = 18 + (CFG.speed/2); if (f > 254) f = 254;
  uint8_t fadeAmt = (uint8_t)(255 - f);
  fadeRing(fadeAmt);

  // Per-pixel twinkle phase 0=off, 1..255 = active progress
  static uint8_t phase[MAX_RING] = {0};

  // Number of new twinkles per frame (scales with intensity and ring size)
  uint16_t pops = 1 + (uint16_t)((CFG.intensity * L) / (255 * 30) + 0.5f); // up to ~7 on 200px

  // Spawn new twinkles on currently inactive pixels
  for (uint16_t n=0; n<pops; ++n) {
    uint16_t k = esp_random() % L;
    if (phase[k] == 0) phase[k] = 1 + (esp_random() & 1); // start
  }

  // Palette for coloration
  uint8_t pn; RgbColor pal[4]; loadMotionPalette(pn, pal);
  float palPhase = tick * 0.0025f;

  // Per-frame phase advance: higher speed = faster glint; width lengthens the glint
  int advance = 2 + (CFG.speed / 24) - (CFG.width / 6);
  if (advance < 1) advance = 1;

  for (uint16_t i=0; i<L; ++i) {
    uint8_t ph = phase[i];
    if (ph == 0) continue;

    // Normalize to 0..1 and give a sharp twinkle curve (sin^3)
    float x = ph / 255.0f;
    float b = sinf(3.1415926f * x);
    b = b*b*b; // sharper peak

    // Sample palette and scale by brightness
    float u = (i/(float)L) + palPhase;
    RgbColor base = samplePalette(u, pn, pal, CFG.intensity);
    RgbColor c(
      (uint8_t)(base.R * b),
      (uint8_t)(base.G * b),
      (uint8_t)(base.B * b)
    );
    setRing(i, c);

    // Advance / finish
    uint16_t next = ph + advance;
    phase[i] = (next >= 255) ? 0 : (uint8_t)next;
  }
}

static void animComet() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 4 - (CFG.speed/64); if (denom < 1) denom = 1;
  uint16_t pos = (tick / (uint16_t)denom) % L;
  uint8_t fadeAmt = (uint8_t)(200 - (CFG.intensity > 199 ? 199 : CFG.intensity));
  fadeRing(fadeAmt);
  uint8_t n; RgbColor pal[4]; loadMotionPalette(n, pal);
  float phase = tick * 0.0055f;
  RgbColor head = samplePalette((pos/(float)L)+phase, n, pal, CFG.intensity);
  for (uint8_t w=0; w<CFG.width; ++w) {
    float tail = 1.0f - (w/(float)CFG.width);
    RgbColor c(
      (uint8_t)(head.R * tail),
      (uint8_t)(head.G * tail),
      (uint8_t)(head.B * tail)
    );
    setRing((pos + L - w) % L, c);
  }
}

// ---- UPDATED: Meteor → Meteor Shower (multiple heads with tapered tails) ----
static void animMeteor() {
  uint16_t L = ringLen(); if (!L) return;

  // Fade existing trails
  uint8_t fadeAmt = (uint8_t)(210 - (CFG.intensity > 209 ? 209 : CFG.intensity));
  fadeRing(fadeAmt);

  // How many concurrent meteors (1..8)
  const uint8_t MAXM = 8;
  uint8_t count = 1 + (uint8_t)((CFG.intensity * (MAXM-1)) / 255);

  // Persistent meteor state
  static bool   inited = false;
  static float  pos[MAXM];
  static float  vel[MAXM];
  static uint8_t len[MAXM];
  static uint16_t lastL = 0;

  if (!inited || lastL != L) {
    for (uint8_t m=0; m<MAXM; ++m) {
      pos[m] = (float)(esp_random() % L);
      vel[m] = 0.35f + 1.25f * ((esp_random() & 255) / 255.0f); // px/frame
      len[m] = 2 + (esp_random() % 6);
    }
    inited = true;
    lastL = L;
  }

  // Tail length mapped to width (2..(2+2*width))
  uint8_t baseTail = 2 + (uint8_t)(CFG.width * 2);

  // Palette
  uint8_t pn; RgbColor pal[4]; loadMotionPalette(pn, pal);
  float pphase = tick * 0.004f;

  // Speed multiplier (higher speed slider => faster meteors)
  float speedMul = 0.5f + 2.0f * (CFG.speed / 255.0f);

  for (uint8_t m=0; m<count; ++m) {
    // Advance & wrap
    pos[m] += vel[m] * speedMul;
    while (pos[m] >= L) pos[m] -= L;

    // Color head from palette varying by position
    float hu = (pos[m] / (float)L) + pphase;
    RgbColor head = samplePalette(hu, pn, pal, CFG.intensity);

    // Draw head
    setRing((uint16_t)pos[m], head);

    // Draw tapered tail behind the head
    uint8_t tl = baseTail + len[m];
    for (uint8_t k=1; k<=tl; ++k) {
      float t = k / (float)tl;  // 0..1
      float fall = (1.0f - t);
      fall *= fall;             // quadratic falloff
      RgbColor c(
        (uint8_t)(head.R * fall),
        (uint8_t)(head.G * fall),
        (uint8_t)(head.B * fall)
      );
      uint16_t p = ((int)pos[m] - k + L*4) % L;
      setRing(p, c);
    }

    // Occasionally randomize a meteor to keep the shower organic
    if ((esp_random() & 255) < 4) {
      vel[m] = 0.35f + 1.25f * ((esp_random() & 255) / 255.0f);
      len[m] = 2 + (esp_random() % 6);
    }
  }
}

static void animClockSpin() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 3 - (CFG.speed/85); if (denom < 1) denom = 1;
  uint16_t pos = (tick / (uint16_t)denom) % L;
  RgbColor bg = rgbFrom24(CFG.colorB);
  RgbColor fg = rgbFrom24(CFG.colorA);
  fillRing(bg);
  uint8_t span = (uint8_t)(CFG.width*2+1);
  if (span < 1) span = 1;
  for (uint8_t w=0; w<span; ++w) setRing((pos + w) % L, fg);
}

// ---- UPDATED: Richer Plasma (multi-octave field, contrast & sparkle) ----
static void animPlasma() {
  uint16_t L = ringLen(); if (!L) return;

  static float t = 0.f;
  float tstep = 0.015f + (CFG.speed / 255.0f) * 0.050f;
  t += tstep;

  const float drift = sinf(t * 0.23f) * 0.35f + sinf(t * 0.11f + 1.3f) * 0.15f;

  // User controls mapping
  const float satBase  = 0.55f + (CFG.intensity / 255.0f) * 0.45f; // 0.55..1.0
  const float contrast = 0.90f + (CFG.width     / 20.0f) * 0.60f;  // ~0.9..1.5
  const float sparkAmp = 0.06f * (CFG.intensity / 255.0f);

  for (uint16_t i=0; i<L; ++i) {
    const float u = (float)i / (float)L;
    const float a = u * 6.2831853f; // 0..2π

    // Multi-octave sine field
    const float f1 = sinf(3.0f * a + t)                * 0.55f;
    const float f2 = sinf(5.0f * a - t * 0.8f + drift) * 0.35f;
    const float f3 = sinf(6.3f * a + t * 1.6f)         * 0.20f;
    float field = (f1 + f2 + f3) * 0.5f + 0.5f; // normalize to ~0..1

    // Local sparkle + contrast
    float v = field * contrast + sparkAmp * sinf(a * 8.0f - t * 2.2f);
    if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f;

    const float hue = fmodf(field * 1.2f + t * 0.05f, 1.0f);
    const float sat = satBase;

    setRing(i, hsv2rgb(hue, sat, v));
  }
}

static void animFire() {
  uint16_t L = ringLen(); if (!L) return;

  // Tunables (subtle brighten without blowing things out)
  const uint8_t COOL_BASE = 50;   // was 55 (less cooling = brighter)
  const uint8_t COOL_SPAN = 36;   // was 40  (keeps similar range)
  const uint8_t SPARK_ADD_BASE = 180; // was 160 (hotter sparks)
  const uint8_t HEAT_BIAS = 65;   // +20 heat before color map (push into yellow/white a bit)

  // 1) cool down each cell a little
  uint8_t cool = COOL_BASE - (uint8_t)((uint16_t)CFG.intensity * COOL_SPAN / 255); // ~14..50
  for (uint16_t i = 0; i < L; ++i) {
    uint8_t dec = esp_random() % (cool + 1);
    heat[i] = (heat[i] > dec) ? (heat[i] - dec) : 0;
  }

  // 2) heat diffuses (blur)
  for (uint16_t i = 0; i < L; ++i) {
    uint16_t i1 = (i + L - 1) % L;
    uint16_t i2 = (i + 1) % L;
    heat[i] = (uint8_t)((heat[i] + heat[i1] + heat[i2]) / 3);
  }

  // 3) random sparks (a bit hotter than before)
  uint8_t sparks = 1 + (CFG.speed / 64); // 1..5
  for (uint8_t s = 0; s < sparks; ++s) {
    uint16_t p = esp_random() % L;
    uint16_t add = SPARK_ADD_BASE + (esp_random() % 96); // 180..275
    uint16_t v = (uint16_t)heat[p] + add;
    heat[p] = (v > 255) ? 255 : (uint8_t)v;
  }

  // 4) map heat to color (biased a bit upward → less red, more yellow/white)
  const uint8_t TH1 = 35;
  const uint8_t TH2 = 160;

  for (uint16_t i = 0; i < L; ++i) {
    uint16_t q16 = (uint16_t)heat[i] + HEAT_BIAS;
    uint8_t  t8  = (q16 > 255) ? 255 : (uint8_t)q16;

    RgbColor c;
    if (t8 < TH1) {
      uint8_t r = (uint16_t)t8 * 255 / TH1;  // 0..255
      c = RgbColor(r, 0, 0);
    } else if (t8 < TH2) {
      uint8_t g = (uint16_t)(t8 - TH1) * 255 / (TH2 - TH1);
      c = RgbColor(255, g, 0);
    } else {
      uint8_t b = (uint16_t)(t8 - TH2) * 255 / (255 - TH2);
      c = RgbColor(255, 255, b);
    }
    setRing(i, c);
  }
}

// --- NEW: Palette Cycle ---
static void animPaletteCycle() {
  uint16_t L = ringLen(); if (!L) return;
  uint8_t n; RgbColor pal[4]; loadPalette(n, pal);

  int denom = 6 - (CFG.speed/51); if (denom < 1) denom = 1;
  float offset = (tick / (float)denom) * 0.015f; // rotation factor
  for (uint16_t i=0;i<L;++i) {
    float x = (i / (float)L) + offset;
    setRing(i, samplePalette(x, n, pal, CFG.intensity));
  }
}

// --- NEW: Palette Chase ---
static void animPaletteChase() {
  uint16_t L = ringLen(); if (!L) return;
  uint8_t n; RgbColor pal[4]; loadPalette(n, pal);

  uint16_t block = (CFG.width < 1) ? 1 : CFG.width;
  int denom = 4 - (CFG.speed/64); if (denom < 1) denom = 1;
  uint16_t pos = (tick / (uint16_t)denom) % L;

  for (uint16_t i=0;i<L;++i) {
    uint16_t k = (i + L - pos) % L;   // shift by pos for motion
    uint16_t which = (k / block) % n; // palette index
    RgbColor base = pal[which];

    if (CFG.intensity == 0) { setRing(i, base); continue; }

    // Soft edges using intensity as softening at block edges
    uint16_t edge = k % block;
    float tEdge = fabsf((edge - (block-1)/2.0f)) / (block/2.0f); // 0 center .. 1 edge
    float soft = 1.0f - (CFG.intensity/255.0f) * tEdge;          // 1 center .. (1-α) edge
    if (soft < 0.f) soft = 0.f;
    RgbColor c = RgbColor(
      (uint8_t)(base.R * soft),
      (uint8_t)(base.G * soft),
      (uint8_t)(base.B * soft)
    );
    setRing(i, c);
  }
}

// -------------------- NEW: Custom sequence (playlist) --------------------
struct CustomStep {
  uint8_t  mode = MODE_SOLID;
  uint16_t duration = 1000; // ms
  // Optional overrides
  bool hasSpeed=false;     uint8_t speed=0;
  bool hasIntensity=false; uint8_t intensity=0;
  bool hasWidth=false;     uint8_t width=0;
  bool hasPCnt=false;      uint8_t pcount=0;
  bool hasA=false;         uint32_t colorA=0;
  bool hasB=false;         uint32_t colorB=0;
  bool hasC=false;         uint32_t colorC=0;
  bool hasD=false;         uint32_t colorD=0;
};

static bool parseCustomSteps(const String& js, std::vector<CustomStep>& out) {
  out.clear();
  if (!js.length()) return true;
  StaticJsonDocument<2048> doc;
  auto err = deserializeJson(doc, js);
  if (err || !doc.is<JsonArray>()) return false;
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (!v.is<JsonObject>()) continue;
    JsonObject o = v.as<JsonObject>();
    CustomStep s;
    s.mode = (uint8_t) (o.containsKey("mode") ? o["mode"].as<int>() : MODE_SOLID);
    int dur = o.containsKey("duration") ? o["duration"].as<int>() : 1000;
    if (dur < 1) dur = 1; if (dur > 60000) dur = 60000;
    s.duration = (uint16_t)dur;
    if (o.containsKey("speed"))       { s.hasSpeed=true;     s.speed=o["speed"].as<uint8_t>(); }
    if (o.containsKey("intensity"))   { s.hasIntensity=true; s.intensity=o["intensity"].as<uint8_t>(); }
    if (o.containsKey("width"))       { s.hasWidth=true;     int w=o["width"].as<int>(); if(w<1)w=1; if(w>255)w=255; s.width=(uint8_t)w; }
    if (o.containsKey("paletteCount")){ s.hasPCnt=true;      uint8_t pc=o["paletteCount"].as<uint8_t>(); s.pcount=(pc<1)?1:((pc>4)?4:pc); }
    if (o.containsKey("colorA"))      { s.hasA=true; s.colorA=o["colorA"].as<uint32_t>(); }
    if (o.containsKey("colorB"))      { s.hasB=true; s.colorB=o["colorB"].as<uint32_t>(); }
    if (o.containsKey("colorC"))      { s.hasC=true; s.colorC=o["colorC"].as<uint32_t>(); }
    if (o.containsKey("colorD"))      { s.hasD=true; s.colorD=o["colorD"].as<uint32_t>(); }
    out.push_back(s);
  }
  return true;
}

static void applyStepOverrides(const CustomStep& s) {
  if (s.hasSpeed)     CFG.speed = s.speed;
  if (s.hasIntensity) CFG.intensity = s.intensity;
  if (s.hasWidth)     CFG.width = s.width;
  if (s.hasPCnt)      CFG.paletteCount = s.pcount;
  if (s.hasA)         CFG.colorA = s.colorA;
  if (s.hasB)         CFG.colorB = s.colorB;
  if (s.hasC)         CFG.colorC = s.colorC;
  if (s.hasD)         CFG.colorD = s.colorD;
}

static void animCustom() {
  static std::vector<CustomStep> seq;
  static String lastJs;
  static uint32_t stepStart=0;
  static size_t idx=0;

  // (Re)parse if changed
  if (lastJs != CFG.customSeq) {
    seq.clear();
    parseCustomSteps(CFG.customSeq, seq);
    lastJs = CFG.customSeq;
    idx = 0; stepStart = millis();
  }

  if (seq.empty()) {
    // No steps → black (silence)
    fillRing(RgbColor(0,0,0));
    return;
  }

  uint32_t now = millis();
  const CustomStep& s = seq[idx];

  // Apply per-step parameter overrides when entering a step
  static size_t lastIdx = (size_t)-1;
  if (idx != lastIdx) {
    applyStepOverrides(s);
    lastIdx = idx;
  }

  // Run selected base mode with current CFG (overrides applied)
  switch (s.mode) {
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
    default:                 animSolid();         break;
  }

  // Step advance
  if (now - stepStart >= s.duration) {
    stepStart = now;
    idx++;
    if (idx >= seq.size()) {
      idx = CFG.customLoop ? 0 : (seq.size()-1);
    }
  }
}

// -------------------- Frame selection --------------------
static void renderFrame() {
  // --- NEW: Master Off (force black regardless of mode) ---
  if (CFG.masterOff) {
    fillRing(RgbColor(0,0,0));
    showRing();
    return;
  }

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

    // NEW: Custom playlist
    case MODE_CUSTOM:        animCustom();        break;

    default: break;
  }
  showRing();
}

// -------------------- Persistence --------------------
static const char* NVS_NS = "rgbctrl";
static const char* NVS_KEY= "config";

static void defaults() { CFG = AppConfig(); }

static String configToJson() {
  StaticJsonDocument<2048> doc;
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

  // per-channel reverse
  JsonArray rev = doc.createNestedArray("reverse");
  for (uint8_t i=0;i<NUM_CH;++i) rev.add(CFG.reverse[i]);

  // NEW fields
  doc["masterOff"]   = CFG.masterOff;
  doc["customSeq"]   = CFG.customSeq;
  doc["customLoop"]  = CFG.customLoop;

  // Non-persistent display info
  doc["buildVersion"] = APP_VERSION;
  doc["copyright"]    = COPYRIGHT_TXT;

  String js; serializeJson(doc, js); return js;
}
static bool parseConfig(const String& body, AppConfig& out) {
  StaticJsonDocument<2048> doc;
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

  // per-channel reverse
  if (doc.containsKey("reverse")) {
    for (uint8_t i=0;i<NUM_CH;++i) {
      if (!doc["reverse"][i].isNull()) {
        out.reverse[i] = doc["reverse"][i].as<bool>();
      }
    }
  }

  // NEW fields
  if (doc.containsKey("masterOff"))   out.masterOff = doc["masterOff"].as<bool>();
  if (doc.containsKey("customLoop"))  out.customLoop= doc["customLoop"].as<bool>();
  if (doc.containsKey("customSeq"))   out.customSeq = doc["customSeq"].as<const char*>();

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

  StaticJsonDocument<2048> doc;
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

    // per-channel reverse
    if (doc.containsKey("reverse")) {
      for (uint8_t i=0;i<NUM_CH;++i) {
        if (!doc["reverse"][i].isNull()) {
          tmp.reverse[i] = doc["reverse"][i].as<bool>();
        }
      }
    }

    // NEW fields
    if (doc.containsKey("masterOff"))   tmp.masterOff = doc["masterOff"].as<bool>();
    if (doc.containsKey("customLoop"))  tmp.customLoop= doc["customLoop"].as<bool>();
    if (doc.containsKey("customSeq"))   tmp.customSeq = doc["customSeq"].as<const char*>();

    CFG = tmp;
  }
}
static void saveConfig() {
  StaticJsonDocument<2048> doc;
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

  // per-channel reverse
  JsonArray rev = doc.createNestedArray("reverse");
  for (uint8_t i=0;i<NUM_CH;++i) rev.add(CFG.reverse[i]);

  // NEW fields
  doc["masterOff"]   = CFG.masterOff;
  doc["customSeq"]   = CFG.customSeq;
  doc["customLoop"]  = CFG.customLoop;

  String js; serializeJson(doc, js);
  prefs.begin(NVS_NS, false);
  prefs.putString(NVS_KEY, js);
  prefs.end();
}

// -------------------- Web UI --------------------
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
fieldset{border:1px solid #273657;border-radius:12px;padding:8px 10px}
legend{padding:0 6px;color:#9ec1ff;font-size:12px}
.inline{display:flex;gap:10px;flex-wrap:wrap}
.inline>label{display:flex;align-items:center;gap:6px;margin:0}
textarea{width:100%;min-height:120px;border-radius:10px;border:1px solid #2a3142;background:#0b0e14;color:#d6e1ff;padding:10px 12px}
code{background:#0b1220;border:1px solid #273657;border-radius:6px;padding:2px 6px}
/* Playlist editor */
.plist{display:flex;flex-direction:column;gap:10px;margin-top:10px}
.step{border:1px solid #273657;border-radius:10px;padding:10px;background:#0b1220}
.step .grid{display:grid;grid-template-columns:repeat(12,1fr);gap:10px}
.step .grid>div{grid-column:span 12}
@media(min-width:900px){
  .step .grid .col-2{grid-column:span 2}
  .step .grid .col-3{grid-column:span 3}
  .step .grid .col-4{grid-column:span 4}
}
.btn-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:6px}
.btn-xs{padding:6px 10px;border-radius:8px;border:1px solid #2a3142;background:#10182a;color:#d6e1ff;cursor:pointer}
.btn-xs:hover{filter:brightness(1.1)}
.muted{color:#90a4c9;font-size:12px}
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
        <option value="14">Custom (Playlist)</option>
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

    <!-- per-channel reverse toggles -->
    <div class="md-12">
      <fieldset>
        <legend>Channel Direction</legend>
        <div class="inline">
          <label><input id="rev0" type="checkbox"> Reverse CH1 (Front)</label>
          <label><input id="rev1" type="checkbox"> Reverse CH2 (Left)</label>
          <label><input id="rev2" type="checkbox"> Reverse CH3 (Rear)</label>
          <label><input id="rev3" type="checkbox"> Reverse CH4 (Right)</label>
        </div>
      </fieldset>
    </div>

    <!-- NEW: Master Off -->
    <div class="md-12">
      <fieldset>
        <legend>Master</legend>
        <div class="inline">
          <label><input id="masterOff" type="checkbox"> Master Off (blank all channels)</label>
        </div>
      </fieldset>
    </div>

    <!-- NEW: Custom Playlist Editor -->
    <div class="md-12 opt opt-custom hide">
      <fieldset>
        <legend>Custom Playlist</legend>
        <div class="inline" style="align-items:center">
          <label><input id="customLoop" type="checkbox" checked> Loop playlist</label>
        </div>
        <label>Steps (JSON array)</label>
        <textarea id="customSeq" rows="8"></textarea>
        <div class="hint">
          Build from existing modes for stability. Example:
          <code>[{"mode":0,"duration":1000,"colorA":16711680},{"mode":7,"duration":1200,"speed":200,"width":6},{"mode":12,"duration":1500,"paletteCount":3}]</code>
        </div>
      </fieldset>
    </div>

    <!-- NEW: Custom Playlist Editor (Visual) -->
    <div class="md-12 opt opt-custom hide">
      <fieldset>
        <legend>Custom Playlist</legend>
        <div class="inline" style="align-items:center">
          <label><input id="customLoop" type="checkbox" checked> Loop playlist</label>
          <button id="addStep" type="button" class="btn-xs">Add Step</button>
          <button id="clearSteps" type="button" class="btn-xs">Clear</button>
          <span class="muted">Drag not required: use Up/Down per step</span>
        </div>
        <div id="plist" class="plist"></div>
        <!-- Keep a hidden field with JSON for firmware compatibility -->
        <textarea id="customSeq" class="hide" rows="1"></textarea>
        <div class="hint">
          The editor builds the playlist for you. Each step plays one built-in mode for a duration.
        </div>
      </fieldset>
    </div>

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


// Labels for per-step Mode selector (indexes must match main Mode list)
const MODE_LABELS=["Solid","Breathe","Color Wipe","Larson","Rainbow","Theater Chase","Twinkle","Comet","Meteor","Clock Spin","Plasma","Fire / Flicker","Palette Cycle","Palette Chase"];

function showOptsFor(mode){
  const vis = {
    colorA:   [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14],
    colorB:   [8,9,10,12,13,14],
    colorC:   [12,13,14],
    colorD:   [12,13,14],
    palette:  [12,13,14],
    width:    [3,5,7,8,9,13,14],        // Palette Chase & Custom
    intensity:[3,5,6,7,8,11,12,13,14],  // palette blend / soft edges
    custom:   [14]
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
  toggle('.opt-custom', on('custom'));

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

  // reverse flags
  const rev = s.reverse || [false,false,false,false];
  for(let i=0;i<4;i++) { const n = el('rev'+i); if (n) n.checked = !!rev[i]; }

  // NEW
  el('masterOff').checked = !!s.masterOff;
  el('customLoop').checked = !!s.customLoop;
  el('customSeq').value = (s.customSeq && String(s.customSeq).length) ? s.customSeq : "[]";

  el('resume').value    = s.resumeOnBoot ? 'true' : 'false';
  el('smbusCpu').checked= !!s.enableCpu;
  el('smbusFan').checked= !!s.enableFan;

  // footer text (always shown)
  el('ver').textContent = 'v' + (s.buildVersion || '—');
  el('cpy').textContent = s.copyright || '© Darkone Customs 2025';

  showOptsFor(s.mode|0);

  // Build the visual playlist from saved JSON
  try {
    const steps = JSON.parse(s.customSeq || "[]");
    setPlaylistUI(Array.isArray(steps) ? steps : []);
  } catch(_e){
    setPlaylistUI([]);
  }
}

function gather(){
  const reverse = [0,1,2,3].map(i => !!el('rev'+i).checked);
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
    reverse:reverse,
    resumeOnBoot:(el('resume').value==='true'),
    enableCpu:el('smbusCpu').checked,
    enableFan:el('smbusFan').checked,

    // NEW
    masterOff: el('masterOff').checked,
    customLoop: el('customLoop').checked,
    customSeq: (el('customSeq').value || "[]"), // kept in sync by the visual editor
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
['brightness','speed','intensity','width','colorA','colorB','colorC','colorD','resume','smbusCpu','smbusFan',
 'rev0','rev1','rev2','rev3','c0','c1','c2','c3',
 'masterOff','customLoop']
  .forEach(id => bind(id, preview));

// ------------ Custom Playlist UI (visual builder) ------------
function stepTemplate() {
  // A row with all fields the firmware understands; we always include values for predictability
  return `
    <div class="step">
      <div class="grid">
        <div class="col-3">
          <label>Mode</label>
          <select data-f="mode" class="mode-select"></select>
        </div>
        <div class="col-3">
          <label>Duration (ms)</label>
          <input data-f="dur" class="num" type="number" min="1" max="60000" value="1000">
        </div>
        <div class="col-2">
          <label>Speed</label>
          <input data-f="speed" class="rng" type="range" min="0" max="255" value="128">
        </div>
        <div class="col-2">
          <label>Intensity</label>
          <input data-f="intensity" class="rng" type="range" min="0" max="255" value="128">
        </div>
        <div class="col-2">
          <label>Width</label>
          <input data-f="width" class="rng" type="range" min="1" max="20" value="4">
        </div>

        <div class="col-2">
          <label>Palette Size</label>
          <select data-f="pcnt">
            <option value="1">1</option>
            <option value="2" selected>2</option>
            <option value="3">3</option>
            <option value="4">4</option>
          </select>
        </div>

        <div class="col-3"><label>Color A</label><input data-f="a" class="clr" type="color" value="#ff0000"></div>
        <div class="col-3"><label>Color B</label><input data-f="b" class="clr" type="color" value="#ffa000"></div>
        <div class="col-3"><label>Color C</label><input data-f="c" class="clr" type="color" value="#00ff00"></div>
        <div class="col-3"><label>Color D</label><input data-f="d" class="clr" type="color" value="#0000ff"></div>
      </div>
      <div class="btn-row">
        <button type="button" data-act="up" class="btn-xs">↑ Up</button>
        <button type="button" data-act="down" class="btn-xs">↓ Down</button>
        <button type="button" data-act="dup" class="btn-xs">Duplicate</button>
        <button type="button" data-act="del" class="btn-xs">Delete</button>
      </div>
    </div>
  `;
}

function makeModeOptions(sel){
  sel.innerHTML = MODE_LABELS.map((label, i) => `<option value="${i}">${label}</option>`).join('');
  // Note: mode 14 = Custom is not for steps; we leave it out on purpose
}

function rowToStep(row){
  const q = s => row.querySelector(s);
  return {
    mode: +q('[data-f=mode]').value,
    duration: Math.max(1, Math.min(60000, +q('[data-f=dur]').value || 1000)),
    speed: +q('[data-f=speed]').value,
    intensity: +q('[data-f=intensity]').value,
    width: +q('[data-f=width]').value,
    paletteCount: +q('[data-f=pcnt]').value,
    colorA: to24(q('[data-f=a]').value),
    colorB: to24(q('[data-f=b]').value),
    colorC: to24(q('[data-f=c]').value),
    colorD: to24(q('[data-f=d]').value),
  };
}

function applyStepToRow(row, s){
  const q = sel => row.querySelector(sel);
  makeModeOptions(q('[data-f=mode]'));
  q('[data-f=mode]').value = (s.mode ?? 0);
  q('[data-f=dur]').value = (s.duration ?? 1000);
  q('[data-f=speed]').value = (s.speed ?? 128);
  q('[data-f=intensity]').value = (s.intensity ?? 128);
  q('[data-f=width]').value = (s.width ?? 4);
  q('[data-f=pcnt]').value = (s.paletteCount ?? 2);
  q('[data-f=a]').value = hex24(s.colorA ?? 0xFF0000);
  q('[data-f=b]').value = hex24(s.colorB ?? 0xFFA000);
  q('[data-f=c]').value = hex24(s.colorC ?? 0x00FF00);
  q('[data-f=d]').value = hex24(s.colorD ?? 0x0000FF);
}

function syncHiddenFromUI(){
  const rows = Array.from(document.querySelectorAll('#plist .step'));
  const steps = rows.map(rowToStep);
  el('customSeq').value = JSON.stringify(steps);
}

function attachRowActions(row){
  const plist = el('plist');
  const act = (sel, fn) => row.querySelector(sel).addEventListener('click', fn);
  act('[data-act=del]', () => { row.remove(); syncHiddenFromUI(); preview(); });
  act('[data-act=dup]', () => {
    const clone = row.cloneNode(true);
    plist.insertBefore(clone, row.nextSibling);
    // reattach listeners and keep values
    attachRowEvents(clone);
    syncHiddenFromUI(); preview();
  });
  act('[data-act=up]', () => {
    const prev = row.previousElementSibling;
    if (prev) plist.insertBefore(row, prev);
    syncHiddenFromUI(); preview();
  });
  act('[data-act=down]', () => {
    const next = row.nextElementSibling;
    if (next) plist.insertBefore(next, row);
    syncHiddenFromUI(); preview();
  });
}

function attachRowEvents(row){
  // inputs that affect JSON/preview
  row.querySelectorAll('input,select').forEach(n => {
    const ev = (n.tagName === 'SELECT' || n.type === 'checkbox') ? 'change' : 'input';
    n.addEventListener(ev, () => { syncHiddenFromUI(); preview(); });
  });
  attachRowActions(row);
}

function addStepRow(step){
  const wrap = document.createElement('div');
  wrap.innerHTML = stepTemplate();
  const row = wrap.firstElementChild;
  applyStepToRow(row, step || {});
  el('plist').appendChild(row);
  attachRowEvents(row);
}

function setPlaylistUI(steps){
  const plist = el('plist');
  plist.innerHTML = '';
  const arr = (Array.isArray(steps) && steps.length) ? steps : [ { mode:0, duration:1000 } ];
  arr.forEach(s => addStepRow(s));
  syncHiddenFromUI();
}

// Add/Clear buttons for playlist
document.addEventListener('click', (e) => {
  if (e.target && e.target.id === 'addStep') {
    // Seed new step from current global controls so it feels intuitive
    const step = {
      mode: 0,
      duration: 1000,
      speed: +el('speed').value || 128,
      intensity: +el('intensity').value || 128,
      width: +el('width').value || 4,
      paletteCount: +el('paletteCount').value || 2,
      colorA: to24(el('colorA').value),
      colorB: to24(el('colorB').value),
      colorC: to24(el('colorC').value),
      colorD: to24(el('colorD').value),
    };
    addStepRow(step);
    syncHiddenFromUI(); preview();
  }
  if (e.target && e.target.id === 'clearSteps') {
    el('plist').innerHTML = '';
    setPlaylistUI([]); // inserts one default step
    syncHiddenFromUI(); preview();
  }
});


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

  // Bind pins/length, then init (start at 0 brightness so fade begins from black)
  strip1.updateLength(MAX_PER_CH); strip1.setPin(PINS.ch1);
  strip2.updateLength(MAX_PER_CH); strip2.setPin(PINS.ch2);
  strip3.updateLength(MAX_PER_CH); strip3.setPin(PINS.ch3);
  strip4.updateLength(MAX_PER_CH); strip4.setPin(PINS.ch4);

  for (uint8_t s=0;s<NUM_CH;++s) {
    STRIPS[s]->begin();
    STRIPS[s]->clear();
    STRIPS[s]->setBrightness(0);   // start dark
    STRIPS[s]->show();
  }
  lastAppliedBrightness = 0;

  memset(heat, 0, sizeof(heat));

  // Load the LAST SAVED preferences from NVS on boot
  loadConfig();
  applyConfig();  // applies counts etc. (we will override brightness below)

  // Arm boot fade to target brightness
  bootFadeTarget   = CFG.brightness;
  bootFadeStartMs  = millis();
  bootFadeActive   = true;
  for (uint8_t s=0; s<NUM_CH; ++s) STRIPS[s]->setBrightness(0);
  lastAppliedBrightness = 0;

  // Render once; showRing() will apply the fade ramp
  renderFrame();
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
  uint8_t frameMs = 10 + (uint8_t)((255 - CFG.speed)/2); // faster speed -> shorter frame
  uint32_t now = millis();
  if (now - msPrev >= frameMs) {
    msPrev = now;
    ++tick;
    renderFrame();

    RGBCtrlUDP::processPending(1500);
  }
}

void setCounts(uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4) {
  CFG.count[0] = (c1 > MAX_PER_CH) ? MAX_PER_CH : c1;
  CFG.count[1] = (c2 > MAX_PER_CH) ? MAX_PER_CH : c2;
  CFG.count[2] = (c3 > MAX_PER_CH) ? MAX_PER_CH : c3;
  CFG.count[3] = (c4 > MAX_PER_CH) ? MAX_PER_CH : c4;
  rebuildRingMap();
}

void forceSave() { saveConfig(); }
void forceLoad() { loadConfig(); applyConfig(); }

// SMBus flags accessors (for RGBsmbus to check)
bool smbusCpuEnabled() { return CFG.enableCpu; }
bool smbusFanEnabled() { return CFG.enableFan; }

// -------------------- JSON helpers for UDP/external control --------------------
bool applyJsonPreview(const String& json) {
  AppConfig tmp = CFG;
  if (!parseConfig(json, tmp)) return false;
  CFG = tmp;
  inPreview = true;
  applyConfig();
  renderFrame();
  return true;
}

bool applyJsonSave(const String& json) {
  AppConfig tmp = CFG;
  if (!parseConfig(json, tmp)) return false;
  CFG = tmp;
  inPreview = false;
  applyConfig();
  saveConfig();
  renderFrame();
  return true;
}

String getConfigJson() {
  return configToJson();
}

void resetToDefaults() {
  // Match the web-reset behavior: erase saved prefs and apply defaults (no save)
  prefs.begin(NVS_NS, false);
  prefs.remove(NVS_KEY);
  prefs.end();

  defaults();
  inPreview = false;
  applyConfig();
  renderFrame();
}

} // namespace RGBCtrl
