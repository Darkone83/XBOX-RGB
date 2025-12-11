# XBOX-RGB Control (RXDK Dashboard App)

An original Xbox dashboard-style app for configuring the **Darkone Customs XBOX-RGB** LED controller directly from your console.  
Built with **RXDK**, rendered with Direct3D 8, and styled after the classic Xbox blade UI.

> **Note:** This README only covers the RXDK dashboard app. The firmware, Web UI, and other tools for XBOX-RGB live in their own projects.

---

## Features

- **Blade-style UI**
  - Five main blades:
    - **MODES** – Select animation mode and tune speed / intensity / width.
    - **BRIGHTNESS** – Global LED brightness control, with fine & coarse adjustment.
    - **CHANNELS** – Per-channel LED counts and direction (reverse flags).
    - **PALETTE** – Up to 4 stored palette slots with live orb-based color editing.
    - **SYSTEM** – Master power, resume-on-boot, telemetry indicators, Kratos mode.

- **Mode control**
  - Matches XBOX-RGB / RGBCtrl modes (by numeric ID):
    - `0`  SOLID  
    - `1`  BREATHE  
    - `2`  COLOR WIPE  
    - `3`  LARSON  
    - `4`  RAINBOW  
    - `5`  THEATER  
    - `6`  TWINKLE  
    - `7`  COMET  
    - `8`  METEOR  
    - `9`  CLOCK SPIN  
    - `10` PLASMA  
    - `11` FIRE  
    - `12` PALETTE CYCLE  
    - `13` PALETTE CHASE  
    - `14` CUSTOM  
    - `15` UNSC COVENANT  

- **Per-channel control**
  - 4 logical channels.
  - Per-channel LED counts (1 LED at a time, up to 1024).
  - Per-channel reverse toggles (ON/OFF).

- **Palette & orb editor**
  - Up to 4 palette slots.
  - Live orb preview in the center of the screen.
  - Left/right sticks adjust hue, saturation, and value for the active slot.
  - The orb color is what will be sent to the XBOX-RGB device for that slot.

- **System flags**
  - Master power on/off.
  - Resume-on-boot flag.
  - CPU / FAN indicator toggles.
  - Kratos mode toggle.

- **Save / load hooks**
  - **X** and **Y** are reserved for save/load.
  - `Menu_Update` exposes `ACT_SAVE` and `ACT_LOAD`; the app’s main code wires those to however you want to persist JSON configs on disk.

- **Device discovery**
  - **START** triggers a discover action (`ACT_DISCOVER`) from anywhere in the UI so the app can (re)locate your XBOX-RGB device.

---

## Controls

### Global

- **D-Pad Up / Down** – Move between blades (on the root) or rows (in submenus).
- **A**  
  - On root: enter the currently highlighted blade.  
  - In any submenu: trigger a **preview** of the current config (`ACT_PREVIEW`).
- **B**  
  - In submenus: go back to the root blade list.  
  - On root: exit back to the dashboard (`ACT_EXIT`).
- **X** – Request **save** (`ACT_SAVE`).
- **Y** – Request **load** (`ACT_LOAD`).
- **START** – Request **device discovery** (`ACT_DISCOVER`).

> All of these come out as `MenuAction` values from `Menu_Update`. The app’s `main.cpp` is responsible for taking those actions and talking to the XBOX-RGB device (network, SMBus bridge, etc.).

### MODES blade

- **Enter**: From root, select **MODES** and press **A** (state = `UI_SUB_MODES`).
- **Rows**:
  1. **Mode** (ID + human name)
  2. **Speed** (0–255)
  3. **Intensity** (0–255)
  4. **Width** (1–255)
- **Controls**:
  - **D-Pad Up / Down** – Move between rows.
  - **D-Pad Left / Right** – Adjust the selected value:
    - Mode: wraps through known IDs.
    - Speed: steps of 8.
    - Intensity: steps of 8.
    - Width: steps of 1.

### BRIGHTNESS blade

- **Enter**: From root, select **BRIGHTNESS** and press **A** (state = `UI_SUB_BRIGHTNESS`).
- **Value**: 0–255, displayed as a percentage + bar.
- **Controls**:
  - **Tap D-Pad Left / Right** – Fine adjust, ±1 step.
  - **Hold D-Pad Left / Right** – After a short delay, auto-repeat coarse steps of ±8 until released.
  - **A** – Preview.
  - **B** – Back to root.

### CHANNELS blade

- **Enter**: From root, select **CHANNELS** and press **A** (state = `UI_SUB_CHANNELS`).
- **Rows** (8 rows total):
  - `0–3`: `CH1–CH4 Count` (per-channel LED count)
  - `4–7`: `CH1–CH4 Reverse` (direction flag)
- **Controls**:
  - **D-Pad Up / Down** – Select row.
  - **D-Pad Left / Right**  
    - On rows 0–3: adjust LED **count** for that channel, **±1 LED per step**, clamped 0–1024.  
    - On rows 4–7: toggle `Reverse` **ON/OFF** for that channel.
  - **A** – Preview.
  - **B** – Back to root.

> Advanced mapping and per-pixel layout still live in the Web UI / firmware side – the RXDK app just pushes counts and reverse flags.

### PALETTE blade

- **Enter**: From root, select **PALETTE** and press **A** (state = `UI_SUB_PALETTE`).  
- **Rows**:
  1. Palette slot **count** (1–4)
  2. Current **palette slot** index (1..count)
- **Controls**:
  - **D-Pad Up / Down** – Choose between *count* and *slot selector* rows.
  - **D-Pad Left / Right**:
    - Row 0: change paletteSlot **count** (1–4).
    - Row 1: change **active slot** (0..count-1).
  - **Left Stick**:
    - X: adjust **hue** (wraps 0–360°).
    - Y: adjust **saturation** (0–1, up = more saturated).
  - **Right Stick**:
    - Y: adjust **value / brightness** (0–1, up = brighter).  
      When the stick is centered, the value tracks global brightness.
  - **A** – Preview.
  - **B** – Back to root.

The orb in the center of the screen shows the live HSV color. That color is converted to RGB and committed to the active palette slot:

- Slot 0 → `colorA`  
- Slot 1 → `colorB`  
- Slot 2 → `colorC`  
- Slot 3 → `colorD`  

These values are what the main app later sends down to the XBOX-RGB device.

### SYSTEM blade

- **Enter**: From root, select **SYSTEM** and press **A** (state = `UI_SUB_SYSTEM`).
- **Rows**:
  1. Master power (`masterOff`)
  2. Resume on boot (`resumeOn`)
  3. CPU indicator (`enableCpu`)
  4. FAN indicator (`enableFan`)
  5. Kratos mode (`kratosMode`)
- **Controls**:
  - **D-Pad Up / Down** – Select row.
  - **D-Pad Left / Right** – Toggle the current flag **ON/OFF**.
  - **A** – Preview.
  - **B** – Back to root.

---

## Building (RXDK)

This project is designed to be built with **RXDK** (the open-source original Xbox SDK replacement). Exact layout may differ depending on how you organize your projects, but the high-level steps are:

1. Install and configure **RXDK** on your build machine.
2. Clone this repository somewhere under your RXDK projects directory.
3. Build using your preferred RXDK build flow, examples:
   - `make`
   - or an IDE project that links against RXDK’s libraries.
4. The build should produce an Xbox executable (`default.xbe`).

> If you run into unresolved external symbols for `Menu_Init`, `Menu_Update`, or `GetMenuLabel`, make sure `menu.cpp` is compiled and linked into the final target, and that `menu.h` matches the function signatures used here.

---

## Installing on your Xbox

1. Copy the built `default.xbe` (and any required assets/configs) to your Xbox, e.g.:  
   `E:\Apps\XBOXRGB\default.xbe`
2. Add an entry to your dashboard (XBMC4Gamers, UnleashX, etc.) pointing at the app.
3. Boot the app from your dashboard.
4. Ensure your **XBOX-RGB** controller is powered and connected (network or wiring path as appropriate for your setup).
5. Press **START** to trigger discovery if the device isn’t found automatically.

You should see:

- Plasma background & glowing orb.
- Blade UI on the right.
- Left-side text legends showing current values and control hints.
- A status line at the top indicating whether the device is found.

---

## Integrating with your device / firmware

`Menu_Update` only handles **UI state**. The actual hardware I/O lives in your app’s main loop.

Typical flow in `main.cpp` (conceptually):

1. Poll controller state.
2. Call `Menu_Update(menuState, buttons, newly)` and handle the returned `MenuAction`:
   - `ACT_DISCOVER` – send out a discovery packet / probe for the XBOX-RGB device.
   - `ACT_PREVIEW` – push the current `MenuState` (mode, brightness, channels, palette, system flags) to the device as a live preview.
   - `ACT_SAVE` – serialize `MenuState` to JSON and save to disk.
   - `ACT_LOAD` – load JSON from disk into `MenuState` and push to the device.
   - `ACT_EXIT` – stop rendering and return to the dashboard.
3. Call `RenderFrame(frame, devFound, buttons, menuState)` each frame for visuals.

Exactly how you translate `MenuState` → wire/network packets is up to you and should match the XBOX-RGB firmware’s expectations.

---

## Troubleshooting

- **Black screen / no blades:**
  - Confirm Direct3D device creation is succeeding.
  - Ensure `InitD3D()` and `RenderFrame()` are both wired correctly in `main.cpp`.

- **Controls don’t respond:**
  - Confirm your controller polling code is correct and the button bitmasks match those used by `Menu_Update`.

- **Device never shows “FOUND”:**
  - Verify your discovery logic in `main.cpp`.
  - Confirm the XBOX-RGB device is powered, on the same network (if using UDP), and running compatible firmware.

- **JSON configs don’t save/load:**
  - Check that your `ACT_SAVE` / `ACT_LOAD` handlers are writing/reading the same schema that the XBOX-RGB firmware expects.

---

## Project status

This RXDK app is intended as a **companion** to the main XBOX-RGB firmware & Web UI.  
Some advanced features (per-pixel mapping, complex animations, sound-reactive modes, etc.) are expected to be configured on the firmware/Web side and are **not** exposed directly here.

---

## Credits

- **XBOX-RGB & RXDK app** – Darkone Customs / Team Resurgent (Darkone83)  
- Built with **RXDK** and love for the OG Xbox modding scene.\
