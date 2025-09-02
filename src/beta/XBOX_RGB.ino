#include "wifimgr.h"
#include "led_stat.h"
#include "RGBCtrl.h"
#include "RGBsmbus.h"
#include "RGBudp.h"
#include  <ESPmDNS.h>

extern AsyncWebServer server;

void setup() {
  LedStat::begin();
  WiFiMgr::begin();
  RGBCtrl::begin({ /*ch1=*/2, /*ch2=*/3, /*ch3=*/4, /*ch4=*/5 });
  RGBCtrl::attachWeb("/config");
  RGBsmbus::begin({
    /*ch5=*/6,   // CPU bar
    /*ch6=*/7,   // Fan bar
    /*sda=*/8,    // XSDA
    /*scl=*/9     // XSCL
  }, 10, 10);
  RGBCtrlUDP::begin(7777 /*port*/, nullptr /*or "my_psk"*/);
}


void loop() {
  LedStat::loop();
  WiFiMgr::loop();
  RGBCtrl::loop();
  RGBsmbus::loop();
  RGBCtrlUDP::loop();

  static bool mdnsStarted = false;
  const bool connected = WiFiMgr::isConnected();

    if (connected && !mdnsStarted) {
        if (MDNS.begin("xboxrgb")) {
            Serial.println("[mDNS] Started: http://xboxrgb.local/");
            mdnsStarted = true;
        } else {
            Serial.println("[mDNS] mDNS start failed");
        }
    }

}
