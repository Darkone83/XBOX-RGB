#include "wifimgr.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "led_stat.h"
#include <vector>
#include <algorithm>
#include "esp_wifi.h"
#include <Update.h>  // OTA

// Single global server used by the whole project
static AsyncWebServer server(80);

namespace WiFiMgr {

static String ssid, password;
static Preferences prefs;
static DNSServer dnsServer;
static std::vector<String> lastScanResults;

enum class State { IDLE, CONNECTING, CONNECTED, PORTAL };
static State state = State::PORTAL;

static int connectAttempts = 0;
static const int maxAttempts = 10;
static unsigned long lastAttempt = 0;
static unsigned long retryDelay = 3000;

// Ensure portal routes are only added once (so we don't need server.reset()).
static bool portalRoutesAdded = false;

AsyncWebServer& getServer() {
  return server;
}

// ---------------- WiFi cred storage ----------------
static void loadCreds() {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  password = prefs.getString("pass", "");
  prefs.end();
}

static void saveCreds(const String& s, const String& p) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}

static void clearCreds() {
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// --------------- AP/Portal helpers -----------------
static void setAPConfig() {
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );
}

static void addPortalRoutesOnce() {
  if (portalRoutesAdded) return;
  portalRoutesAdded = true;

  // ---------- Portal UI ----------
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String page = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <title>WiFi Setup</title>
  <meta name="viewport" content="width=320,initial-scale=1">
  <style>
    body {background:#111;color:#EEE;font-family:system-ui,Segoe UI,Roboto,Arial;margin:0}
    .container {max-width:340px;margin:24px auto;background:#222;padding:1.2em;border-radius:10px;box-shadow:0 8px 20px #0008;}
    h1 {margin:0 0 .6em; font-size:1.6em}
    input,select,button {width:100%;box-sizing:border-box;margin:.6em 0;padding:.6em;font-size:1em;border-radius:7px;border:1px solid #555;background:#111;color:#EEE}
    .btn-primary {background:#299a2c;border:0;color:white}
    .btn-danger {background:#a22;border:0;color:white}
    .btn-ota {background:#265aa5;border:0;color:white}
    .btn-config {background:#7a3ef0;border:0;color:white}
    .row {display:grid;grid-template-columns:1fr;gap:.6em}
    .status {margin-top:8px;opacity:.85}
  </style>
</head>
<body>
  <div class="container">
    <h1>XBOX RGB Setup</h1>
    <div class="row">
      <label>WiFi Network</label>
      <select id="ssidDropdown">
        <option value="">Scanning...</option>
      </select>
      <input type="text" id="ssid" placeholder="SSID">
      <label>Password</label>
      <input type="password" id="pass" placeholder="WiFi Password">
      <button type="button" onclick="save()" class="btn-primary">Connect & Save</button>
      <button type="button" onclick="forget()" class="btn-danger">Forget WiFi</button>
      <button type="button" onclick="window.location='/ota'" class="btn-ota">OTA Update</button>
      <button type="button" onclick="window.location='/config'" class="btn-config">Config</button>
      <div class="status" id="status">Status: ...</div>
    </div>
  </div>
<script>
  function scan() {
    fetch('/scan').then(r => r.json()).then(list => {
      let dd = document.getElementById('ssidDropdown');
      dd.innerHTML = '';
      let def = document.createElement('option');
      def.value = '';
      def.text = list.length ? 'Please select a network' : 'No networks found';
      dd.appendChild(def);
      list.forEach(name => {
        let opt = document.createElement('option');
        opt.value = name;
        opt.text = name;
        dd.appendChild(opt);
      });
      dd.onchange = function(){ document.getElementById('ssid').value = dd.value; };
    }).catch(() => {
      let dd = document.getElementById('ssidDropdown');
      dd.innerHTML = '';
      let opt = document.createElement('option');
      opt.value = '';
      opt.text = 'Scan failed';
      dd.appendChild(opt);
    });
  }
  setInterval(scan, 2500);
  window.onload = scan;

  function save() {
    let ssid = document.getElementById('ssid').value;
    let pass = document.getElementById('pass').value;
    fetch('/save',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:ssid,pass:pass})
    }).then(r=>r.text()).then(t=>{ document.getElementById('status').innerText=t; });
  }
  function forget() {
    fetch('/forget').then(r=>r.text()).then(t=>{
      document.getElementById('status').innerText=t;
      document.getElementById('ssid').value='';
      document.getElementById('pass').value='';
    });
  }
</script>
</body>
</html>
)HTML";
    request->send(200, "text/html", page);
  });

  // ---------- OTA PAGE (simple & robust) ----------
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *request){
    static const char kPage[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Update</title>
<style>
  body{background:#111;color:#EEE;font-family:system-ui,Segoe UI,Roboto,Arial;margin:0}
  .box{max-width:360px;margin:32px auto;background:#222;padding:18px;border-radius:10px;box-shadow:0 8px 20px #0008}
  h2{margin:0 0 12px} input,button{width:100%;margin:.5rem 0;padding:.6rem;border-radius:6px;border:1px solid #555;background:#111;color:#EEE}
  button{background:#2563eb;color:#fff;border:0}
  a{display:inline-block;margin-top:10px;color:#9ec1ff}
</style></head>
<body><div class="box">
  <h2>OTA Update</h2>
  <form method="POST" action="/update" enctype="multipart/form-data">
    <input type="file" name="firmware" accept=".bin,.bin.gz" required>
    <button type="submit">Upload & Flash</button>
  </form>
  <a href="/">⟵ Back to WiFi Setup</a>
  <a href="/config">Open Config</a>
</div></body></html>
)HTML";
    request->send(200, "text/html", kPage);
  });

  // ---------- OTA FLASH (chunk-safe) ----------
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // reply will be sent from upload handler on 'final'
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      static bool hadError = false;

      if (index == 0) {
        hadError = false;
        Serial.printf("[OTA] Start: %s (%u bytes expected)\n", filename.c_str(), (unsigned)request->contentLength());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
          hadError = true;
        }
      }

      if (!hadError && len) {
        size_t written = Update.write(data, len);
        if (written != len) {
          Serial.printf("[OTA] Write failed: wrote %u of %u\n", (unsigned)written, (unsigned)len);
          Update.printError(Serial);
          hadError = true;
        }
      }

      if (final) {
        bool ok = !hadError && Update.end(true); // set as boot partition
        if (!ok) Update.printError(Serial);

        AsyncWebServerResponse *res = request->beginResponse(
          ok ? 200 : 500, "text/plain",
          ok ? "Update complete. Rebooting..." : "Update failed. See serial log."
        );
        res->addHeader("Connection", "close");
        request->send(res);

        Serial.printf("[OTA] %s (%s)\n", ok ? "Success" : "Failed", filename.c_str());
        if (ok) {
          delay(600); // let HTTP response flush
          ESP.restart();
        }
      }
    }
  );

  // ---------- WiFi status ----------
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String stat;
    if (WiFi.status() == WL_CONNECTED)
      stat = "Connected to " + WiFi.SSID() + " - IP: " + WiFi.localIP().toString();
    else if (state == State::CONNECTING)
      stat = "Connecting to " + ssid + "...";
    else
      stat = "In portal mode";
    request->send(200, "text/plain", stat);
  });

  // ---------- Connect (GET) ----------
  server.on("/connect", HTTP_GET, [](AsyncWebServerRequest *request){
    String ss, pw;
    if (request->hasParam("ssid")) ss = request->getParam("ssid")->value();
    if (request->hasParam("pass")) pw = request->getParam("pass")->value();
    if (ss.length() == 0) {
      request->send(400, "text/plain", "SSID missing");
      return;
    }
    saveCreds(ss, pw);
    ssid = ss; password = pw;
    state = State::CONNECTING;
    connectAttempts = 1;
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
    request->send(200, "text/plain", "Connecting to: " + ssid);
  });

  // ---------- Save creds (POST JSON body) ----------
  server.on("/save", HTTP_POST,
    [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      String body; body.reserve(len ? len : 64);
      for (size_t i=0;i<len;i++) body += (char)data[i];
      // crude parse: {"ssid":"...","pass":"..."}
      int ssidStart = body.indexOf("\"ssid\":\"") + 8;
      int ssidEnd   = body.indexOf("\"", ssidStart);
      int passStart = body.indexOf("\"pass\":\"") + 8;
      int passEnd   = body.indexOf("\"", passStart);
      String newSsid = (ssidStart >= 8 && ssidEnd > ssidStart) ? body.substring(ssidStart, ssidEnd) : "";
      String newPass = (passStart >= 8 && passEnd > passStart) ? body.substring(passStart, passEnd) : "";
      if (newSsid.length() == 0) {
        request->send(400, "text/plain", "SSID missing");
        return;
      }
      saveCreds(newSsid, newPass);
      ssid = newSsid; password = newPass;
      state = State::CONNECTING;
      connectAttempts = 1;
      WiFi.begin(newSsid.c_str(), newPass.c_str());
      request->send(200, "text/plain", "Connecting to: " + newSsid);
      Serial.printf("[WiFiMgr] Received new creds. SSID: %s\n", newSsid.c_str());
    }
  );

  // ---------- Scan: return de-duped, RSSI-sorted names ----------
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    int n = WiFi.scanComplete();

    // Start async scan if not running yet
    if (n == -2) {
      // async=true, show_hidden=true (passive=false keeps it quick)
      WiFi.scanNetworks(true, true);
    }

    if (n >= 0) {
      struct Net { String name; int32_t rssi; };
      std::vector<Net> nets;
      nets.reserve(n);

      // Build unique set by SSID, keep strongest RSSI
      for (int i = 0; i < n; ++i) {
        String name = WiFi.SSID(i);
        if (!name.length()) continue; // ignore hidden/empty SSIDs
        int32_t rssi = WiFi.RSSI(i);

        bool merged = false;
        for (auto &it : nets) {
          if (it.name == name) {
            if (rssi > it.rssi) it.rssi = rssi;
            merged = true;
            break;
          }
        }
        if (!merged) nets.push_back({name, rssi});
      }

      // sort by strongest first
      std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b){ return a.rssi > b.rssi; });

      lastScanResults.clear();
      lastScanResults.reserve(nets.size());
      for (auto &it : nets) lastScanResults.push_back(it.name);

      WiFi.scanDelete();
      // Immediately kick off a new async scan so results stay fresh
      WiFi.scanNetworks(true, true);
    }

    // Return cached (or just-updated) names only
    String json = "[";
    for (size_t i=0; i<lastScanResults.size(); ++i) {
      if (i) json += ",";
      json += "\"" + lastScanResults[i] + "\"";
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // ---------- Forget ----------
  server.on("/forget", HTTP_GET, [](AsyncWebServerRequest *request){
    clearCreds();
    ssid = ""; password = "";
    WiFi.disconnect();
    state = State::PORTAL;
    request->send(200, "text/plain", "WiFi credentials cleared.");
  });

  // ---------- Captive portal helpers ----------
  auto cp = [](AsyncWebServerRequest *r){
    r->send(200, "text/html", "<meta http-equiv='refresh' content='0; url=/' />");
  };
  server.on("/generate_204", HTTP_GET, cp);
  server.on("/hotspot-detect.html", HTTP_GET, cp);
  server.on("/redirect", HTTP_GET, cp);
  server.on("/ncsi.txt", HTTP_GET, cp);
  server.on("/captiveportal", HTTP_GET, cp);
  server.onNotFound(cp);

  // No caching for UI/API
  DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");
}

static void startPortal() {
  WiFi.disconnect(true);
  delay(100);
  setAPConfig();
  WiFi.mode(WIFI_AP_STA);  // AP+STA so scan works while in portal
  delay(100);

  // Channel 6 for iOS compatibility
  bool apok = WiFi.softAP("XBOX RGB Setup", "", 6, 0);
  esp_wifi_set_max_tx_power(20);
  LedStat::setStatus(LedStatus::Portal);
  Serial.printf("[WiFiMgr] softAP=%d, IP: %s\n", apok, WiFi.softAPIP().toString().c_str());
  delay(200);

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);

  // Do NOT reset the server: keep previously-registered routes (like /config) live in AP mode.
  addPortalRoutesOnce();

  server.begin();
  state = State::PORTAL;

  // Kick off an initial async scan
  WiFi.scanNetworks(true, true);
}

static void stopPortal() {
  dnsServer.stop();
}

static void tryConnect() {
  if (ssid.length() > 0) {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
    state = State::CONNECTING;
    connectAttempts = 1;
    lastAttempt = millis();
  } else {
    startPortal();
  }
}

// ---------------- Public API ----------------
void begin() {
  LedStat::setStatus(LedStatus::Booting);
  loadCreds();
  startPortal();    // always start captive portal
  if (ssid.length() > 0) {
    tryConnect();   // attempt STA connect in background
  }
}

void loop() {
  dnsServer.processNextRequest();

  if (state == State::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      state = State::CONNECTED;
      dnsServer.stop();
      Serial.println("[WiFiMgr] WiFi connected.");
      Serial.print("[WiFiMgr] IP Address: ");
      Serial.println(WiFi.localIP());
      LedStat::setStatus(LedStatus::WifiConnected);
    } else if (millis() - lastAttempt > retryDelay) {
      connectAttempts++;
      if (connectAttempts >= maxAttempts) {
        state = State::PORTAL;
        startPortal();
        LedStat::setStatus(LedStatus::WifiFailed);
      } else {
        WiFi.disconnect();
        WiFi.begin(ssid.c_str(), password.c_str());
        lastAttempt = millis();
      }
    }
  }
}

void restartPortal() {
  startPortal();
}

void forgetWiFi() {
  clearCreds();
  startPortal();
}

bool isConnected() {
  return WiFi.status() == WL_CONNECTED;
}

String getStatus() {
  if (isConnected()) return "Connected to: " + ssid;
  if (state == State::CONNECTING) return "Connecting to: " + ssid;
  return "Not connected";
}

} // namespace WiFiMgr
