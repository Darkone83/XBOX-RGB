#include "wifimgr.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "led_stat.h"
#include <vector>
#include "esp_wifi.h"
#include <Update.h> // For OTA

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

AsyncWebServer& getServer() {
    return server;
}

static void setAPConfig() {
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
}

void loadCreds() {
    prefs.begin("wifi", true);
    ssid = prefs.getString("ssid", "");
    password = prefs.getString("pass", "");
    prefs.end();
}

void saveCreds(const String& s, const String& p) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
    prefs.end();
}

void clearCreds() {
    prefs.begin("wifi", false);
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
}

void startPortal() {
    WiFi.disconnect(true);
    delay(100);
    setAPConfig();
    WiFi.mode(WIFI_AP_STA);  // AP+STA for S3
    delay(100);

    // Use channel 6 for iOS compatibility, or try 1
    bool apok = WiFi.softAP("XBOX RGB Setup", "", 6, 0);
    esp_wifi_set_max_tx_power(20);
    LedStat::setStatus(LedStatus::Portal);
    Serial.printf("[WiFiMgr] softAP result: %d, IP: %s\n", apok, WiFi.softAPIP().toString().c_str());
    delay(200);

    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(53, "*", apIP);

    server.reset(); // Needed to avoid double route definition on multiple starts

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>WiFi Setup</title>
    <meta name="viewport" content="width=320,initial-scale=1">
    <style>
        body {background:#111;color:#EEE;font-family:sans-serif;}
        .container {max-width:320px;margin:24px auto;background:#222;padding:2em;border-radius:8px;box-shadow:0 0 16px #0008;}
        input,select,button {width:100%;box-sizing:border-box;margin:.7em 0;padding:.5em;font-size:1.1em;border-radius:5px;border:1px solid #555;}
        .btn-primary {background:#299a2c;color:white;}
        .btn-danger {background:#a22;color:white;}
        .btn-ota {background:#265aa5;color:white;}
        .status {margin-top:1em;font-size:.95em;}
        label {display:block;margin-top:.5em;margin-bottom:.1em;}
    </style>
</head>
<body>
    <div class="container">
        <div style="width:100%;text-align:center;margin-bottom:1em">
            <span style="font-size:2em;font-weight:bold;">XBOX RGB Setup</span>
        </div>
        <form id="wifiForm">
            <label>WiFi Network</label>
            <select id="ssidDropdown" style="margin-bottom:1em;">
                <option value="">Please select a network</option>
            </select>
            <input type="text" id="ssid" placeholder="SSID" style="margin-bottom:1em;">
            <label>Password</label>
            <input type="password" id="pass" placeholder="WiFi Password">
            <button type="button" onclick="save()" class="btn-primary">Connect & Save</button>
            <button type="button" onclick="forget()" class="btn-danger">Forget WiFi</button>
            <button type="button" onclick="window.location='/ota'" class="btn-ota">OTA Update</button>
        </form>
        <div class="status" id="status">Status: ...</div>
    </div>
    <script>
        function scan() {
            fetch('/scan').then(r => r.json()).then(list => {
                let dropdown = document.getElementById('ssidDropdown');
                dropdown.innerHTML = '';
                let defaultOpt = document.createElement('option');
                defaultOpt.value = '';
                defaultOpt.text = 'Please select a network';
                dropdown.appendChild(defaultOpt);
                list.forEach(ssid => {
                    let opt = document.createElement('option');
                    opt.value = ssid;
                    opt.text = ssid;
                    dropdown.appendChild(opt);
                });
                dropdown.onchange = function() {
                    document.getElementById('ssid').value = dropdown.value;
                };
            }).catch(() => {
                let dropdown = document.getElementById('ssidDropdown');
                dropdown.innerHTML = '';
                let opt = document.createElement('option');
                opt.value = '';
                opt.text = 'Scan failed';
                dropdown.appendChild(opt);
            });
        }
        setInterval(scan, 2000);
        window.onload = scan;
        function save() {
            let ssid = document.getElementById('ssid').value;
            let pass = document.getElementById('pass').value;
            fetch('/save', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ssid:ssid,pass:pass})
            }).then(r => r.text()).then(t => {
                document.getElementById('status').innerText = t;
            });
        }
        function forget() {
            fetch('/forget').then(r => r.text()).then(t => {
                document.getElementById('status').innerText = t;
                document.getElementById('ssid').value = '';
                document.getElementById('pass').value = '';
            });
        }
    </script>
</body>
</html>
        )rawliteral";
        request->send(200, "text/html", page);
    });

    // === OTA PAGE ===
    server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *request){
        String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>OTA Update</title>
    <meta name="viewport" content="width=320,initial-scale=1">
    <style>
        body {background:#111;color:#EEE;font-family:sans-serif;}
        .container {max-width:340px;margin:24px auto;background:#222;padding:2em;border-radius:8px;box-shadow:0 0 16px #0008;}
        input[type=file],button {width:100%;box-sizing:border-box;margin:.7em 0;padding:.5em;font-size:1.1em;border-radius:5px;border:1px solid #555;}
        .btn-update {background:#265aa5;color:white;}
        .status {margin-top:1em;font-size:.95em;}
        label {display:block;margin-top:.5em;margin-bottom:.1em;}
    </style>
</head>
<body>
    <div class="container">
        <h2>OTA Update</h2>
        <form id="otaForm" method="POST" action="/update" enctype="multipart/form-data">
            <label>Select firmware .bin file</label>
            <input type="file" name="firmware">
            <button type="submit" class="btn-update">Upload & Flash</button>
        </form>
        <div id="otaStatus" class="status"></div>
        <button onclick="window.location='/'" class="btn-update" style="margin-top:14px;">Back to WiFi Setup</button>
    </div>
</body>
</html>
        )rawliteral";
        request->send(200, "text/html", page);
    });

    // === OTA FIRMWARE UPLOAD HANDLER ===
    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            static bool updateError = false;
            if (!index) {
                Serial.printf("[OTA] Start update: %s\n", filename.c_str());
                updateError = false;
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // start with max available size
                    Update.printError(Serial);
                    updateError = true;
                }
            }
            if (!updateError && !Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                    updateError = true;
                }
            }
            if (final) {
                if (!updateError && Update.end(true)) {
                    Serial.println("[OTA] Update Success. Rebooting...");
                    request->send(200, "text/plain", "Update complete! Rebooting...");
                    delay(1200);
                    ESP.restart();
                } else {
                    Update.printError(Serial);
                    request->send(200, "text/plain", "Update failed! " + String(Update.errorString()));
                }
            }
        }
    );

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

    server.on("/connect", HTTP_GET, [](AsyncWebServerRequest *request){
        String ss, pw;
        if (request->hasParam("ssid")) ss = request->getParam("ssid")->value();
        if (request->hasParam("pass")) pw = request->getParam("pass")->value();
        if (ss.length() == 0) {
            request->send(400, "text/plain", "SSID missing");
            return;
        }
        saveCreds(ss, pw);
        ssid = ss;
        password = pw;
        state = State::CONNECTING;
        connectAttempts = 1;
        WiFi.mode(WIFI_AP_STA);
        delay(100);
        WiFi.begin(ssid.c_str(), password.c_str());
        request->send(200, "text/plain", "Connecting to: " + ssid);
    });

    // PATCHED /scan endpoint: caches last scan result for reliability
    server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        int n = WiFi.scanComplete();
        if (n == -2) {
            WiFi.scanNetworks(true, true);
            String json = "[";
            for (size_t i = 0; i < lastScanResults.size(); ++i) {
                if (i) json += ",";
                json += "\"" + lastScanResults[i] + "\"";
            }
            json += "]";
            request->send(200, "application/json", json);
            return;
        } else if (n == -1) {
            String json = "[";
            for (size_t i = 0; i < lastScanResults.size(); ++i) {
                if (i) json += ",";
                json += "\"" + lastScanResults[i] + "\"";
            }
            json += "]";
            request->send(200, "application/json", json);
            return;
        }
        lastScanResults.clear();
        for (int i = 0; i < n; ++i) {
            lastScanResults.push_back(WiFi.SSID(i));
        }
        WiFi.scanDelete();
        String json = "[";
        for (size_t i = 0; i < lastScanResults.size(); ++i) {
            if (i) json += ",";
            json += "\"" + lastScanResults[i] + "\"";
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    server.on("/forget", HTTP_GET, [](AsyncWebServerRequest *request){
        clearCreds();
        ssid = ""; password = "";
        WiFi.disconnect();
        state = State::PORTAL;
        request->send(200, "text/plain", "WiFi credentials cleared.");
    });

    server.on("/debug/forget", HTTP_GET, [](AsyncWebServerRequest *request){
        clearCreds();
        ssid = "";
        password = "";
        WiFi.disconnect(true);
        state = State::PORTAL;
        Serial.println("[DEBUG] WiFi credentials cleared via /debug/forget");
        request->send(200, "text/plain", "WiFi credentials cleared (debug).");
    });

    // ---- PATCHED: Proper POST JSON body for ESPAsyncWebServer (Arduino 3.2.0)
    server.on("/save", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
            String body = "";
            for (size_t i = 0; i < len; i++) body += (char)data[i];
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
            ssid = newSsid;
            password = newPass;
            state = State::CONNECTING;
            connectAttempts = 1;
            WiFi.begin(newSsid.c_str(), newPass.c_str());
            request->send(200, "text/plain", "Connecting to: " + newSsid);
            Serial.printf("[WiFiMgr] Received new creds. SSID: %s\n", newSsid.c_str());
        }
    );

    auto cp = [](AsyncWebServerRequest *r){
        r->send(200, "text/html", "<meta http-equiv='refresh' content='0; url=/' />");
    };
    server.on("/generate_204", HTTP_GET, cp);
    server.on("/hotspot-detect.html", HTTP_GET, cp);
    server.on("/redirect", HTTP_GET, cp);
    server.on("/ncsi.txt", HTTP_GET, cp);
    server.on("/captiveportal", HTTP_GET, cp);
    server.onNotFound(cp);

    server.begin();
    state = State::PORTAL;
}

void stopPortal() {
    dnsServer.stop();
}

void tryConnect() {
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

void begin() {
    LedStat::setStatus(LedStatus::Booting);
    loadCreds();
    startPortal();
    if (ssid.length() > 0)
        tryConnect();
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

void forgetWiFiFromSerial() {
    clearCreds();
    WiFi.disconnect(true);
    ssid = "";
    password = "";
    Serial.println("[SerialCmd] WiFi credentials forgotten.");
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
