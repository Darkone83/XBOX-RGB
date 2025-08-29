#include "RGBsmbus.h"
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>

// Read the toggles saved by RGBCtrl's Web UI (no extra REST needed)
namespace RGBCtrl {
  bool smbusCpuEnabled();
  bool smbusFanEnabled();
}

// ========================= USER CONFIG =========================
#ifndef RGBSMBUS_PIXEL_TYPE
#define RGBSMBUS_PIXEL_TYPE (NEO_GRB + NEO_KHZ800)
#endif

static const uint8_t  BRIGHTNESS       = 160;    // CH5/CH6 bar brightness
static const uint32_t POLL_INTERVAL_MS = 10000;  // poll every 10s
static const float    SMOOTH_ALPHA     = 0.35f;  // EMA smoothing

// CPU °C thresholds
static const float CPU_COOL_MAX_C = 25.0f;
static const float CPU_WARM_MAX_C = 45.0f;
static const float CPU_MAX_C      = 65.0f;

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
static const uint8_t SMC_ADDRESS   = 0x10; // SMC PIC (7-bit)
static const uint8_t REG_CPUTEMP   = 0x09; // °C
static const uint8_t REG_FANSPEED  = 0x10; // 0..50 -> ×2 => %  (some FW returns 0..100)

// Video encoders (7-bit) — probe to identify board family
static const uint8_t I2C_XCALIBUR  = 0x70;
// ===============================================

namespace RGBsmbus {

static RGBsmbusPins PINS{0,0,7,6};

static uint8_t CH5_COUNT = 5;
static uint8_t CH6_COUNT = 5;

// CH5 = CPU bar, CH6 = FAN bar
static Adafruit_NeoPixel cpuStrip(5, 1, RGBSMBUS_PIXEL_TYPE);
static Adafruit_NeoPixel fanStrip(5, 2, RGBSMBUS_PIXEL_TYPE);

static uint32_t lastPoll = 0;
static float smoothedCpu = 0.0f;
static float smoothedFan = 0.0f;

// We still keep local copies, but we now *mirror* RGBCtrl flags each poll
static bool gEnableCPU   = true;
static bool gEnableFAN   = true;

static bool gIsXcalibur  = false;

// track last applied brightness
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
                    uint8_t& lastBriRef,
                    uint8_t nleds,
                    uint8_t lit,
                    uint32_t rgb24) {
  const uint8_t r=(rgb24>>16)&0xFF, g=(rgb24>>8)&0xFF, b=rgb24&0xFF;

  for (uint8_t i=0;i<nleds;++i)
    strip.setPixelColor(i, (i<lit)?r:0, (i<lit)?g:0, (i<lit)?b:0);

  if (lastBriRef != BRIGHTNESS) {
    strip.setBrightness(BRIGHTNESS);
    lastBriRef = BRIGHTNESS;
  }
  strip.show();
}

// ---------- SMBus (robust reads) ----------
static bool readTryRepeatedStart(uint8_t addr7, uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(addr7);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false; // no STOP
  if (Wire.requestFrom((int)addr7, 1, (int)true) != 1) return false;
  value = Wire.read();
  return true;
}
static bool readTryStopThenRead(uint8_t addr7, uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(addr7);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;  // with STOP
  delayMicroseconds(150);
  if (Wire.requestFrom((int)addr7, 1, (int)true) != 1) return false;
  value = Wire.read();
  return true;
}
static bool readSMBusByteRobust(uint8_t addr7, uint8_t reg, uint8_t& value) {
  for (uint8_t attempt=0; attempt<3; ++attempt) {
    if (readTryRepeatedStart(addr7, reg, value)) return true;
    if (readTryStopThenRead(addr7, reg, value))  return true;
    delay(2);
  }
  return false;
}
static bool readMedianByte(uint8_t addr7, uint8_t reg, uint8_t& out) {
  uint8_t got = 0, a=0, b=0, c=0;
  for (uint8_t i=0; i<3; ++i) {
    uint8_t v=0;
    if (readSMBusByteRobust(addr7, reg, v)) {
      if (got==0) a=v; else if (got==1) b=v; else c=v;
      ++got;
      delayMicroseconds(200);
    }
  }
  if (got < 2) return false;
  if (got == 2) { out = (uint8_t)((a + b) / 2); return true; }
  uint8_t lo = a<b ? a : b;  uint8_t hi = a>b ? a : b;
  out = (c<lo) ? lo : (c>hi ? hi : c);
  return true;
}
static bool readCpuCelsius(uint8_t& outC) {
  uint8_t v=0;
  if (!readMedianByte(SMC_ADDRESS, REG_CPUTEMP, v)) return false;
  if (v > 100) return false;     // plausibility guard (0..100 °C)
  outC = v;
  return true;
}
static bool readFanPercent(uint8_t& outPct) {
  uint8_t v=0;
  if (!readMedianByte(SMC_ADDRESS, REG_FANSPEED, v)) return false;
  uint16_t pct = (v <= 50) ? (uint16_t)v * 2u : (uint16_t)v; // 0..50 → %
  if (pct > 100) pct = 100;
  outPct = (uint8_t)pct;
  return true;
}

// Probe for encoder (optional)
static bool probeI2C(uint8_t addr7) {
  Wire.beginTransmission(addr7);
  uint8_t err = Wire.endTransmission();
  return (err == 0);
}
static void detectBoard() {
  gIsXcalibur = probeI2C(I2C_XCALIBUR);
}

// ---------- internal ----------
static void applyEnableFlags(bool wantCPU, bool wantFAN) {
  if (gEnableCPU != wantCPU) {
    gEnableCPU = wantCPU;
    if (!gEnableCPU && CH5_COUNT) drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
  }
  if (gEnableFAN != wantFAN) {
    gEnableFAN = wantFAN;
    if (!gEnableFAN && CH6_COUNT) drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
  }
}

// ---------- public ----------
void begin(const RGBsmbusPins& pins, uint8_t ch5Count, uint8_t ch6Count) {
  PINS = pins;
  CH5_COUNT = ch5Count>10?10:ch5Count;
  CH6_COUNT = ch6Count>10?10:ch6Count;

  cpuStrip.updateLength(CH5_COUNT); cpuStrip.setPin(PINS.ch5);
  fanStrip.updateLength(CH6_COUNT); fanStrip.setPin(PINS.ch6);

  cpuStrip.begin(); cpuStrip.clear(); cpuStrip.setBrightness(BRIGHTNESS); cpuStrip.show();
  fanStrip.begin(); fanStrip.clear(); fanStrip.setBrightness(BRIGHTNESS); fanStrip.show();

  lastAppliedBrightnessCpu = BRIGHTNESS;
  lastAppliedBrightnessFan = BRIGHTNESS;

  Wire.begin(PINS.sda, PINS.scl);
  Wire.setClock(72000);     // <<<<<< requested 72 kHz
#ifdef ARDUINO_ARCH_ESP32
  Wire.setTimeOut(20);      // keep bus stalls short to avoid UI hiccups
#endif

  detectBoard();

  lastPoll = 0;
  smoothedCpu = 0.f; smoothedFan = 0.f;

  // Initialize local flags from RGBCtrl's saved state
  applyEnableFlags(RGBCtrl::smbusCpuEnabled(), RGBCtrl::smbusFanEnabled());
}

static void updateOnce() {
  // Mirror UI flags on every cycle (cheap & keeps in sync)
  applyEnableFlags(RGBCtrl::smbusCpuEnabled(), RGBCtrl::smbusFanEnabled());

  // If both disabled, skip the bus entirely and ensure LEDs are off
  if (!gEnableCPU && !gEnableFAN) {
    if (CH5_COUNT) drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
    if (CH6_COUNT) drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
    return;
  }

  bool ok = true;
  int cpuC = -1;
  int fanP = -1;

  if (gEnableCPU) {
    uint8_t v=0;
    if (readCpuCelsius(v)) cpuC = (int)v; else ok = false;
  }
  if (gEnableFAN) {
    uint8_t v=0;
    if (readFanPercent(v)) fanP = (int)v; else ok = false;
  }

  if (ok) {
    if (gEnableCPU) {
      if (cpuC >= 0) {
        smoothedCpu = SMOOTH_ALPHA * float(cpuC) + (1.f-SMOOTH_ALPHA)*smoothedCpu;
        drawBar(cpuStrip, lastAppliedBrightnessCpu,
                CH5_COUNT,
                barLen(smoothedCpu, CPU_MAX_C, CH5_COUNT),
                colorForCpu(smoothedCpu));
      } else {
        drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
      }
    } else {
      drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
    }

    if (gEnableFAN) {
      if (fanP >= 0) {
        smoothedFan = SMOOTH_ALPHA * float(fanP) + (1.f-SMOOTH_ALPHA)*smoothedFan;
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
    // Error indicator on first pixel of enabled bars
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

// Manual refresh hook
void refreshNow() { updateOnce(); }

// These remain as direct controls if you want to toggle outside RGBCtrl
void setCpuEnabled(bool en) { applyEnableFlags(en, gEnableFAN); }
void setFanEnabled(bool en) { applyEnableFlags(gEnableCPU, en); }
bool cpuEnabled() { return gEnableCPU; }
bool fanEnabled() { return gEnableFAN; }

bool isXcalibur() { return gIsXcalibur; }

// Optional: tiny REST API (can be kept or removed; not required now that we mirror RGBCtrl flags)
void attachWeb(AsyncWebServer& server, const char* basePath) {
  String base = (basePath && *basePath) ? basePath : "/config/smbus";
  String api  = base + "/api/flags";

  server.on(api.c_str(), HTTP_GET, [](AsyncWebServerRequest* r){
    auto* s = r->beginResponseStream("application/json");
    // report the *effective* flags (mirroring RGBCtrl)
    bool cpu = RGBCtrl::smbusCpuEnabled();
    bool fan = RGBCtrl::smbusFanEnabled();
    s->printf("{\"cpu\":%s,\"fan\":%s,\"xcalibur\":%s}",
              cpu?"true":"false", fan?"true":"false", gIsXcalibur?"true":"false");
    r->send(s);
  });

  server.on(api.c_str(), HTTP_POST, [](AsyncWebServerRequest* r){
    r->send(405, "text/plain", "POST with body only");
  },
  nullptr,
  [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
    // Keeping this endpoint for compatibility: it toggles local flags and LEDs,
    // but note RGBCtrl will overwrite on next poll if its saved flags differ.
    bool ok = true;
    String body((const char*)data, len);
    body.replace(" ", ""); body.replace("\n", ""); body.replace("\r", "");
    int icpu = body.indexOf("\"cpu\":");
    int ifan = body.indexOf("\"fan\":");
    bool cpu=gEnableCPU, fan=gEnableFAN;
    if (icpu >= 0) {
      int v = body.indexOf("true", icpu);  int f = body.indexOf("false", icpu);
      if      (v>=0 && (f<0 || v<f)) cpu = true;
      else if (f>=0 && (v<0 || f<v)) cpu = false;
      else ok = false;
    }
    if (ifan >= 0) {
      int v = body.indexOf("true", ifan);  int f = body.indexOf("false", ifan);
      if      (v>=0 && (f<0 || f<v)) fan = true;
      else if (f>=0 && (v<0 || f<v)) fan = false;
      else ok = false;
    }
    applyEnableFlags(cpu, fan);
    auto* s = req->beginResponseStream("application/json");
    s->printf("{\"ok\":%s,\"cpu\":%s,\"fan\":%s,\"xcalibur\":%s}",
              ok?"true":"false", gEnableCPU?"true":"false", gEnableFAN?"true":"false", gIsXcalibur?"true":"false");
    req->send(s);
  });
}

} // namespace RGBsmbus
