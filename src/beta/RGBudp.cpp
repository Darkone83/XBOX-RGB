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

void begin(uint16_t port, const char* psk) {
  gPort = port;
  gPSK = (psk && *psk) ? String(psk) : String();
  udp.begin(gPort);
}

void sendDiscovery() {
  IPAddress bcast(255,255,255,255);
  String out = String("{\"ok\":true,\"op\":\"discover\",\"name\":\"XBOX RGB\","
                      "\"ver\":\"1.4.1\",\"port\":") + gPort +
               ",\"ip\":\"" + WiFi.localIP().toString() + "\","
               "\"mac\":\"" + macStr() + "\"}";
  reply(bcast, gPort, out);
}

static void handlePlain(IPAddress ip, uint16_t port, const String& s) {
  if (s == "RGBDISC?" || s == "RGBDISC?\n") {
    String out = String("{\"ok\":true,\"op\":\"discover\",\"name\":\"XBOX RGB\","
                        "\"ver\":\"1.4.1\",\"port\":") + gPort +
                 ",\"ip\":\"" + WiFi.localIP().toString() + "\","
                 "\"mac\":\"" + macStr() + "\"}";
    // prefix so plain-text clients can detect easily
    reply(ip, port, String("RGBDISC! ") + out);
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
  DeserializationError err = deserializeJson(doc, buf);
  if (err) { replyErr(rip, rport, "parse", "bad json"); return; }

  if (!checkKey(doc)) { replyErr(rip, rport, "auth", "bad key"); return; }

  const char* op = doc["op"] | "";
  if (!*op) { replyErr(rip, rport, "op", "missing op"); return; }

  if (!strcmp(op, "discover")) {
    String out = String("{\"ok\":true,\"op\":\"discover\",\"name\":\"XBOX RGB\","
                        "\"ver\":\"1.4.1\",\"port\":") + gPort +
                 ",\"ip\":\"" + WiFi.localIP().toString() + "\","
                 "\"mac\":\"" + macStr() + "\"}";
    reply(rip, rport, out);
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
