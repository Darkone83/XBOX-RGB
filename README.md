<div align=center>
  <img src="https://github.com/Darkone83/XBOX-RGB/blob/main/images/DC%20logo.png">
</div>

# XBOX RGB 

<div align=center>
  <img src="https://github.com/Darkone83/XBOX-RGB/blob/main/images/3D%20Front.png" width=400><img src="https://github.com/Darkone83/XBOX-RGB/blob/main/images/3D%20Back.png" width=400>
</div>


A custom RGB controller that drives up to **6 WS2812/NeoPixel channels** for original Xbox RGB lighting — with a **captive-portal Wi-Fi setup**, **web control UI**, **OTA updates**, and optional **Xbox SMBus** telemetry bars for **CPU temperature (CH5)** and **fan speed (CH6)**.

- **CH1–CH4**: Main LED ring (up to **50 pixels per channel**).  
- **CH5**: CPU temperature bar (up to **10 pixels**).  
- **CH6**: Fan speed bar (up to **10 pixels**).  

The web UI previews changes live and saves them to flash (NVS).

---

## Features

- ✨ **12 animation modes** (see below) with live preview
- 🔆 Global **brightness** (1–255)
-  💡 **Power on fade in** slow but gentle boot fdade in
- ⚙️ Per-mode controls: **Speed**, **Intensity**, **Width/Gap**, **Color A / Color B**
- 🧭 **Sane orientation & sync**: effects start at **CH1 (Front)** and flow clockwise **CH1→CH2→CH3→CH4**; all “phase” effects are synchronized
- 🪩 **Adjustable Strip length** CH1 - 4 can support up to 50 LEDs configurable from the webui
- 🔀 **Reversible channel support** Adjust the direction of your channels if you installed them backwards
- 💾 **Persistent settings** in NVS (counts, mode, colors, brightness, toggles…)
- 📶 **Wi-Fi portal** (AP SSID `XBOX RGB Setup`) with a sticky SSID picker
- 🌐 **Web UI** at `/config`
- 🔁 **OTA updates** at `/ota`
- 🧪 **SMBus (Xbox SMC) telemetry** (optional, on CH5/CH6)
  - **CPU temperature** bar (green→yellow→red, max 75 °C)
  - **Fan percentage** bar (blue→yellow→orange)
- 🛜 **UDP control** UDP comminucation and control for XBMC4Gamers and the PC app

---

## Animation Modes

1. **Solid**  
2. **Breathe** *(synced across all channels)*  
3. **Color Wipe**  
4. **Larson Scanner (Cylon)**  
5. **Rainbow** *(synced progression around the ring)*  
6. **Theater Chase**  
7. **Twinkle**  
8. **Comet**  
9. **Meteor** *(Color A→Color B trail)*  
10. **Clock Spin**  
11. **Plasma**  
12. **Fire / Flicker**
13. **Palette Cycle**
14. **Palette Chase**

> The UI only shows controls that matter for the selected mode.  
> Example: *Meteor* uses **Color A** and **Color B**; *Breathe* ignores **width**.

---

## Purchase:

<a href="https://www.darkonecustoms.com/store/p/xbox-rgb">Darkone Custsoms</a>

---

## Hardware

## Purchasing sources

ESP32 S3 Zero: <a href="https://www.amazon.com/dp/B0D1CB3PBW?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_1">Amazon</a>, <a href="https://www.aliexpress.us/item/3256808233319699.html?spm=a2g0o.order_list.order_list_main.11.51cb1802dPO6b4&gatewayAdapt=glo2usa">Aliexpress</a>

JST Wire kit: <a href="https://www.amazon.com/dp/B0D5X6BY5Z?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_1">Amazon</a>

JST Socket kit: <a href="amazon.com/dp/B0CQ28CCQG?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_2">Amazon</a>

Additional wire may be needed to connect your harness to your LED strips.

## Hardware layout

Designed around **ESP32-S3-Zero** (or similar ESP32-S3 boards).

**Typical channel mapping** (adjust in code if needed):

- **CH1** → `IO1` (Front)  
- **CH2** → `IO2` (Left)  
- **CH3** → `IO3` (Rear)  
- **CH4** → `IO4` (Right)  
- **CH5** → `IO5` (CPU bar)  
- **CH6** → `IO6` (Fan bar)

**SMBus (Xbox SMC I²C):**

- **SDA** → `IO7`  
- **SCL** → `IO8`

> If a strip is reversed physically, flip that channel via the `REVERSE[]` flags in `RGBCtrl.cpp`, or the WebUi.

---

## Pinouts

CH1 - 3:

<img src="https://github.com/Darkone83/XBOX-RGB/blob/main/images/CH1-3.png">

Ch4 - 6:

<img src="https://github.com/Darkone83/XBOX-RGB/blob/main/images/CH4-6.png">

XSMB:

<img src="https://github.com/Darkone83/XBOX-RGB/blob/main/images/XSMB.png">

---

## Web UI

- Visit **`/config`** on the device to control animations.  
- **Live preview** applies instantly; **Save** writes to flash.  
- SMBus toggles (Enable CPU / Enable Fan) live under “Xbox SMBus LEDs”.

**Captive Portal:**  
On first boot (or after forgetting Wi-Fi) connect to AP **`XBOX RGB Setup`** → it redirects to the setup page.

**OTA:**  
Open **`/ota`**, pick your compiled `.bin`, upload, wait for reboot.

---

## Building / Compiling

### Prerequisites

- **Arduino IDE 2.x** (or PlatformIO)
- **ESP32 board support** (Arduino-ESP32 **3.x**)
- Libraries:
  - **ESP Async WebServer** (me-no-dev)
  - **AsyncTCP** (ESP32)
  - **Adafruit NeoPixel**
  - **ArduinoJson**
  - Core: **Preferences**, **DNSServer**, **Update**

> Ensure AsyncTCP matches the ESP32 core version (latest works with Arduino-ESP32 3.x).

### Board Settings (typical)

- **Board:** `ESP32S3 Dev Module` (or your S3 variant)  
- **USB CDC On Boot:** Enabled  
- **PSRAM:** Optional  
- **Partition Scheme:** Default (4 MB with spiffs is fine)
