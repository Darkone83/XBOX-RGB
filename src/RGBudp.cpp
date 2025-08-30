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

  // --- incoming packets ---
  int pkLen = udp.parsePacket();
  if (pkLen <= 0 || pkLen >= (int)sizeof(buf)) return;

  IPAddress rip = udp.remoteIP();
  uint16_t rport = udp.remotePort();

  int read = udp.read((uint8_t*)buf, pkLen);
  if (read <= 0) return;
  buf[read] = '\0';

  // Plain-text discovery?
  if (buf[0] != '{') {
    handlePlain(rip, rport, String(buf));
    return;
  }

  StaticJsonDocument<1536> doc;
  DeserializationError jerr = deserializeJson(doc, buf);
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
    // Accept either {"cfg":{...}} or direct fields; we normalize to full JSON
    if (doc.containsKey("cfg")) {
      String js; serializeJson(doc["cfg"], js);
      if (RGBCtrl::applyJsonPreview(js)) replyOk(rip, rport, "preview");
      else replyErr(rip, rport, "preview", "bad cfg");
    } else {
      // treat whole body as cfg
      String js; serializeJson(doc, js);
      if (RGBCtrl::applyJsonPreview(js)) replyOk(rip, rport, "preview");
      else replyErr(rip, rport, "preview", "bad cfg");
    }
  }
  else if (!strcmp(op, "save")) {
    if (doc.containsKey("cfg")) {
      String js; serializeJson(doc["cfg"], js);
      if (RGBCtrl::applyJsonSave(js)) replyOk(rip, rport, "save");
      else replyErr(rip, rport, "save", "bad cfg");
    } else {
      String js; serializeJson(doc, js);
      if (RGBCtrl::applyJsonSave(js)) replyOk(rip, rport, "save");
      else replyErr(rip, rport, "save", "bad cfg");
    }
  }
  else if (!strcmp(op, "reset")) {
    RGBCtrl::resetToDefaults();
    replyOk(rip, rport, "reset");
  }
  else if (!strcmp(op, "setCounts")) {
    auto arr = doc["c"];
    if (!arr || arr.size() < 4) { replyErr(rip, rport, "setCounts", "need 4 ints"); return; }
    RGBCtrl::setCounts(arr[0]|0, arr[1]|0, arr[2]|0, arr[3]|0);
    replyOk(rip, rport, "setCounts");
  }
  else {
    replyErr(rip, rport, "op", "unknown op");
  }
}

} // namespace RGBCtrlUDP
