//------------------------------//
// ROL mirrors all aniomations  //
// on ch7, fade in works as     //
// expected. Brightness         //
// control and master on and    // 
// oof all work as expected     //
//------------------------------//

#include "rol.h"
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <math.h>
#include <vector>

// Mirror RGBCtrl configuration without exposing its internals.
#include "RGBCtrl.h"  // for RGBCtrl::getConfigJson()
using namespace ROL;

static Adafruit_NeoPixel g_strip(20, 10, NEO_GRB + NEO_KHZ800);
static bool     g_inited       = false;
static bool     g_enabled      = true;
static uint32_t g_lastCfgMs    = 0;
static uint32_t g_tickMs       = 0;

// Boot fade state (mirror main ring behaviour)
static bool     g_bootFadeActive          = false;
static uint32_t g_bootFadeStartMs         = 0;
static uint16_t g_bootFadeDurationMs      = 3200;
static uint8_t  g_bootFadeTarget          = 0;
static uint8_t  g_lastAppliedBrightness   = 0;

// Cached config we care about (mirrors RGBCtrl keys).
struct Cfg {
  uint8_t  mode         = 4;     // rainbow default
  uint8_t  brightness   = 180;
  uint8_t  speed        = 128;
  uint8_t  intensity    = 128;
  uint8_t  width        = 4;
  uint8_t  paletteCount = 2;
  uint32_t colorA       = 0xFF0000;
  uint32_t colorB       = 0xFFA000;
  uint32_t colorC       = 0x00FF00;
  uint32_t colorD       = 0x0000FF;
  bool     masterOff    = false;
} CFG;

// ---- helpers ----
struct RGB { uint8_t r,g,b; };
static inline RGB rgb(uint8_t r, uint8_t g, uint8_t b){ return {r,g,b}; }
static inline RGB from24(uint32_t v){ return rgb((v>>16)&0xFF, (v>>8)&0xFF, v&0xFF); }

static RGB hsv2rgb(float h, float s, float v){
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
  return rgb(uint8_t(r*255), uint8_t(g*255), uint8_t(b*255));
}

static RGB wheel(uint8_t pos){
  if (pos < 85)   return rgb(255 - pos*3, pos*3, 0);
  if (pos < 170)  { pos -= 85; return rgb(0, 255 - pos*3, pos*3); }
  pos -= 170;     return rgb(pos*3, 0, 255 - pos*3);
}

// palette sampling (compatible with RGBCtrl’s semantics)
static inline uint8_t clampPC(uint8_t n){ return n<1?1:(n>4?4:n); }
static void loadPalette(uint8_t& n, RGB p[4]){
  n = clampPC(CFG.paletteCount);
  uint32_t src[4] = { CFG.colorA, CFG.colorB, CFG.colorC, CFG.colorD };
  for (uint8_t i=0;i<4;i++) p[i] = from24(src[i]);
}
static inline RGB lerp(const RGB&a, const RGB&b, float t){
  if (t<0) t=0; if (t>1) t=1;
  return rgb(uint8_t(a.r+(b.r-a.r)*t),
             uint8_t(a.g+(b.g-a.g)*t),
             uint8_t(a.b+(b.b-a.b)*t));
}
static RGB samplePalette(float x, uint8_t n, const RGB p[4], uint8_t blend){
  if (n==1) return p[0];
  float fx = x - floorf(x);
  float pos = fx * n;
  int i0 = (int)floorf(pos) % n;
  int i1 = (i0 + 1) % n;
  float t = pos - floorf(pos);
  if (blend == 0) return p[i0];
  float bw = (blend / 255.0f);
  return lerp(p[i0], p[i1], t * bw);
}

// ---- config polling ----
static void pullCfgIfNeeded(){
  uint32_t now = millis();
  if (now - g_lastCfgMs < 100) return; // ~10 Hz
  g_lastCfgMs = now;

  String js = RGBCtrl::getConfigJson();  // mirrors live state (no writes)  :contentReference[oaicite:0]{index=0}
  if (!js.length()) return;

  StaticJsonDocument<1536> doc;
  if (deserializeJson(doc, js)) return;

  auto getU8 = [&](const char* k, uint8_t& out){
    if (doc.containsKey(k)) out = doc[k].as<uint8_t>();
  };
  auto getU32 = [&](const char* k, uint32_t& out){
    if (doc.containsKey(k)) out = doc[k].as<uint32_t>();
  };
  getU8("mode", CFG.mode);
  getU8("brightness", CFG.brightness);
  getU8("speed", CFG.speed);
  getU8("intensity", CFG.intensity);
  getU8("width", CFG.width);
  getU8("paletteCount", CFG.paletteCount);
  getU32("colorA", CFG.colorA);
  getU32("colorB", CFG.colorB);
  getU32("colorC", CFG.colorC);
  getU32("colorD", CFG.colorD);
  if (doc.containsKey("masterOff")) CFG.masterOff = doc["masterOff"].as<bool>();
}

// ---- animations (mirrors RGBCtrl modes; simplified for 20px) ----
static void fill(const RGB& c){
  for (uint16_t i=0;i<g_strip.numPixels();++i) g_strip.setPixelColor(i, c.r, c.g, c.b);
}
static void fade(uint8_t amt){
  for (uint16_t i=0;i<g_strip.numPixels();++i){
    uint32_t packed = g_strip.getPixelColor(i);
    uint8_t g = (packed>>16)&0xFF, r = (packed>>8)&0xFF, b = packed&0xFF;
    r = (uint16_t)r*(255-amt)>>8;
    g = (uint16_t)g*(255-amt)>>8;
    b = (uint16_t)b*(255-amt)>>8;
    g_strip.setPixelColor(i, r,g,b);
  }
}

static void animSolid(){ fill(from24(CFG.colorA)); }

static void animBreathe(){
  static float phase=0.f;
  float step = 0.010f + (CFG.speed/255.0f)*0.045f;
  phase += step;
  float s = 0.5f + 0.5f*sinf(phase*6.2831853f);
  float eased = s*s*(3.f-2.f*s);
  float lvl = 0.10f + 0.90f*eased;
  RGB base = from24(CFG.colorA);
  RGB c = rgb(uint8_t(base.r*lvl), uint8_t(base.g*lvl), uint8_t(base.b*lvl));
  fill(c);
}

static void animColorWipe(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  fill(rgb(0,0,0));
  uint8_t n; RGB pal[4]; loadPalette(n, pal);
  float phase = (millis()/1000.0f) * (0.3f + (CFG.speed/255.0f)*0.9f);
  uint16_t idx = (millis()/40) % L;
  RGB c = samplePalette((idx/(float)L)+phase, n, pal, CFG.intensity);
  g_strip.setPixelColor(idx, c.r, c.g, c.b);
}

static void animLarson(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  int denom = 6 - (CFG.speed/51); if (denom < 1) denom = 1;
  uint16_t pos = (g_tickMs / (uint16_t)denom) % (L*2);
  if (pos >= L) pos = 2*L - 1 - pos;
  int fadeBase = 10 + CFG.intensity; if (fadeBase > 254) fadeBase = 254;
  uint8_t fadeAmt = (uint8_t)(255 - fadeBase);
  fade(fadeAmt);
  uint8_t n; RGB pal[4]; loadPalette(n, pal);
  float ph = g_tickMs * 0.006f;
  for (int w=-(int)CFG.width; w<=(int)CFG.width; ++w){
    int p = (int)pos + w;
    if (p>=0 && p<(int)L){
      RGB c = samplePalette((p/(float)L)+ph, n, pal, CFG.intensity);
      g_strip.setPixelColor(p, c.r, c.g, c.b);
    }
  }
}

static void animRainbow(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  int denom = 6 - (CFG.speed/51); if (denom < 1) denom = 1;
  uint8_t offset = g_tickMs / (uint8_t)denom;
  for (uint16_t i=0;i<L;++i){
    RGB c = wheel((i*256/L + offset) & 255);
    g_strip.setPixelColor(i, c.r, c.g, c.b);
  }
}

static void animTheater(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  int denom = 10 - (CFG.speed/32); if (denom < 1) denom = 1;
  uint8_t gap = (CFG.width < 1) ? 1 : CFG.width;
  uint8_t q = (g_tickMs / (uint8_t)denom) % gap;
  int fadeBase = 10 + CFG.intensity; if (fadeBase > 254) fadeBase = 254;
  uint8_t fadeAmt = (uint8_t)(255 - fadeBase);
  fade(fadeAmt);
  uint8_t n; RGB pal[4]; loadPalette(n, pal);
  float ph = g_tickMs * 0.0045f;
  for (uint16_t i=q; i<L; i+=gap){
    RGB c = samplePalette((i/(float)L)+ph, n, pal, CFG.intensity);
    g_strip.setPixelColor(i, c.r, c.g, c.b);
  }
}

static void animTwinkle(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  int f = 18 + (CFG.speed/2); if (f > 254) f = 254;
  uint8_t fadeAmt = (uint8_t)(255 - f);
  fade(fadeAmt);
  static uint8_t phase[128] = {0}; // plenty for 20px
  uint16_t pops = 1 + (uint16_t)((CFG.intensity * L) / (255 * 6) + 0.5f);
  for (uint16_t n=0; n<pops; ++n){
    uint16_t k = esp_random() % L;
    if (phase[k] == 0) phase[k] = 1 + (esp_random() & 1);
  }
  uint8_t pn; RGB pal[4]; loadPalette(pn, pal);
  float palPhase = g_tickMs * 0.0025f;
  int advance = 2 + (CFG.speed / 24) - (CFG.width / 6);
  if (advance < 1) advance = 1;
  for (uint16_t i=0; i<L; ++i){
    uint8_t ph = phase[i];
    if (!ph) continue;
    float x = ph / 255.0f;
    float b = sinf(3.1415926f * x); b=b*b*b;
    float u = (i/(float)L) + palPhase;
    RGB base = samplePalette(u, pn, pal, CFG.intensity);
    RGB c = rgb(uint8_t(base.r*b), uint8_t(base.g*b), uint8_t(base.b*b));
    g_strip.setPixelColor(i, c.r, c.g, c.b);
    uint16_t next = ph + advance;
    phase[i] = (next >= 255) ? 0 : (uint8_t)next;
  }
}

static void animComet(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  int denom = 4 - (CFG.speed/64); if (denom < 1) denom = 1;
  uint16_t pos = (g_tickMs / (uint16_t)denom) % L;
  uint8_t fadeAmt = (uint8_t)(200 - (CFG.intensity > 199 ? 199 : CFG.intensity));
  fade(fadeAmt);
  uint8_t n; RGB pal[4]; loadPalette(n, pal);
  float ph = g_tickMs * 0.0055f;
  RGB head = samplePalette((pos/(float)L)+ph, n, pal, CFG.intensity);
  for (uint8_t w=0; w<((CFG.width<1)?1:CFG.width); ++w){
    float tail = 1.0f - (w/(float)((CFG.width<1)?1:CFG.width));
    RGB c = rgb(uint8_t(head.r*tail), uint8_t(head.g*tail), uint8_t(head.b*tail));
    g_strip.setPixelColor((pos + L - w) % L, c.r, c.g, c.b);
  }
}

static void animMeteor(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  uint8_t fadeAmt = (uint8_t)(210 - (CFG.intensity > 209 ? 209 : CFG.intensity));
  fade(fadeAmt);
  const uint8_t MAXM = 4;
  static bool inited=false; static float pos[MAXM]; static float vel[MAXM]; static uint8_t len[MAXM];
  if (!inited){
    for (uint8_t m=0;m<MAXM;++m){ pos[m]=esp_random()%L; vel[m]=0.35f+1.25f*((esp_random()&255)/255.0f); len[m]=2+(esp_random()%4); }
    inited=true;
  }
  uint8_t baseTail = 1 + (uint8_t)(CFG.width * 2);
  uint8_t pn; RGB pal[4]; loadPalette(pn, pal);
  float pphase = g_tickMs * 0.004f;
  float speedMul = 0.5f + 2.0f * (CFG.speed / 255.0f);
  for (uint8_t m=0;m<MAXM;++m){
    pos[m] += vel[m] * speedMul; while (pos[m]>=L) pos[m]-=L;
    float hu = (pos[m]/(float)L) + pphase;
    RGB head = samplePalette(hu, pn, pal, CFG.intensity);
    g_strip.setPixelColor((uint16_t)pos[m], head.r, head.g, head.b);
    uint8_t tl = baseTail + len[m];
    for (uint8_t k=1;k<=tl;++k){
      float t = k/(float)tl; float fall = (1.0f - t); fall*=fall;
      RGB c = rgb(uint8_t(head.r*fall), uint8_t(head.g*fall), uint8_t(head.b*fall));
      uint16_t p = ((int)pos[m] - k + L*4) % L;
      g_strip.setPixelColor(p, c.r, c.g, c.b);
    }
  }
}

static void animClockSpin(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  int denom = 3 - (CFG.speed/85); if (denom < 1) denom = 1;
  uint16_t pos = (g_tickMs / (uint16_t)denom) % L;
  RGB bg = from24(CFG.colorB), fg = from24(CFG.colorA);
  fill(bg);
  uint8_t span = (uint8_t)(CFG.width*2+1); if (span<1) span=1;
  for (uint8_t w=0; w<span; ++w) g_strip.setPixelColor((pos+w)%L, fg.r, fg.g, fg.b);
}

static void animPlasma(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  static float t=0.f;
  float tstep = 0.015f + (CFG.speed / 255.0f) * 0.050f; t += tstep;
  float sat = 0.55f + (CFG.intensity/255.0f)*0.45f;
  float contrast = 0.90f + (CFG.width/20.0f)*0.60f;
  for (uint16_t i=0;i<L;++i){
    float u = (float)i/L;
    float a = u*6.2831853f;
    float f1 = sinf(3.0f*a + t)*0.55f;
    float f2 = sinf(5.0f*a - t*0.8f)*0.35f;
    float f3 = sinf(6.3f*a + t*1.6f)*0.20f;
    float field = (f1+f2+f3)*0.5f+0.5f;
    float v = field*contrast; if (v<0) v=0; if (v>1) v=1;
    float hue = fmodf(field*1.2f + t*0.05f, 1.0f);
    RGB c = hsv2rgb(hue, sat, v);
    g_strip.setPixelColor(i, c.r, c.g, c.b);
  }
}

static void animFire(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  static uint8_t heat[128] = {0};
  uint8_t cool = 50 - (uint8_t)((uint16_t)CFG.intensity * 36 / 255);
  for (uint16_t i=0;i<L;++i){
    uint8_t dec = esp_random() % (cool + 1);
    heat[i] = (heat[i] > dec) ? (heat[i] - dec) : 0;
  }
  for (uint16_t i=0;i<L;++i){
    uint16_t i1 = (i+L-1)%L, i2=(i+1)%L;
    heat[i] = (uint8_t)((heat[i]+heat[i1]+heat[i2])/3);
  }
  uint8_t sparks = 1 + (CFG.speed/64);
  for (uint8_t s=0;s<sparks;++s){
    uint16_t p = esp_random()%L;
    uint16_t v = (uint16_t)heat[p] + 180 + (esp_random()%96);
    heat[p] = (v>255)?255:(uint8_t)v;
  }
  for (uint16_t i=0;i<L;++i){
    uint8_t t8 = heat[i] + 65; if (t8<heat[i]) t8=255; // saturate
    uint8_t r,g,b;
    if (t8<35){ r = (uint16_t)t8*255/35; g=0; b=0; }
    else if (t8<160){ g=(uint16_t)(t8-35)*255/(160-35); r=255; b=0; }
    else { b=(uint16_t)(t8-160)*255/(255-160); r=255; g=255; }
    g_strip.setPixelColor(i, r,g,b);
  }
}

static void animPaletteCycle(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  uint8_t n; RGB pal[4]; loadPalette(n, pal);
  int denom = 6 - (CFG.speed/51); if (denom < 1) denom = 1;
  float offset = (g_tickMs / (float)denom) * 0.015f;
  for (uint16_t i=0;i<L;++i){
    float x = (i/(float)L) + offset;
    RGB c = samplePalette(x, n, pal, CFG.intensity);
    g_strip.setPixelColor(i, c.r, c.g, c.b);
  }
}

static void animPaletteChase(){
  uint16_t L = g_strip.numPixels(); if (!L) return;
  uint8_t n; RGB pal[4]; loadPalette(n, pal);
  uint16_t block = (CFG.width<1)?1:CFG.width;
  int denom = 4 - (CFG.speed/64); if (denom < 1) denom = 1;
  uint16_t pos = (g_tickMs / (uint16_t)denom) % L;
  for (uint16_t i=0;i<L;++i){
    uint16_t k = (i + L - pos) % L;
    uint16_t which = (k / block) % n;
    RGB base = pal[which];
    if (CFG.intensity == 0){ g_strip.setPixelColor(i, base.r, base.g, base.b); continue; }
    uint16_t edge = k % block;
    float tEdge = fabsf((edge - (block-1)/2.0f)) / (block/2.0f);
    float soft = 1.0f - (CFG.intensity/255.0f) * tEdge;
    if (soft<0) soft=0;
    RGB c = rgb(uint8_t(base.r*soft), uint8_t(base.g*soft), uint8_t(base.b*soft));
    g_strip.setPixelColor(i, c.r, c.g, c.b);
  }
}

// ---- Mode 14: Custom Playlist (Simplified Engine) ----

struct SimpleStep {
  uint8_t  mode = 0;
  uint16_t duration = 1000;
  bool hasSpeed = false;     uint8_t speed = 128;
  bool hasIntensity = false; uint8_t intensity = 128;
  bool hasWidth = false;     uint8_t width = 4;
  bool hasA = false;         uint32_t colorA = 0xFF0000;
  bool hasB = false;         uint32_t colorB = 0xFFA000;
};

// Parse customSeq from CFG into simple steps
static bool parseSimpleSteps(std::vector<SimpleStep>& out) {
  out.clear();
  
  // We need to access CFG which has customSeq
  // For now, we'll parse from RGBCtrl's config JSON
  String js = RGBCtrl::getConfigJson();
  if (!js.length()) return false;
  
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, js)) return false;
  
  if (!doc.containsKey("customSeq")) return false;
  
  String seqStr = doc["customSeq"].as<const char*>();
  if (!seqStr.length() || seqStr == "[]") return false;
  
  StaticJsonDocument<2048> seqDoc;
  if (deserializeJson(seqDoc, seqStr)) return false;
  
  if (!seqDoc.is<JsonArray>()) return false;
  JsonArray arr = seqDoc.as<JsonArray>();
  
  for (JsonVariant v : arr) {
    if (!v.is<JsonObject>()) continue;
    JsonObject o = v.as<JsonObject>();
    
    SimpleStep s;
    s.mode = (uint8_t)(o.containsKey("mode") ? o["mode"].as<int>() : 0);
    
    // Skip MODE_CUSTOM (14) and MODE_UNSC_COVENANT (15) to avoid recursion/complexity
    if (s.mode >= 14) s.mode = 0;
    
    int dur = o.containsKey("duration") ? o["duration"].as<int>() : 1000;
    if (dur < 1) dur = 1;
    if (dur > 60000) dur = 60000;
    s.duration = (uint16_t)dur;
    
    // Optional overrides
    if (o.containsKey("speed")) { 
      s.hasSpeed = true; 
      s.speed = o["speed"].as<uint8_t>(); 
    }
    if (o.containsKey("intensity")) { 
      s.hasIntensity = true; 
      s.intensity = o["intensity"].as<uint8_t>(); 
    }
    if (o.containsKey("width")) { 
      s.hasWidth = true; 
      int w = o["width"].as<int>();
      if (w < 1) w = 1;
      if (w > 255) w = 255;
      s.width = (uint8_t)w;
    }
    if (o.containsKey("colorA")) { 
      s.hasA = true; 
      s.colorA = o["colorA"].as<uint32_t>(); 
    }
    if (o.containsKey("colorB")) { 
      s.hasB = true; 
      s.colorB = o["colorB"].as<uint32_t>(); 
    }
    
    out.push_back(s);
  }
  
  return true;
}

// Apply step overrides to current CFG (saved to restore later)
static void applySimpleStepOverrides(const SimpleStep& s, Cfg& savedCfg) {
  savedCfg = CFG;
  CFG.mode = s.mode;
  if (s.hasSpeed)     CFG.speed     = s.speed;
  if (s.hasIntensity) CFG.intensity = s.intensity;
  if (s.hasWidth)     CFG.width     = s.width;
  if (s.hasA)         CFG.colorA    = s.colorA;
  if (s.hasB)         CFG.colorB    = s.colorB;
}

static void animCustom(){
  static std::vector<SimpleStep> seq;
  static String lastSeq;
  static uint32_t stepStart = 0;
  static size_t idx = 0;
  static bool firstRun = true;
  static Cfg savedCfg;
  static bool inStep = false;
  
  String js = RGBCtrl::getConfigJson();
  StaticJsonDocument<1536> doc;
  if (!deserializeJson(doc, js) && doc.containsKey("customSeq")) {
    String currentSeq = doc["customSeq"].as<const char*>();
    
    if (currentSeq != lastSeq || firstRun) {
      seq.clear();
      if (parseSimpleSteps(seq)) {
        lastSeq = currentSeq;
        idx = 0;
        stepStart = millis();
        inStep = false;
      }
      firstRun = false;
    }
  }
  
  if (seq.empty()) {
    uint16_t L = g_strip.numPixels();
    static float phase = 0.f;
    float step = 0.020f + (CFG.speed/255.0f)*0.040f;
    phase += step;
    float pulse = 0.3f + 0.7f * (0.5f + 0.5f * sinf(phase * 3.1415926f));
    for (uint16_t i=0; i<L; ++i){
      float hue = fmodf((i/(float)L) + phase * 0.1f, 1.0f);
      RGB c = hsv2rgb(hue, 0.8f, pulse);
      g_strip.setPixelColor(i, c.r, c.g, c.b);
    }
    return;
  }
  
  uint32_t now = millis();
  const SimpleStep& s = seq[idx];
  
  if (!inStep) {
    applySimpleStepOverrides(s, savedCfg);
    inStep = true;
  }
  
  switch (s.mode) {
    case 0:  animSolid();         break;
    case 1:  animBreathe();       break;
    case 2:  animColorWipe();     break;
    case 3:  animLarson();        break;
    case 4:  animRainbow();       break;
    case 5:  animTheater();       break;
    case 6:  animTwinkle();       break;
    case 7:  animComet();         break;
    case 8:  animMeteor();        break;
    case 9:  animClockSpin();     break;
    case 10: animPlasma();        break;
    case 11: animFire();          break;
    case 12: animPaletteCycle();  break;
    case 13: animPaletteChase();  break;
    default: animSolid();         break;
  }
  
  if (now - stepStart >= s.duration) {
    stepStart = now;
    idx++;
    inStep = false;
    
    bool shouldLoop = true;
    if (doc.containsKey("customLoop")) {
      shouldLoop = doc["customLoop"].as<bool>();
    }
    
    if (idx >= seq.size()) {
      if (shouldLoop) {
        idx = 0;
      } else {
        idx = seq.size() - 1;
      }
    }
    
    CFG = savedCfg;
  }
}

static void animUNSCCovenant(){
  uint16_t L = g_strip.numPixels(); if (!L) return;

  uint16_t block = (CFG.width < 2) ? 2 : CFG.width;
  int denomMain = 5 - (CFG.speed / 64); if (denomMain < 1) denomMain = 1;
  
  uint16_t posCW  = (g_tickMs / (uint16_t)denomMain) % L;
  uint16_t posCCW = (L - posCW) % L;
  
  bool flip = ((millis() / 8000U) & 1);
  
  RGB unscA = (CFG.paletteCount >= 4) ? from24(CFG.colorA) : rgb(0x6B, 0x8E, 0xFF);
  RGB unscB = (CFG.paletteCount >= 4) ? from24(CFG.colorB) : rgb(0x40, 0x60, 0xD0);
  
  RGB covA = (CFG.paletteCount >= 4) ? from24(CFG.colorC) : rgb(0xE6, 0x66, 0xFF);
  RGB covB = (CFG.paletteCount >= 4) ? from24(CFG.colorD) : rgb(0x99, 0x33, 0xCC);
  
  float drift = fmodf((millis() * 0.00008f), 1.0f);
  RGB unsc0 = lerp(unscA, unscB, 0.5f + 0.5f * sinf(drift * 6.2831853f));
  RGB cov0  = lerp(covA,  covB,  0.5f + 0.5f * sinf((1.0f - drift) * 6.2831853f));
  
  float sp01 = CFG.speed / 255.0f;
  float period = 3.0f - 2.5f * sp01; if (period < 0.5f) period = 0.5f;
  float ph = fmodf((millis() / 1000.0f) / period, 1.0f);
  float swell = 0.7f + 0.3f * powf(0.5f + 0.5f * sinf(6.2831853f * ph), 2.0f);
  
  uint8_t trailFade = (uint8_t)(210 - (CFG.intensity > 200 ? 200 : CFG.intensity));
  if (trailFade < 10) trailFade = 10;
  fade(trailFade);
  
  uint16_t bands = L / block; if (bands < 2) bands = 2;
  
  for (uint16_t b = 0; b < bands; ++b) {
    bool unscBand = ((b & 1) == 0) ^ flip;
    RGB color = unscBand ? unsc0 : cov0;
    
    float edgeSoft = (CFG.intensity / 255.0f);
    if (edgeSoft < 0.02f) edgeSoft = 0.02f;
    
    for (uint16_t w = 0; w < block; ++w) {
      uint16_t idx = unscBand ? ((posCW + b * block + w) % L) 
                              : ((posCCW + b * block + w) % L);
      
      float edgePos = fabsf((w - (block-1)/2.0f)) / (block/2.0f);
      float soft = 1.0f - edgeSoft * powf(edgePos, 1.5f);
      if (soft < 0.f) soft = 0.f;
      
      float amp = soft * swell;
      RGB px = rgb(
        uint8_t(color.r * amp),
        uint8_t(color.g * amp),
        uint8_t(color.b * amp)
      );
      
      g_strip.setPixelColor(idx, px.r, px.g, px.b);
    }
  }
  
  if ((esp_random() & 31) < 3) {
    uint16_t sparkPos = esp_random() % L;
    float sparkIntensity = 0.4f + 0.4f * ((esp_random() & 255) / 255.0f);
    RGB sparkColor = ((esp_random() & 1) ? unsc0 : cov0);
    RGB spark = rgb(
      uint8_t(sparkColor.r * sparkIntensity * swell),
      uint8_t(sparkColor.g * sparkIntensity * swell),
      uint8_t(sparkColor.b * sparkIntensity * swell)
    );
    g_strip.setPixelColor(sparkPos, spark.r, spark.g, spark.b);
  }
}

static void render(){
  if (!g_enabled || CFG.masterOff){
    fill(rgb(0,0,0));
    return;
  }
  switch (CFG.mode){
    default:
    case 0:  animSolid();         break;
    case 1:  animBreathe();       break;
    case 2:  animColorWipe();     break;
    case 3:  animLarson();        break;
    case 4:  animRainbow();       break;
    case 5:  animTheater();       break;
    case 6:  animTwinkle();       break;
    case 7:  animComet();         break;
    case 8:  animMeteor();        break;
    case 9:  animClockSpin();     break;
    case 10: animPlasma();        break;
    case 11: animFire();          break;
    case 12: animPaletteCycle();  break;
    case 13: animPaletteChase();  break;
    case 14: animCustom();        break;
    case 15: animUNSCCovenant();  break;
  }
}

void ROL::setEnabled(bool on){ g_enabled = on; }

void ROL::begin(uint8_t pin, uint16_t count){
  if (g_inited) return;

  g_strip.updateLength(count);
  g_strip.setPin(pin);
  g_strip.begin();
  g_strip.clear();
  g_strip.setBrightness(0);
  g_strip.show();

  g_bootFadeTarget        = CFG.brightness;
  g_bootFadeStartMs       = millis();
  g_bootFadeActive        = true;
  g_lastAppliedBrightness = 0;

  g_inited = true;
}

void ROL::loop(){
  if (!g_inited) return;

  pullCfgIfNeeded();

  uint32_t now = millis();

  static uint32_t msPrev = 0;
  uint8_t frameMs = 10 + (uint8_t)((255 - CFG.speed) / 2);
  bool doFrame = false;
  if (now - msPrev >= frameMs) {
    msPrev = now;
    ++g_tickMs;
    doFrame = true;
  }

  uint8_t target = (!g_enabled || CFG.masterOff) ? 0 : CFG.brightness;

  if (g_bootFadeActive) {
    g_bootFadeTarget = target;
    uint32_t elapsed = now - g_bootFadeStartMs;
    uint8_t cur = (elapsed >= g_bootFadeDurationMs)
                    ? g_bootFadeTarget
                    : (uint8_t)((uint32_t)g_bootFadeTarget * elapsed / g_bootFadeDurationMs);

    if (g_bootFadeTarget && cur == 0) cur = 1;

    if (cur != g_lastAppliedBrightness) {
      g_strip.setBrightness(cur);
      g_lastAppliedBrightness = cur;
    }
    if (elapsed >= g_bootFadeDurationMs) {
      g_bootFadeActive = false;
    }
  } else {
    if (g_lastAppliedBrightness != target) {
      g_strip.setBrightness(target);
      g_lastAppliedBrightness = target;
    }
  }

  if (doFrame) {
    render();
    g_strip.show();
  }
}

void ROL::kratosFeed(const uint8_t* rgb, uint16_t count) {
  if (!g_inited) return;
  
  if (!rgb || count == 0) {
    g_strip.clear();
    g_strip.show();
    return;
  }
  
  uint16_t L = g_strip.numPixels();
  uint16_t n = (count < L) ? count : L;
  
  for (uint16_t i = 0; i < n; i++) {
    g_strip.setPixelColor(i, rgb[i*3+0], rgb[i*3+1], rgb[i*3+2]);
  }
  
  for (uint16_t i = n; i < L; i++) {
    g_strip.setPixelColor(i, 0, 0, 0);
  }
  
  g_strip.show();
}
