#include "RGBudp.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include "RGBCtrl.h"

namespace RGBCtrlUDP {

static WiFiUDP udp;
static uint16_t gPort = 7777;
static String gPSK;
static char buf[1600];  // fits our JSON comfortably

// --- periodic advertisement control ---
static uint32_t lastAdvertMs = 0;
static uint8_t  fastBurstsLeft = 3;           // a few fast announces right after boot
static const uint32_t ADVERT_FAST_MS = 3000;  // 3s for first few
static const uint32_t ADVERT_SLOW_MS = 15000; // 15s afterwards
static IPAddress lastIp;                      // re-announce on IP change

// ---------- SMBus quiet window (avoid jitter while SMBus transacts) ----------
static volatile uint32_t gQuietUntilUs = 0;
static inline bool quietActive() {
  // Signed difference to handle micros() wrap
  return (int32_t)(gQuietUntilUs - micros()) > 0;
}
// Make this visible to SMBus (declare in RGBudp.h as noted above)
void enterSmbusQuietUs(uint32_t dur_us) {
  uint32_t now = micros();
  uint32_t t   = now + dur_us;
  // Extend if already inside a quiet window
  if ((int32_t)(gQuietUntilUs - now) > 0) {
    if ((int32_t)(t - gQuietUntilUs) > 0) gQuietUntilUs = t;
  } else {
    gQuietUntilUs = t;
  }
}

// ---------- Pending / coalesced work (applied in tiny slices) ----------
static bool     pendHasCfg     = false;
static bool     pendIsSave     = false;   // true=save, false=preview
static String   pendJson;                 // normalized cfg JSON

static bool     pendHasCounts  = false;
static uint16_t pendCounts[4]  = {0,0,0,0};

static bool     pendDoReset    = false;

// Raw packet deferral (avoid JSON parse during quiet window)
static bool     pendHasRaw     = false;
static IPAddress pendRawIp;
static uint16_t  pendRawPort = 0;
static size_t    pendRawLen  = 0;
static char      pendRawBuf[sizeof(buf)];

// Forward decl
static void handleJsonPacket(const char* data, int len, IPAddress rip, uint16_t rport);

// Time-boxed processor for pending heavy ops. One item per call.
void processPending(uint32_t budget_us) {
  const uint32_t t0 = micros();

  // 1) If a raw packet was deferred (e.g., SMBus quiet), handle it now.
  if (pendHasRaw && !quietActive()) {
    pendHasRaw = false;
    handleJsonPacket(pendRawBuf, (int)pendRawLen, pendRawIp, pendRawPort);
    return; // process one heavy item per loop
  }

  // 2) Reset
  if (pendDoReset) {
    pendDoReset = false;
    RGBCtrl::resetToDefaults();
    if (micros() - t0 >= budget_us) return;
  }

  // 3) Counts
  if (pendHasCounts) {
    pendHasCounts = false;
    RGBCtrl::setCounts(pendCounts[0], pendCounts[1], pendCounts[2], pendCounts[3]);
    if (micros() - t0 >= budget_us) return;
  }

  // 4) Config (coalesced to latest)
  if (pendHasCfg) {
    String js = pendJson;      // local copy
    bool doSave = pendIsSave;
    pendHasCfg = false;

    if (doSave) {
      RGBCtrl::applyJsonSave(js);
    } else {
      RGBCtrl::applyJsonPreview(js);
    }
    (void)budget_us;
  }
}

static String macStr() {
  uint8_t m[6]; WiFi.macAddress(m);
  char s[18]; snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
                       m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(s);
}

static void reply(IPAddress ip, uint16_t port, const String& json) {
  udp.beginPacket(ip, port);
  udp.write((const uint8_t*)json.c_str(), json.length());
  udp.endPacket();
}

static void replyOk(IPAddress ip, uint16_t port, const char* op, const String* cfg = nullptr) {
  String out = String("{\"ok\":true,\"op\":\"") + op + "\"";
  if (cfg) { out += ",\"cfg\":"; out += *cfg; }
  out += "}";
  reply(ip, port, out);
}
static void replyErr(IPAddress ip, uint16_t port, const char* op, const char* err) {
  String out = String("{\"ok\":false,\"op\":\"") + op + "\",\"err\":\"" + err + "\"}";
  reply(ip, port, out);
}

static String buildDiscoverJson() {
  // Keep "ver" for compatibility; consumers can ignore it.
  String out = String("{\"ok\":true,\"op\":\"discover\",\"name\":\"XBOX RGB\",")
             + "\"ver\":\"1.4.x\",\"port\":" + String(gPort)
             + ",\"ip\":\"" + WiFi.localIP().toString() + "\""
             + ",\"mac\":\"" + macStr() + "\"}";
  return out;
}

void begin(uint16_t port, const char* psk) {
  gPort = port;
  gPSK = (psk && *psk) ? String(psk) : String();
  udp.begin(gPort);

  lastIp = WiFi.localIP();
  // Send an immediate boot advertisement (both formats).
  IPAddress bcast(255,255,255,255);
  String js = buildDiscoverJson();
  reply(bcast, gPort, js);
  reply(bcast, gPort, String("RGBDISC! ") + js);
  lastAdvertMs = millis();
}

void sendDiscovery() {
  IPAddress bcast(255,255,255,255);
  String js = buildDiscoverJson();
  // JSON broadcast
  reply(bcast, gPort, js);
  // Text-prefixed broadcast for very simple listeners
  reply(bcast, gPort, String("RGBDISC! ") + js);
}

static void handlePlain(IPAddress ip, uint16_t port, const String& s) {
  if (s == "RGBDISC?" || s == "RGBDISC?\n") {
    String js = buildDiscoverJson();
    // prefix so plain-text clients can detect easily
    reply(ip, port, String("RGBDISC! ") + js);
  } else {
    reply(ip, port, "{\"ok\":false,\"op\":\"raw\",\"err\":\"unknown text\"}");
  }
}

static bool checkKey(JsonVariantConst root) {
  if (gPSK.isEmpty()) return true;
  auto k = root["key"];
  if (!k.is<const char*>()) return false;
  return gPSK.equals(k.as<const char*>());
}

// Small helpers to queue work
static void queuePreviewOrSave(const String& js, bool isSave) {
  pendJson   = js;
  pendIsSave = isSave;
  pendHasCfg = true;  // coalesces to latest
}
static void queueSetCounts(uint16_t c0, uint16_t c1, uint16_t c2, uint16_t c3) {
  pendCounts[0] = c0; pendCounts[1] = c1; pendCounts[2] = c2; pendCounts[3] = c3;
  pendHasCounts = true;
}

// Heavy JSON handler (used by normal path and deferred-raw path)
static void handleJsonPacket(const char* data, int len, IPAddress rip, uint16_t rport) {
  StaticJsonDocument<1536> doc;
  DeserializationError jerr = deserializeJson(doc, data, len);
  if (jerr) { replyErr(rip, rport, "parse", "bad json"); return; }

  if (!checkKey(doc)) { replyErr(rip, rport, "auth", "bad key"); return; }

  const char* op = doc["op"] | "";
  if (!*op) { replyErr(rip, rport, "op", "missing op"); return; }

  if (!strcmp(op, "discover")) {
    String js = buildDiscoverJson();
    reply(rip, rport, js);
  }
  else if (!strcmp(op, "get")) {
    String cfg = RGBCtrl::getConfigJson();
    replyOk(rip, rport, "get", &cfg);
  }
  else if (!strcmp(op, "preview")) {
    // Normalize to cfg JSON and queue (reply OK immediately; apply later)
    String js;
    if (doc.containsKey("cfg")) serializeJson(doc["cfg"], js);
    else serializeJson(doc, js);
    queuePreviewOrSave(js, /*isSave=*/false);
    replyOk(rip, rport, "preview");
  }
  else if (!strcmp(op, "save")) {
    String js;
    if (doc.containsKey("cfg")) serializeJson(doc["cfg"], js);
    else serializeJson(doc, js);
    queuePreviewOrSave(js, /*isSave=*/true);
    replyOk(rip, rport, "save");
  }
  else if (!strcmp(op, "reset")) {
    pendDoReset = true;        // queue to avoid doing it in RX path
    replyOk(rip, rport, "reset");
  }
  else if (!strcmp(op, "setCounts")) {
    auto arr = doc["c"];
    if (!arr || arr.size() < 4) { replyErr(rip, rport, "setCounts", "need 4 ints"); return; }
    queueSetCounts(arr[0]|0, arr[1]|0, arr[2]|0, arr[3]|0);
    replyOk(rip, rport, "setCounts");
  }
  else {
    replyErr(rip, rport, "op", "unknown op");
  }
}

void loop() {
  // --- periodic self-advertisement (proactive presence broadcast) ---
  const bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if (wifiUp) {
    // Re-announce immediately on IP change (e.g., after DHCP renew/connect).
    IPAddress nowIp = WiFi.localIP();
    if (nowIp != lastIp) {
      lastIp = nowIp;
      sendDiscovery();
      lastAdvertMs = millis();
      fastBurstsLeft = 3; // do a few fast announces again
    }
    uint32_t now = millis();
    uint32_t interval = fastBurstsLeft ? ADVERT_FAST_MS : ADVERT_SLOW_MS;
    if (now - lastAdvertMs >= interval) {
      sendDiscovery();
      lastAdvertMs = now;
      if (fastBurstsLeft) fastBurstsLeft--;
    }
  }

  // Always give pending work a small budget each pass to avoid long stalls.
  processPending(1500);

  // --- incoming packets ---
  int pkLen = udp.parsePacket();
  if (pkLen <= 0 || pkLen >= (int)sizeof(buf)) return;

  IPAddress rip = udp.remoteIP();
  uint16_t rport = udp.remotePort();

  int read = udp.read((uint8_t*)buf, pkLen);
  if (read <= 0) return;
  buf[read] = '\0';

  // Plain-text discovery is always cheap; answer immediately.
  if (buf[0] != '{') {
    handlePlain(rip, rport, String(buf));
    return;
  }

  // If SMBus has requested a quiet window, defer the whole JSON packet.
  if (quietActive()) {
    // Keep only ONE raw packet; overwrite older if new arrives (coalescing).
    size_t toCopy = (read < (int)sizeof(pendRawBuf)) ? (size_t)read : (sizeof(pendRawBuf)-1);
    memcpy(pendRawBuf, buf, toCopy);
    pendRawBuf[toCopy] = '\0';
    pendRawLen  = toCopy;
    pendRawIp   = rip;
    pendRawPort = rport;
    pendHasRaw  = true;
    return;
  }

  // No quiet guard active → handle JSON now
  handleJsonPacket(buf, read, rip, rport);
}

} // namespace RGBCtrlUDP
