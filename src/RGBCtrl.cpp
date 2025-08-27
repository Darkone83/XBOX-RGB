#include "RGBCtrl.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <esp_system.h>  // esp_random

// Use the existing WiFiMgr server; no separate server objects needed.
namespace WiFiMgr { AsyncWebServer& getServer(); }

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
  uint32_t colorA        = 0xFF0000; // #RRGGBB
  uint32_t colorB        = 0xFFA000; // used by some modes
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

// -------------------- Helpers --------------------
inline RgbColor rgbFrom24(uint32_t rgb) {
  return RgbColor((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF);
}
static inline RgbColor unpackGRB(uint32_t c) {
  uint8_t g = (c >> 16) & 0xFF;
  uint8_t r = (c >>  8) & 0xFF;
  uint8_t b = (c      ) & 0xFF;
  return RgbColor(r,g,b);
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
      uint16_t px = (REVERSE[segs[s].ch] && segs[s].count)
                    ? (segs[s].count - 1 - within)
                    : within;
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
  for (uint8_t s=0; s<NUM_CH; ++s) {
    for (uint16_t i=0; i<segs[s].count; ++i) {
      RgbColor c = unpackGRB(STRIPS[segs[s].ch]->getPixelColor(i));
      uint8_t r = (uint16_t)c.R * (255-amt) >> 8;
      uint8_t g = (uint16_t)c.G * (255-amt) >> 8;
      uint8_t b = (uint16_t)c.B * (255-amt) >> 8;
      STRIPS[segs[s].ch]->setPixelColor(i, r,g,b);
    }
  }
}
static void showRing() {
  if (lastAppliedBrightness != CFG.brightness) {
    for (uint8_t s=0; s<NUM_CH; ++s) STRIPS[s]->setBrightness(CFG.brightness);
    lastAppliedBrightness = CFG.brightness;
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

// -------------------- Animations (synced around the ring) --------------------
static void animSolid() { fillRing(rgbFrom24(CFG.colorA)); }

static void animBreathe() {
  float t = (tick * (CFG.speed/255.0f)) * 0.05f;
  float s = (sinf(t) * 0.5f + 0.5f);
  uint8_t lvl = (uint8_t)(30 + s * 225);
  RgbColor base = rgbFrom24(CFG.colorA);
  RgbColor cur(uint8_t((base.R*lvl)/255), uint8_t((base.G*lvl)/255), uint8_t((base.B*lvl)/255));
  fillRing(cur);
}
static void animColorWipe(bool forward=true) {
  uint16_t L = ringLen(); if (!L) return;
  RgbColor off(0,0,0);
  fillRing(off);
  uint16_t idx = (tick/2) % L;
  setRing(forward ? idx : (L-1-idx), rgbFrom24(CFG.colorA));
}
static void animLarson() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 6 - (CFG.speed/51); if (denom < 1) denom = 1;
  uint16_t pos = (tick / (uint16_t)denom) % (L*2);
  if (pos >= L) pos = 2*L - 1 - pos;
  int fadeBase = 10 + CFG.intensity; if (fadeBase > 254) fadeBase = 254;
  uint8_t fadeAmt = (uint8_t)(255 - fadeBase);
  fadeRing(fadeAmt);
  for (int w=-(int)CFG.width; w<=(int)CFG.width; ++w) {
    int p = (int)pos + w;
    if (p>=0 && p<(int)L) setRing((uint16_t)p, rgbFrom24(CFG.colorA));
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
  RgbColor c = rgbFrom24(CFG.colorA);
  for (uint16_t i=q; i<L; i+=gap) setRing(i, c);
}
static void animTwinkle() {
  int f = 5 + (CFG.speed/3); if (f > 254) f = 254;
  uint8_t fadeAmt = (uint8_t)(255 - f);
  fadeRing(fadeAmt);
  uint16_t L = ringLen(); if (!L) return;
  uint16_t pops = 1 + (uint16_t)((CFG.intensity/255.0f) * (L/3 + 1));
  for (uint16_t i=0; i<pops; ++i) setRing(esp_random()%L, rgbFrom24(CFG.colorA));
}
static void animComet() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 4 - (CFG.speed/64); if (denom < 1) denom = 1;
  uint16_t pos = (tick / (uint16_t)denom) % L;
  uint8_t fadeAmt = (uint8_t)(200 - (CFG.intensity > 199 ? 199 : CFG.intensity));
  fadeRing(fadeAmt);
  RgbColor c = rgbFrom24(CFG.colorA);
  for (uint8_t w=0; w<CFG.width; ++w) setRing((pos + L - w) % L, c);
}
static void animMeteor() {
  uint16_t L = ringLen(); if (!L) return;
  int denom = 4 - (CFG.speed/64); if (denom < 1) denom = 1;
  uint16_t pos = (tick / (uint16_t)denom) % L;
  uint8_t fadeAmt = (uint8_t)(210 - (CFG.intensity > 209 ? 209 : CFG.intensity));
  fadeRing(fadeAmt);
  RgbColor a = rgbFrom24(CFG.colorA);
  RgbColor b = rgbFrom24(CFG.colorB);
  uint8_t wmax = (CFG.width < 1) ? 1 : CFG.width;
  for (uint8_t w=0; w<wmax; ++w) {
    float t = w / float(wmax);
    RgbColor mix(uint8_t(a.R*(1-t)+b.R*t), uint8_t(a.G*(1-t)+b.G*t), uint8_t(a.B*(1-t)+b.B*t));
    setRing((pos + w) % L, mix);
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
static void animPlasma() {
  uint16_t L = ringLen(); if (!L) return;
  float t  = tick * (0.02f + (CFG.speed/255.0f)*0.06f);
  for (uint16_t i=0; i<L; ++i) {
    float a = (float)i / (float)L * 6.2831853f; // 0..2π
    float v = 0.5f + 0.5f * (sinf(3*a + t) * 0.5f + sinf(5*a - t*0.8f) * 0.5f);
    float sat = 0.4f + (CFG.intensity/255.0f)*0.6f;
    float con = 0.7f + (CFG.width/20.0f)*0.6f;
    float hue = fmodf((v*con + t*0.03f), 1.0f);
    setRing(i, hsv2rgb(hue, sat, 1.0f));
  }
}
static void animFire() {
  uint16_t L = ringLen(); if (!L) return;
  uint8_t cool = 55 - (uint8_t)(CFG.intensity/255.0f * 40); // 15..55
  for (uint16_t i=0;i<L;++i) {
    uint8_t dec = esp_random() % (cool+1);
    heat[i] = (heat[i] > dec) ? (heat[i]-dec) : 0;
  }
  for (uint16_t i=0;i<L;++i) {
    uint16_t i1 = (i + L - 1) % L;
    uint16_t i2 = (i + 1) % L;
    heat[i] = (uint8_t)((heat[i] + heat[i1] + heat[i2]) / 3);
  }
  uint8_t sparks = 1 + (CFG.speed / 64); // 1..5
  for (uint8_t s=0; s<sparks; ++s) {
    uint16_t p = esp_random() % L;
    uint8_t  add = 160 + (esp_random() % 96);
    uint16_t v = heat[p] + add;
    heat[p] = (v > 255) ? 255 : (uint8_t)v;
  }
  for (uint16_t i=0;i<L;++i) {
    uint8_t t = heat[i];
    RgbColor c;
    if (t < 85)       c = RgbColor(t*3, 0, 0);
    else if (t < 170) c = RgbColor(255, (t-85)*3, 0);
    else              c = RgbColor(255, 255, (t-170)*3);
    setRing(i, c);
  }
}

// -------------------- Frame selection --------------------
static void renderFrame() {
  switch (CFG.mode) {
    case MODE_SOLID:        animSolid();      break;
    case MODE_BREATHE:      animBreathe();    break;
    case MODE_COLOR_WIPE:   animColorWipe();  break;
    case MODE_LARSON:       animLarson();     break;
    case MODE_RAINBOW:      animRainbow();    break;
    case MODE_THEATER:      animTheater();    break;
    case MODE_TWINKLE:      animTwinkle();    break;
    case MODE_COMET:        animComet();      break;
    case MODE_METEOR:       animMeteor();     break;
    case MODE_CLOCK_SPIN:   animClockSpin();  break;
    case MODE_PLASMA:       animPlasma();     break;
    case MODE_FIRE:         animFire();       break;
    default: break;
  }
  showRing();
}

// -------------------- Persistence --------------------
static const char* NVS_NS = "rgbctrl";
static const char* NVS_KEY= "config";

static void defaults() { CFG = AppConfig(); }

static String configToJson() {
  StaticJsonDocument<896> doc;
  JsonArray counts = doc.createNestedArray("count");
  for (uint8_t i=0;i<NUM_CH;++i) counts.add(CFG.count[i]);
  doc["brightness"]   = CFG.brightness;
  doc["mode"]         = CFG.mode;
  doc["speed"]        = CFG.speed;
  doc["intensity"]    = CFG.intensity;
  doc["width"]        = CFG.width;
  doc["colorA"]       = CFG.colorA;
  doc["colorB"]       = CFG.colorB;
  doc["resumeOnBoot"] = CFG.resumeOnBoot;
  doc["enableCpu"]    = CFG.enableCpu;
  doc["enableFan"]    = CFG.enableFan;
  doc["inPreview"]    = inPreview;
  String js; serializeJson(doc, js); return js;
}
static bool parseConfig(const String& body, AppConfig& out) {
  StaticJsonDocument<896> doc;
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

  StaticJsonDocument<896> doc;
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
    if (doc.containsKey("resumeOnBoot"))tmp.resumeOnBoot= doc["resumeOnBoot"].as<bool>();
    if (doc.containsKey("enableCpu"))   tmp.enableCpu   = doc["enableCpu"].as<bool>();
    if (doc.containsKey("enableFan"))   tmp.enableFan   = doc["enableFan"].as<bool>();
    CFG = tmp;
  }
}
static void saveConfig() {
  StaticJsonDocument<896> doc;
  JsonArray counts = doc.createNestedArray("count");
  for (uint8_t i=0;i<NUM_CH;++i) counts.add(CFG.count[i]);
  doc["brightness"]   = CFG.brightness;
  doc["mode"]         = CFG.mode;
  doc["speed"]        = CFG.speed;
  doc["intensity"]    = CFG.intensity;
  doc["width"]        = CFG.width;
  doc["colorA"]       = CFG.colorA;
  doc["colorB"]       = CFG.colorB;
  doc["resumeOnBoot"] = CFG.resumeOnBoot;
  doc["enableCpu"]    = CFG.enableCpu;
  doc["enableFan"]    = CFG.enableFan;

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
.toggle{display:flex;align-items:center;gap:8px}
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
      </select>
    </div>
    <div class="md-4"><label>Brightness</label><input id="brightness" type="range" min="1" max="255"></div>
    <div class="md-4"><label>Speed</label><input id="speed" type="range" min="0" max="255"></div>

    <div class="md-3 opt opt-intensity"><label>Intensity</label><input id="intensity" type="range" min="0" max="255"></div>
    <div class="md-3 opt opt-width"><label>Width / Gap</label><input id="width" type="range" min="1" max="20"></div>
    <div class="md-3 opt opt-colorA"><label>Primary Color</label><input id="colorA" type="color"></div>
    <div class="md-3 opt opt-colorB"><label>Secondary Color</label><input id="colorB" type="color"></div>

    <div class="md-3"><label>CH1 (Front) Count</label><input id="c0" type="number" min="0" max="50"></div>
    <div class="md-3"><label>CH2 (Left) Count</label><input id="c1" type="number" min="0" max="50"></div>
    <div class="md-3"><label>CH3 (Rear) Count</label><input id="c2" type="number" min="0" max="50"></div>
    <div class="md-3"><label>CH4 (Right) Count</label><input id="c3" type="number" min="0" max="50"></div>

    <div class="md-6"><label>Resume last mode on boot</label>
      <select id="resume"><option value="true">Yes</option><option value="false">No</option></select>
    </div>

    <div class="md-6"><label>Xbox SMBus LEDs</label>
      <div class="toggle"><input id="smbusCpu" type="checkbox"> <span>Enable CPU temp LEDs (CH5)</span></div>
      <div class="toggle"><input id="smbusFan" type="checkbox"> <span>Enable Fan speed LEDs (CH6)</span></div>
      <span class="hint">Disable to avoid SMBus polling by the other module.</span>
    </div>

    <div class="md-6"><button class="primary" id="save">Save</button></div>
    <div class="md-6"><button id="revert">Reload</button></div>
    <div class="md-6"><button id="reset">Reset Defaults</button></div>
    <div class="md-12"><span class="hint">All changes preview live. Click Save to persist to flash.</span></div>
  </div>
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
    colorA:   [0,1,2,3,4,5,6,7,8,9,10,11],
    colorB:   [8,9,10],
    width:    [3,5,7,8,9],
    intensity:[3,5,6,7,8,11],
  };
  const on = k => (vis[k]||[]).includes(mode);
  document.querySelectorAll('.opt-colorA').forEach(n=>n.classList.toggle('hide',!on('colorA')));
  document.querySelectorAll('.opt-colorB').forEach(n=>n.classList.toggle('hide',!on('colorB')));
  document.querySelectorAll('.opt-width').forEach(n=>n.classList.toggle('hide',!on('width')));
  document.querySelectorAll('.opt-intensity').forEach(n=>n.classList.toggle('hide',!on('intensity')));
}

function fillForm(s){
  el('mode').value      = s.mode;
  el('brightness').value= s.brightness;
  el('speed').value     = s.speed;
  el('intensity').value = s.intensity;
  el('width').value     = s.width;
  el('colorA').value    = hex24(s.colorA);
  el('colorB').value    = hex24(s.colorB);
  for(let i=0;i<4;i++) el('c'+i).value = s.count[i];
  el('resume').value    = s.resumeOnBoot ? 'true' : 'false';
  el('smbusCpu').checked= !!s.enableCpu;
  el('smbusFan').checked= !!s.enableFan;
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

  // 2) Also fetch live from API (keeps page correct even if config changed elsewhere)
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

['mode','brightness','speed','intensity','width','colorA','colorB','resume','smbusCpu','smbusFan']
.forEach(id=>{
  const n = el(id); const ev = (n.type==='checkbox') ? 'change' : 'input';
  n.addEventListener(ev, preview);
});
for(let i=0;i<4;i++){ el('c'+i).addEventListener('change', preview); }
el('save').addEventListener('click',save);
el('revert').addEventListener('click',load);
el('reset').addEventListener('click',resetDefaults);
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
    request->send(200, "text/html", html);
  });
  DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");

  // GET config (also reload from NVS so page sees LAST SAVED prefs)
  server.on(String(gBase + "/api/ledconfig").c_str(), HTTP_GET, [](AsyncWebServerRequest *request){
    loadConfig();        // pull from flash
    applyConfig();       // apply to strips (counts/brightness)
    request->send(200, "application/json", configToJson());
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
        if (!parseConfig(*body, tmp)) { req->send(400, "text/plain", "Bad JSON"); }
        else {
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
        if (!parseConfig(*body, tmp)) { req->send(400, "text/plain", "Bad JSON"); }
        else {
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

} // namespace RGBCtrl
