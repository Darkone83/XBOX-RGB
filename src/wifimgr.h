#pragma once

#include <Arduino.h>

namespace WiFiMgr {

    //AsyncWebServer& getServer();

    void begin();
    void loop();
    void restartPortal();
    void forgetWiFi();
    bool isConnected();
    String getStatus();
}
