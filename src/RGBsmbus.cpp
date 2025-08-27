#include "RGBsmbus.h"
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>

// ========================= USER CONFIG =========================
// LED pixel format for CH5/CH6 bars:
#ifndef RGBSMBUS_PIXEL_TYPE
#define RGBSMBUS_PIXEL_TYPE (NEO_GRB + NEO_KHZ800)
#endif

// Global brightness for CH5/CH6 bars (0..255)
static const uint8_t  BRIGHTNESS       = 160;

// How often to poll the Xbox SMBus (ms)  —— per request: 10 seconds
static const uint32_t POLL_INTERVAL_MS = 10000;

// Smoothing factor for readings (0..1). Lower = slower response.
static const float    SMOOTH_ALPHA     = 0.35f;

// CPU °C thresholds
static const float CPU_COOL_MAX_C = 45.0f;
static const float CPU_WARM_MAX_C = 65.0f;
static const float CPU_MAX_C      = 75.0f;

// Fan % thresholds
static const float FAN_SLOW_MAX   = 33.0f;
static const float FAN_MED_MAX    = 66.0f;
static const float FAN_FAST_MAX   = 100.0f;

// Colors (0xRRGGBB)
static const uint32_t CPU_COOL_COLOR = 0x00FF00; // green
static const uint32_t CPU_WARM_COLOR = 0xFFFF00; // yellow
static const uint32_t CPU_HOT_COLOR  = 0xFF0000; // red

static const uint32_t FAN_SLOW_COLOR = 0x0066FF; // blue
static const uint32_t FAN_MED_COLOR  = 0xFFFF00; // yellow
static const uint32_t FAN_FAST_COLOR = 0xFF7A00; // orange

static const uint32_t FAIL_COLOR     = 0x400000; // dim red on error
// ===============================================================


// ======== Xbox SMC SMBus (I²C) details =========
// NOTE: Wire uses 7-bit addresses. These match the Xbox SMC/encoders.
static const uint8_t SMC_ADDRESS   = 0x10; // SMC PIC (7-bit)
static const uint8_t REG_CPUTEMP   = 0x09; // °C
static const uint8_t REG_FANSPEED  = 0x10; // raw 0..50 -> ×2 => percent

// Video encoders (7-bit):
//  - Conexant ~0x45 (8-bit 0x8A/0x8B)
//  - Focus    ~0x6A (8-bit 0xD4/0xD5)
//  - Xcalibur ~0x70 (8-bit 0xE0/0xE1)
static const uint8_t I2C_XCALIBUR  = 0x70;
// ==============================================


namespace RGBsmbus {

// Defaults: SDA/SCL 0/0 here are placeholders; call begin(...) with real pins.
// ch5/ch6 default to 7/6 for compatibility with your earlier mapping.
static RGBsmbusPins PINS{0,0,7,6};

static uint8_t CH5_COUNT = 10;
static uint8_t CH6_COUNT = 10;

// CH5 = CPU bar, CH6 = FAN bar
static Adafruit_NeoPixel cpuStrip(10, 1, RGBSMBUS_PIXEL_TYPE);
static Adafruit_NeoPixel fanStrip(10, 2, RGBSMBUS_PIXEL_TYPE);

static uint32_t lastPoll = 0;
static float smoothedCpu = 0.0f;
static float smoothedFan = 0.0f;

static bool gEnableCPU   = true;
static bool gEnableFAN   = true;

static bool gIsXcalibur  = false;   // detected at runtime

// Track last brightness applied (to mirror RGBCtrl behavior)
static uint8_t lastAppliedBrightnessCpu = 0xFF;
static uint8_t lastAppliedBrightnessFan = 0xFF;


// ---------- helpers ----------
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
  t = clampf(t, 0.f, 1.f);
  uint8_t ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
  uint8_t br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
  uint8_t r=uint8_t(ar + (br-ar)*t);
  uint8_t g=uint8_t(ag + (bg-ag)*t);
  uint8_t b8=uint8_t(ab + (bb-ab)*t);
  return (uint32_t(r)<<16)|(uint32_t(g)<<8)|b8;
}
static uint32_t colorForCpu(float c) {
  c = clampf(c, 0.f, CPU_MAX_C);
  if (c <= CPU_COOL_MAX_C) {
    float t = (CPU_COOL_MAX_C>0)?(c/CPU_COOL_MAX_C):0.f;
    return lerpColor(CPU_COOL_COLOR, CPU_WARM_COLOR, t);
  } else if (c <= CPU_WARM_MAX_C) {
    float span = CPU_WARM_MAX_C - CPU_COOL_MAX_C;
    float t = span>0 ? (c-CPU_COOL_MAX_C)/span : 1.f;
    return lerpColor(CPU_WARM_COLOR, CPU_HOT_COLOR, t);
  }
  return CPU_HOT_COLOR;
}
static uint32_t colorForFan(float p) {
  p = clampf(p, 0.f, FAN_FAST_MAX);
  if (p <= FAN_SLOW_MAX) {
    float t = (FAN_SLOW_MAX>0)?(p/FAN_SLOW_MAX):0.f;
    return lerpColor(FAN_SLOW_COLOR, FAN_MED_COLOR, t);
  } else if (p <= FAN_MED_MAX) {
    float span = FAN_MED_MAX - FAN_SLOW_MAX;
    float t = span>0 ? (p-FAN_SLOW_MAX)/span : 1.f;
    return lerpColor(FAN_MED_COLOR, FAN_FAST_COLOR, t);
  }
  return FAN_FAST_COLOR;
}
static uint8_t barLen(float val, float maxVal, uint8_t n) {
  if (maxVal <= 0.f) return 0;
  float f = clampf(val/maxVal, 0.f, 1.f);
  int lit = int(f*n + 0.5f);
  if (lit < 0) lit = 0; if (lit > (int)n) lit = n;
  return (uint8_t)lit;
}
static void drawBar(Adafruit_NeoPixel& strip,
                    uint8_t& lastBriRef,   // reference to last brightness cache
                    uint8_t nleds,
                    uint8_t lit,
                    uint32_t rgb24) {
  const uint8_t r=(rgb24>>16)&0xFF, g=(rgb24>>8)&0xFF, b=rgb24&0xFF;

  // Update pixels
  for (uint8_t i=0;i<nleds;++i)
    strip.setPixelColor(i, (i<lit)?r:0, (i<lit)?g:0, (i<lit)?b:0);

  // Apply brightness if changed (like RGBCtrl)
  if (lastBriRef != BRIGHTNESS) {
    strip.setBrightness(BRIGHTNESS);
    lastBriRef = BRIGHTNESS;
  }

  // One show() per bar update (consistent with RGBCtrl’s cadence)
  strip.show();
}


// ---------- SMBus ----------
static int readSMBusByte(uint8_t addr7, uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(addr7);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  Wire.requestFrom(addr7, (uint8_t)1);
  if (Wire.available()) { value = Wire.read(); return 0; }
  return -1;
}

static bool pollSelective(bool wantCPU, bool wantFAN, int& outCpu, int& outFan) {
  bool ok = true;
  outCpu = -1; outFan = -1;
  uint8_t v;

  if (wantCPU) {
    if (readSMBusByte(SMC_ADDRESS, REG_CPUTEMP, v)==0 && v < 120) outCpu = (int)v;
    else ok = false;
  }
  if (wantFAN) {
    if (readSMBusByte(SMC_ADDRESS, REG_FANSPEED, v)==0 && v <= 50) outFan = int(v)*2; // 0..100%
    else ok = false;
  }
  return ok;
}

// Lightweight presence probe (7-bit I2C address)
static bool probeI2C(uint8_t addr7) {
  Wire.beginTransmission(addr7);
  uint8_t err = Wire.endTransmission();
  return (err == 0);
}

static void detectBoard() {
  // Detect Xcalibur encoder; safe no-op if absent.
  gIsXcalibur = probeI2C(I2C_XCALIBUR);
  // (If you need different scaling/regs for 1.6 in the future,
  //  use gIsXcalibur to branch. Current SMC regs work as-is.)
}


// ---------- public ----------
void begin(const RGBsmbusPins& pins, uint8_t ch5Count, uint8_t ch6Count) {
  PINS = pins;
  CH5_COUNT = ch5Count>10?10:ch5Count;
  CH6_COUNT = ch6Count>10?10:ch6Count;

  // Bind LED channels to the right GPIOs and init (RGBCtrl style)
  cpuStrip.updateLength(CH5_COUNT); cpuStrip.setPin(PINS.ch5);
  fanStrip.updateLength(CH6_COUNT); fanStrip.setPin(PINS.ch6);

  cpuStrip.begin(); cpuStrip.clear(); cpuStrip.setBrightness(BRIGHTNESS); cpuStrip.show();
  fanStrip.begin(); fanStrip.clear(); fanStrip.setBrightness(BRIGHTNESS); fanStrip.show();

  lastAppliedBrightnessCpu = BRIGHTNESS;
  lastAppliedBrightnessFan = BRIGHTNESS;

  // Bring up I2C and auto-detect Xcalibur
  Wire.begin(PINS.sda, PINS.scl);
  detectBoard();

  lastPoll = 0;
  smoothedCpu = 0.f; smoothedFan = 0.f;
}

static void updateOnce() {
  // If both disabled, skip the bus entirely
  if (!gEnableCPU && !gEnableFAN) return;

  int rawCpu=-1, rawFan=-1;
  bool ok = pollSelective(gEnableCPU, gEnableFAN, rawCpu, rawFan);

  if (ok) {
    if (gEnableCPU) {
      if (rawCpu >= 0) {
        smoothedCpu = SMOOTH_ALPHA * float(rawCpu) + (1.f-SMOOTH_ALPHA)*smoothedCpu;
        drawBar(cpuStrip, lastAppliedBrightnessCpu,
                CH5_COUNT,
                barLen(smoothedCpu, CPU_MAX_C, CH5_COUNT),
                colorForCpu(smoothedCpu));
      } else {
        // reading disabled or invalid—turn off if not enabled
        drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
      }
    } else {
      drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
    }

    if (gEnableFAN) {
      if (rawFan >= 0) {
        smoothedFan = SMOOTH_ALPHA * float(rawFan) + (1.f-SMOOTH_ALPHA)*smoothedFan;
        drawBar(fanStrip, lastAppliedBrightnessFan,
                CH6_COUNT,
                barLen(smoothedFan, FAN_FAST_MAX, CH6_COUNT),
                colorForFan(smoothedFan));
      } else {
        drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
      }
    } else {
      drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
    }
  } else {
    // Error indicator on first pixel(s) of enabled bars
    if (gEnableCPU && CH5_COUNT) {
      cpuStrip.setPixelColor(0,(FAIL_COLOR>>16)&0xFF,(FAIL_COLOR>>8)&0xFF,FAIL_COLOR&0xFF);
      if (lastAppliedBrightnessCpu != BRIGHTNESS) { cpuStrip.setBrightness(BRIGHTNESS); lastAppliedBrightnessCpu = BRIGHTNESS; }
      cpuStrip.show();
    }
    if (gEnableFAN && CH6_COUNT) {
      fanStrip.setPixelColor(0,(FAIL_COLOR>>16)&0xFF,(FAIL_COLOR>>8)&0xFF,FAIL_COLOR&0xFF);
      if (lastAppliedBrightnessFan != BRIGHTNESS) { fanStrip.setBrightness(BRIGHTNESS); lastAppliedBrightnessFan = BRIGHTNESS; }
      fanStrip.show();
    }
  }
}

void loop() {
  uint32_t now = millis();
  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    updateOnce();
  }
}

void refreshNow() { updateOnce(); }

void setCpuEnabled(bool en) { gEnableCPU = en; }
void setFanEnabled(bool en) { gEnableFAN = en; }
bool cpuEnabled() { return gEnableCPU; }
bool fanEnabled() { return gEnableFAN; }

bool isXcalibur() { return gIsXcalibur; }

// ----- tiny REST API so the /config page (RGBCtrl) can toggle flags -----
void attachWeb(AsyncWebServer& server, const char* basePath) {
  String base = (basePath && *basePath) ? basePath : "/config/smbus";
  String api  = base + "/api/flags";

  // GET flags (+ board detection info)
  server.on(api.c_str(), HTTP_GET, [](AsyncWebServerRequest* r){
    auto* s = r->beginResponseStream("application/json");
    s->printf("{\"cpu\":%s,\"fan\":%s,\"xcalibur\":%s}",
              gEnableCPU?"true":"false", gEnableFAN?"true":"false", gIsXcalibur?"true":"false");
    r->send(s);
  });

  // POST flags {"cpu":true/false,"fan":true/false}
  server.on(api.c_str(), HTTP_POST, [](AsyncWebServerRequest* r){
    r->send(405, "text/plain", "POST with body only");
  },
  nullptr,
  [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
    bool ok = true;
    String body((const char*)data, len);
    body.replace(" ", ""); body.replace("\n", ""); body.replace("\r", "");
    int icpu = body.indexOf("\"cpu\":");
    int ifan = body.indexOf("\"fan\":");
    if (icpu >= 0) {
      int v = body.indexOf("true", icpu);  int f = body.indexOf("false", icpu);
      if      (v>=0 && (f<0 || v<f)) gEnableCPU = true;
      else if (f>=0 && (v<0 || f<v)) gEnableCPU = false;
      else ok = false;
    }
    if (ifan >= 0) {
      int v = body.indexOf("true", ifan);  int f = body.indexOf("false", ifan);
      if      (v>=0 && (f<0 || v<f)) gEnableFAN = true;
      else if (f>=0 && (v<0 || f<v)) gEnableFAN = false;
      else ok = false;
    }
    auto* s = req->beginResponseStream("application/json");
    s->printf("{\"ok\":%s,\"cpu\":%s,\"fan\":%s,\"xcalibur\":%s}",
              ok?"true":"false",
              gEnableCPU?"true":"false",
              gEnableFAN?"true":"false",
              gIsXcalibur?"true":"false");
    req->send(s);
  });
}

} // namespace RGBsmbus
