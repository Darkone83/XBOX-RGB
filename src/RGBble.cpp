#include "RGBble.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "RGBCtrl.h"

namespace RGBble {

static const char* SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char* TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static BLEServer*         g_server    = nullptr;
static BLECharacteristic* g_rxChar    = nullptr;
static BLECharacteristic* g_txChar    = nullptr;
static bool               g_connected = false;
static bool               g_sending   = false;  // Guard against re-entrant sends

// RX chunk buffer for receiving large JSON payloads
static String             g_rxBuffer  = "";
static uint32_t           g_rxLastMs  = 0;
static const uint32_t     RX_TIMEOUT_MS = 1000; // 1 second timeout

// ---------- helpers ----------

static String mkOk(const char* op, const String* cfg = nullptr) {
  String out = String("{\"ok\":true,\"op\":\"") + op + "\"";
  if (cfg) {
    out += ",\"cfg\":";
    out += *cfg;
  }
  out += "}";
  return out;
}

static String mkErr(const char* op, const char* err) {
  String out = String("{\"ok\":false,\"op\":\"") + op + "\",\"err\":\"" + err + "\"}";
  return out;
}

static String macStr() {
  uint8_t m[6];
  WiFi.macAddress(m);
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(s);
}

static String buildDiscoverJson() {
  String out = String("{\"ok\":true,\"op\":\"discover\",\"name\":\"XBOX RGB\",")
             + "\"ver\":\"1.4.x\",\"transport\":\"ble\",\"mac\":\"" + macStr() + "\"}";
  return out;
}

static String handleJson(const char* data, size_t len) {
  Serial.print("[RGBble] handleJson len=");
  Serial.println(len);
  StaticJsonDocument<2048> doc; // Increased from 1536 to handle larger configs
  DeserializationError jerr = deserializeJson(doc, data, len);
  if (jerr) {
    Serial.print("[RGBble] JSON parse error: ");
    Serial.println(jerr.c_str());
    return mkErr("parse", "bad json");
  }

  const char* op = doc["op"] | "";
  if (!*op) {
    Serial.println("[RGBble] JSON missing 'op'");
    return mkErr("op", "missing op");
  }

  Serial.print("[RGBble] op=");
  Serial.println(op);

  if (!strcmp(op, "discover")) {
    Serial.println("[RGBble] op=discover");
    return buildDiscoverJson();
  } else if (!strcmp(op, "get")) {
    String cfg = RGBCtrl::getConfigJson();
    Serial.print("[RGBble] op=get cfg len=");
    Serial.println(cfg.length());
    Serial.print("[RGBble] op=get cfg JSON: ");
    Serial.println(cfg);
    return mkOk("get", &cfg);
  } else if (!strcmp(op, "preview")) {
    String js;
    if (doc.containsKey("cfg")) serializeJson(doc["cfg"], js);
    else                        serializeJson(doc, js);
    Serial.print("[RGBble] op=preview cfg=");
    Serial.println(js);

    bool ok = RGBCtrl::applyJsonPreview(js);
    Serial.print("[RGBble] applyJsonPreview=");
    Serial.println(ok ? "true" : "false");
    if (!ok) return mkErr("preview", "bad cfg");
    return mkOk("preview");
  } else if (!strcmp(op, "save")) {
    String js;
    if (doc.containsKey("cfg")) serializeJson(doc["cfg"], js);
    else                        serializeJson(doc, js);
    Serial.print("[RGBble] op=save cfg=");
    Serial.println(js);

    bool ok = RGBCtrl::applyJsonSave(js);
    Serial.print("[RGBble] applyJsonSave=");
    Serial.println(ok ? "true" : "false");
    if (!ok) return mkErr("save", "bad cfg");
    return mkOk("save");
  } else if (!strcmp(op, "reset")) {
    Serial.println("[RGBble] op=reset");
    RGBCtrl::resetToDefaults();
    return mkOk("reset");
  } else if (!strcmp(op, "setCounts")) {
    auto arr = doc["c"];
    if (!arr || arr.size() < 4) {
      Serial.println("[RGBble] op=setCounts invalid 'c'");
      return mkErr("setCounts", "need 4 ints");
    }
    uint16_t c0 = arr[0] | 0;
    uint16_t c1 = arr[1] | 0;
    uint16_t c2 = arr[2] | 0;
    uint16_t c3 = arr[3] | 0;
    Serial.printf("[RGBble] op=setCounts [%u,%u,%u,%u]\n", c0, c1, c2, c3);
    RGBCtrl::setCounts(c0, c1, c2, c3);
    return mkOk("setCounts");
  }

  Serial.println("[RGBble] unknown op");
  return mkErr("op", "unknown op");
}

static void sendReply(const String& s) {
  if (!g_txChar) {
    Serial.println("[RGBble] sendReply: no TX char");
    return;
  }
  if (!g_connected) {
    Serial.println("[RGBble] sendReply: not connected");
    return;
  }
  if (g_sending) {
    Serial.println("[RGBble] sendReply: BLOCKED - already sending");
    return;
  }
  
  g_sending = true;  // Set guard
  
  // BLE packet size limit - use very small chunks for maximum reliability
  const size_t MAX_CHUNK = 200; // Further reduced from 400 for safety with MTU issues
  size_t totalLen = s.length();
  
  Serial.print("[RGBble] sendReply total len=");
  Serial.println(totalLen);
  
  if (totalLen <= MAX_CHUNK) {
    // Send as single packet
    Serial.println("[RGBble] sendReply single packet");
    g_txChar->setValue(s.c_str());
    g_txChar->notify();
  } else {
    // Send in chunks using substring
    Serial.print("[RGBble] sendReply chunking into ");
    size_t numChunks = (totalLen + MAX_CHUNK - 1) / MAX_CHUNK;
    Serial.print(numChunks);
    Serial.println(" packets");
    
    for (size_t i = 0; i < numChunks; i++) {
      // Check if still connected before each chunk
      if (!g_connected) {
        Serial.print("[RGBble] Connection lost at chunk ");
        Serial.print(i + 1);
        Serial.print("/");
        Serial.println(numChunks);
        g_sending = false;
        return;
      }
      
      size_t offset = i * MAX_CHUNK;
      size_t remaining = totalLen - offset;
      size_t chunkSize = (remaining > MAX_CHUNK) ? MAX_CHUNK : remaining;
      
      String chunk = s.substring(offset, offset + chunkSize);
      
      Serial.print("[RGBble]   chunk ");
      Serial.print(i + 1);
      Serial.print("/");
      Serial.print(numChunks);
      Serial.print(": offset=");
      Serial.print(offset);
      Serial.print(" len=");
      Serial.print(chunk.length());
      Serial.println();
      
      g_txChar->setValue(chunk.c_str());
      g_txChar->notify();
      Serial.println("[RGBble]     notify() sent");
      
      // Delay between chunks - give receiver time to process
      if (i < numChunks - 1) {
        delay(150); // Increased to 150ms for more reliability
      }
    }
    
    Serial.println("[RGBble] sendReply chunking complete");
  }
  
  g_sending = false;  // Clear guard
}

// ---------- callbacks ----------

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    g_connected = true;
    g_rxBuffer = ""; // Clear buffer on new connection
    Serial.println("[RGBble] Central connected");
  }
  void onDisconnect(BLEServer* server) override {
    (void)server;
    g_connected = false;
    g_rxBuffer = ""; // Clear buffer on disconnect
    Serial.println("[RGBble] Central disconnected, restart advertising");
    BLEDevice::startAdvertising();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    String chunk = ch->getValue();
    if (!chunk.length()) {
      Serial.println("[RGBble] onWrite: empty");
      return;
    }
    
    uint32_t now = millis();
    
    // Check for timeout - if too much time passed, assume new message
    if (g_rxBuffer.length() > 0 && (now - g_rxLastMs) > RX_TIMEOUT_MS) {
      Serial.println("[RGBble] RX timeout, clearing buffer");
      g_rxBuffer = "";
    }
    
    g_rxLastMs = now;
    
    // Append chunk to buffer
    g_rxBuffer += chunk;
    
    Serial.print("[RGBble] onWrite chunk len=");
    Serial.print(chunk.length());
    Serial.print(", buffer total=");
    Serial.println(g_rxBuffer.length());
    
    // Try to parse buffer as complete JSON
    StaticJsonDocument<32> testDoc; // Small doc just for validation
    DeserializationError jerr = deserializeJson(testDoc, g_rxBuffer.c_str());
    
    if (!jerr) {
      // Valid complete JSON received!
      Serial.println("[RGBble] Complete JSON received, processing...");
      Serial.print("[RGBble] Full message: ");
      Serial.println(g_rxBuffer);
      
      String reply = handleJson(g_rxBuffer.c_str(), g_rxBuffer.length());
      g_rxBuffer = ""; // Clear buffer after processing
      
      if (reply.length()) {
        sendReply(reply);
      }
    } else {
      // Not complete yet, or buffer limit reached
      if (g_rxBuffer.length() > 4096) {
        Serial.println("[RGBble] Buffer overflow, clearing");
        g_rxBuffer = "";
      } else {
        Serial.println("[RGBble] Incomplete JSON, waiting for more chunks...");
      }
    }
  }
};

// ---------- public API ----------

void begin() {
  Serial.println("[RGBble] begin(): init BLE 'XBOX-RGB'");
  BLEDevice::init("XBOX-RGB");

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());
  Serial.println("[RGBble] createServer OK");

  BLEService* svc = g_server->createService(SERVICE_UUID);
  Serial.println("[RGBble] createService OK");

  g_txChar = svc->createCharacteristic(
      TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
  );
  g_txChar->addDescriptor(new BLE2902());
  Serial.println("[RGBble] TX char OK (NOTIFY + INDICATE)");

  g_rxChar = svc->createCharacteristic(
      RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  g_rxChar->setCallbacks(new RxCallbacks());
  Serial.println("[RGBble] RX char OK");

  svc->start();
  Serial.println("[RGBble] service started");
  
  // Request MTU size of 512 bytes for better throughput
  BLEDevice::setMTU(512);
  Serial.println("[RGBble] requested MTU=512");

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);

  BLEDevice::startAdvertising();
  Serial.println("[RGBble] advertising started");
}

void loop() {
  // nothing yet
}

} // namespace RGBble