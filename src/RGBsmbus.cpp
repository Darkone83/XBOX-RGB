#include "RGBsmbus.h"
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <WiFiUdp.h>

// === UDP quiet-window hook (no heavy JSON parsing while we touch SMBus) ===
namespace RGBCtrlUDP { void enterSmbusQuietUs(uint32_t dur_us); }

// Read the toggles saved by RGBCtrl's Web UI (no extra REST needed)
namespace RGBCtrl {
  bool smbusCpuEnabled();
  bool smbusFanEnabled();
}

// ==== Local SMBus coordination (built-in; no external poller needed) ====
#if defined(ARDUINO_ARCH_ESP32)
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
#endif

static SemaphoreHandle_t g_smbusMutex = nullptr;
static volatile uint32_t g_smbusLastMs = 0;

static inline void smbus_init_mutex() {
#if defined(ARDUINO_ARCH_ESP32)
  if (!g_smbusMutex) g_smbusMutex = xSemaphoreCreateMutex();
#endif
}
static inline void smbus_note_activity() { g_smbusLastMs = millis(); }

// Weak so a future central poller can override with strong defs.
extern "C" __attribute__((weak)) bool smbus_acquire(uint32_t timeout_ms) {
  smbus_init_mutex();
#if defined(ARDUINO_ARCH_ESP32)
  if (!g_smbusMutex) return true;
  TickType_t to = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
  return xSemaphoreTake(g_smbusMutex, to) == pdTRUE;
#else
  (void)timeout_ms; return true;
#endif
}
extern "C" __attribute__((weak)) void smbus_release() {
#if defined(ARDUINO_ARCH_ESP32)
  if (g_smbusMutex) xSemaphoreGive(g_smbusMutex);
#endif
}
extern "C" __attribute__((weak)) uint32_t smbus_last_activity_ms() {
  return g_smbusLastMs;  // 0 means "no activity observed"
}

// ========================= USER CONFIG =========================
#ifndef RGBSMBUS_PIXEL_TYPE
#define RGBSMBUS_PIXEL_TYPE (NEO_GRB + NEO_KHZ800)
#endif

static const uint8_t  BRIGHTNESS        = 160;    // CH5/CH6 bar brightness

// Cadence: very light on the bus. One RR step ~4s (+ jitter).
#ifndef RGBSMBUS_POLL_MS
#define RGBSMBUS_POLL_MS     4000
#endif
#ifndef RGBSMBUS_JITTER_MAX_MS
#define RGBSMBUS_JITTER_MAX_MS  250
#endif

static const float    SMOOTH_ALPHA      = 0.35f;  // EMA smoothing

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

// ========================= SAFETY KNOBS =========================
#ifndef RGBSMBUS_ALLOW_RS
#define RGBSMBUS_ALLOW_RS 0
#endif
#ifndef RGBSMBUS_I2C_HZ
#define RGBSMBUS_I2C_HZ 72000
#endif
#ifndef RGBSMBUS_WAIT_IDLE_MS
#define RGBSMBUS_WAIT_IDLE_MS 15
#endif
#ifndef RGBSMBUS_IDLE_STABLE
#define RGBSMBUS_IDLE_STABLE 6
#endif
#ifndef RGBSMBUS_GUARD_PER_ATTEMPT_US
#define RGBSMBUS_GUARD_PER_ATTEMPT_US 3200
#endif
#ifndef RGBSMBUS_GUARD_PER_POLL_US
#define RGBSMBUS_GUARD_PER_POLL_US 4800
#endif
#ifndef RGBSMBUS_INTER_SAMPLE_US
#define RGBSMBUS_INTER_SAMPLE_US 180
#endif
#ifndef RGBSMBUS_STUCK_POLL_THRESHOLD
#define RGBSMBUS_STUCK_POLL_THRESHOLD 3
#endif
#ifndef RGBSMBUS_TYPE_D_TTL_MS
#define RGBSMBUS_TYPE_D_TTL_MS 15000
#endif
// Additional quiet window relative to last SMBus activity.
#ifndef RGBSMBUS_MIN_QUIET_MS
#define RGBSMBUS_MIN_QUIET_MS 6
#endif
// ===============================================================

namespace RGBsmbus {

static RGBsmbusPins PINS{0,0,7,6};

static uint8_t CH5_COUNT = 5;
static uint8_t CH6_COUNT = 5;

// CH5 = CPU bar, CH6 = FAN bar
static Adafruit_NeoPixel cpuStrip(5, 1, RGBSMBUS_PIXEL_TYPE);
static Adafruit_NeoPixel fanStrip(5, 2, RGBSMBUS_PIXEL_TYPE);

static uint32_t nextPoll     = 0;
static float    smoothedCpu  = 0.0f;
static float    smoothedFan  = 0.0f;

static bool gEnableCPU   = true;
static bool gEnableFAN   = true;

static bool gIsXcalibur  = false;
static bool gBoardDetected = false;  // probe once, when safe

// last applied brightness
static uint8_t lastAppliedBrightnessCpu = 0xFF;
static uint8_t lastAppliedBrightnessFan = 0xFF;

// Round-robin step: 0=CPU, 1=FAN, 2=idle, 3=idle → repeat
static uint8_t rr_step = 0;

// -------- Type-D Expansion guard (UDP presence beacon) --------
static WiFiUDP guardUdp;
static const uint16_t TYPE_D_PORT = 50502;            // beacons arrive here
static unsigned long lastTypeDSeen = 0;

static inline unsigned long jitter_ms(unsigned long maxJ) {
  return (millis() ^ 0xA5A5u) % (maxJ + 1);
}

static void armNextPoll(uint32_t base_ms = RGBSMBUS_POLL_MS) {
  nextPoll = millis() + base_ms + jitter_ms(RGBSMBUS_JITTER_MAX_MS);
}

static void pollTypeD() {
  int len = guardUdp.parsePacket();
  while (len > 0) {
    char msg[64];
    int r = guardUdp.read((uint8_t*)msg, (len < 63 ? len : 63));
    msg[r > 0 ? r : 0] = '\0';
    if (strstr(msg, "TYPE_D_ID:6")) {
      lastTypeDSeen = millis();
    }
    len = guardUdp.parsePacket();
  }
}
static inline bool typeDPresent() {
  return (millis() - lastTypeDSeen) < RGBSMBUS_TYPE_D_TTL_MS;
}

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

// ---------- SMBus safety helpers ----------
static bool waitBusIdle(uint8_t sda, uint8_t scl,
                        uint32_t timeout_ms = RGBSMBUS_WAIT_IDLE_MS,
                        int stable_needed = RGBSMBUS_IDLE_STABLE) {
  uint32_t start = millis();
  int stable = 0;
  while ((millis() - start) < timeout_ms) {
    bool sdaHigh = digitalRead(sda) == HIGH;
    bool sclHigh = digitalRead(scl) == HIGH;
    if (sdaHigh && sclHigh) {
      if (++stable >= stable_needed) return true;
    } else {
      stable = 0;
    }
    delayMicroseconds(140);
  }
  return false;
}

static inline bool quietSinceLastPollerTouch() {
  uint32_t last = smbus_last_activity_ms();   // 0 -> no activity / no poller
  if (last == 0) return true;
  uint32_t now = millis();
  return (now - last) >= RGBSMBUS_MIN_QUIET_MS;
}

static void smbusBreather() {
  delayMicroseconds(150);
  yield();
}

// Gentle recovery if the bus *never* looks idle across several polls
static uint8_t s_stuckPolls = 0;
static void maybeRecoverWire() {
  if (++s_stuckPolls >= RGBSMBUS_STUCK_POLL_THRESHOLD) {
    Wire.begin(PINS.sda, PINS.scl);
    Wire.setClock(RGBSMBUS_I2C_HZ);
#if defined(ARDUINO_ARCH_ESP32)
    Wire.setTimeOut(20);
#endif
    s_stuckPolls = 0;
  }
}

// ---------- STOP-only single byte read (1.6-safe) ----------
static bool readByteSTOP(uint8_t addr7, uint8_t reg, uint8_t& value) {
  RGBCtrlUDP::enterSmbusQuietUs(RGBSMBUS_GUARD_PER_ATTEMPT_US);
  if (!waitBusIdle(PINS.sda, PINS.scl)) return false;
  if (!quietSinceLastPollerTouch())     return false;

  if (!smbus_acquire(5)) return false;

  Wire.beginTransmission(addr7);
  Wire.write(reg);
  int ok = (Wire.endTransmission(true) == 0);
  if (ok) smbus_note_activity();
  smbusBreather();

  int n = 0;
  if (ok) n = Wire.requestFrom((int)addr7, 1, (int)true); // STOP

  bool good = (ok && n == 1 && Wire.available());
  if (good) {
    value = Wire.read();
    smbus_note_activity();
  }

  smbus_release();
  if (good) { smbusBreather(); }
  return good;
}

// ---------- RS+read (only if allowed & not 1.6) ----------
static bool readByteRS(uint8_t addr7, uint8_t reg, uint8_t& value) {
  if (gIsXcalibur || !RGBSMBUS_ALLOW_RS) return false;
  RGBCtrlUDP::enterSmbusQuietUs(RGBSMBUS_GUARD_PER_ATTEMPT_US);
  if (!waitBusIdle(PINS.sda, PINS.scl)) return false;
  if (!quietSinceLastPollerTouch())     return false;

  if (!smbus_acquire(5)) return false;

  Wire.beginTransmission(addr7);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    smbus_release();
    return false;
  }
  smbus_note_activity();

  int n = Wire.requestFrom((int)addr7, 1, (int)true); // STOP
  bool good = (n == 1 && Wire.available());
  if (good) {
    value = Wire.read();
    smbus_note_activity();
  }

  smbus_release();
  return good;
}

static bool readOncePrefStop(uint8_t addr7, uint8_t reg, uint8_t& value) {
  if (readByteSTOP(addr7, reg, value)) return true;
  if (readByteRS(addr7, reg, value))   return true;
  return false;
}

// Light sampling: Xcalibur=1 sample, others=3 samples (median), one attempt each
static bool readMedianByte(uint8_t addr7, uint8_t reg, uint8_t& out) {
  if (gIsXcalibur) {
    uint8_t v;
    if (!readOncePrefStop(addr7, reg, v)) return false;
    out = v;
    return true;
  }

  uint8_t got = 0, a=0, b=0, c=0;
  for (uint8_t i=0; i<3; ++i) {
    uint8_t v=0;
    if (readOncePrefStop(addr7, reg, v)) {
      if (got==0) a=v; else if (got==1) b=v; else c=v;
      ++got;
      delayMicroseconds(RGBSMBUS_INTER_SAMPLE_US);
    }
  }
  if (got == 0) return false;
  if (got == 1) { out = a; return true; }
  if (got == 2) { out = uint8_t((a + b)/2); return true; }
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
  uint8_t dummy;
  return readByteSTOP(addr7, 0x00, dummy);
}

static void detectBoardLazy() {
  if (gBoardDetected) return;
  if (typeDPresent()) return;               // never probe while Type-D is around
  RGBCtrlUDP::enterSmbusQuietUs(2000);
  if (!waitBusIdle(PINS.sda, PINS.scl)) return;
  if (!quietSinceLastPollerTouch())     return;
  gIsXcalibur = probeI2C(I2C_XCALIBUR);
  gBoardDetected = true;
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

  // IMPORTANT: no internal pull-ups; Xbox SMBus has its own
  pinMode(PINS.sda, INPUT);
  pinMode(PINS.scl, INPUT);

  Wire.begin(PINS.sda, PINS.scl);
  Wire.setClock(RGBSMBUS_I2C_HZ);
#if defined(ARDUINO_ARCH_ESP32)
  Wire.setTimeOut(20); // keep stalls short to avoid UI hiccups
#endif

  smoothedCpu = 0.f; smoothedFan = 0.f;
  applyEnableFlags(RGBCtrl::smbusCpuEnabled(), RGBCtrl::smbusFanEnabled());

  // Start Type-D beacon listener
  guardUdp.begin(TYPE_D_PORT);

  rr_step = 0;
  nextPoll = 0;
}

static void updateOnceRR() {
  // Mirror UI flags each cycle
  applyEnableFlags(RGBCtrl::smbusCpuEnabled(), RGBCtrl::smbusFanEnabled());

  // If Type-D present → never touch SMBus; blanks to indicate guard
  if (typeDPresent()) {
    if (CH5_COUNT) drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
    if (CH6_COUNT) drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
    return;
  }

  // If both disabled, ensure LEDs are off and skip the bus entirely
  if (!gEnableCPU && !gEnableFAN) {
    if (CH5_COUNT) drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
    if (CH6_COUNT) drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
    return;
  }

  // Safe, light probe once when allowed
  detectBoardLazy();

  // Coarse quiet window for the whole RR step
  RGBCtrlUDP::enterSmbusQuietUs(RGBSMBUS_GUARD_PER_POLL_US);

  // Require idle lines AND spacing since last SMBus activity
  if (!waitBusIdle(PINS.sda, PINS.scl) || !quietSinceLastPollerTouch()) {
    maybeRecoverWire();
    // show blanks to indicate skip
    if (CH5_COUNT) drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
    if (CH6_COUNT) drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
    return;
  }
  s_stuckPolls = 0;

  // Round-robin: only one read per tick to avoid bursts
  const uint8_t step = (rr_step++) & 0x03;
  bool ok = true;
  int cpuC = -1;
  int fanP = -1;

  if (step == 0 && gEnableCPU) {
    uint8_t v=0; ok = readCpuCelsius(v); if (ok) cpuC = (int)v;
  } else if (step == 1 && gEnableFAN) {
    uint8_t v=0; ok = readFanPercent(v); if (ok) fanP = (int)v;
  } else {
    // idle steps (2,3) → no SMBus I/O
  }

  if (ok) {
    if (cpuC >= 0) {
      smoothedCpu = SMOOTH_ALPHA * float(cpuC) + (1.f-SMOOTH_ALPHA)*smoothedCpu;
      drawBar(cpuStrip, lastAppliedBrightnessCpu,
              CH5_COUNT,
              barLen(smoothedCpu, CPU_MAX_C, CH5_COUNT),
              colorForCpu(smoothedCpu));
    } else if (!gEnableCPU) {
      drawBar(cpuStrip, lastAppliedBrightnessCpu, CH5_COUNT, 0, 0);
    }

    if (fanP >= 0) {
      smoothedFan = SMOOTH_ALPHA * float(fanP) + (1.f-SMOOTH_ALPHA)*smoothedFan;
      drawBar(fanStrip, lastAppliedBrightnessFan,
              CH6_COUNT,
              barLen(smoothedFan, FAN_FAST_MAX, CH6_COUNT),
              colorForFan(smoothedFan));
    } else if (!gEnableFAN) {
      drawBar(fanStrip, lastAppliedBrightnessFan, CH6_COUNT, 0, 0);
    }
  } else {
    // Error blink on first pixel of *enabled* bars
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
  pollTypeD();

  const uint32_t now = millis();
  if (now >= nextPoll) {
    updateOnceRR();
    armNextPoll();
  }
}

// Manual refresh hook
void refreshNow() { updateOnceRR(); }

// Direct controls
void setCpuEnabled(bool en) { applyEnableFlags(en, gEnableFAN); }
void setFanEnabled(bool en) { applyEnableFlags(gEnableCPU, en); }
bool cpuEnabled() { return gEnableCPU; }
bool fanEnabled() { return gEnableFAN; }

bool isXcalibur() { return gIsXcalibur; }

// Optional: tiny REST API. (Kept as-is; attach from your Web setup if desired.)
void attachWeb(AsyncWebServer& server, const char* basePath) {
  String base = (basePath && *basePath) ? basePath : "/config/smbus";
  String api  = base + "/api/flags";

  server.on(api.c_str(), HTTP_GET, [](AsyncWebServerRequest* r){
    bool cpuSaved = RGBCtrl::smbusCpuEnabled();
    bool fanSaved = RGBCtrl::smbusFanEnabled();
    bool guarded  = typeDPresent();

    bool cpuEff = guarded ? false : cpuSaved;
    bool fanEff = guarded ? false : fanSaved;

    auto* s = r->beginResponseStream("application/json");
    s->printf("{\"cpu\":%s,\"fan\":%s,\"savedCpu\":%s,\"savedFan\":%s,"
              "\"guarded\":%s,\"guardReason\":\"%s\",\"xcalibur\":%s}",
              cpuEff?"true":"false",
              fanEff?"true":"false",
              cpuSaved?"true":"false",
              fanSaved?"true":"false",
              guarded?"true":"false",
              guarded?"TypeD":"none",
              gIsXcalibur?"true":"false");
    r->send(s);
  });

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

    bool guarded  = typeDPresent();
    bool cpuEff = guarded ? false : gEnableCPU;
    bool fanEff = guarded ? false : gEnableFAN;

    auto* s = req->beginResponseStream("application/json");
    s->printf("{\"ok\":%s,\"cpu\":%s,\"fan\":%s,\"guarded\":%s,\"guardReason\":\"%s\",\"xcalibur\":%s}",
              ok?"true":"false",
              cpuEff?"true":"false",
              fanEff?"true":"false",
              guarded?"true":"false",
              guarded?"TypeD":"none",
              gIsXcalibur?"true":"false");
    req->send(s);
  });
}

} // namespace RGBsmbus
