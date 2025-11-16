# XBOX-RGB BLE API

This document describes the Bluetooth Low Energy (BLE) API for controlling the **XBOX-RGB** firmware.  
BLE is treated as a JSON-over-UART transport that mirrors the existing UDP/web JSON API used by the firmware.

Device advertises as **`XBOX-RGB`**.

---

## 1. BLE GATT Layout

- **Device name:** `XBOX-RGB`
- **Service UUID (UART-style):** `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- **RX Characteristic UUID (Write / Write Without Response):** `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- **TX Characteristic UUID (Notify + Indicate):** `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

### Behavior

- Phone/app connects to the device `XBOX-RGB`.
- Discover the UART service and RX/TX characteristics.
- **All commands** are sent as UTF-8 JSON bytes via **RX**.
- **All responses** are sent as UTF-8 JSON via **TX notifications**.

### Packet Size & Chunking

BLE characteristics have a **512-byte MTU limit**, but effective payload is typically smaller. Both the client and device implement automatic chunking:

#### Client → Device (RX)
- JSON payloads **exceeding 200 bytes** are automatically split into chunks
- Each chunk is sent with a **50ms delay** between writes
- Device buffers incoming chunks and validates JSON completeness before processing
- **1-second timeout** between chunks triggers buffer reset

#### Device → Client (TX)
- Response payloads **exceeding 200 bytes** are automatically chunked
- Client must buffer incoming notifications until valid complete JSON is received
- Each chunk is sent with a **150ms delay** between notifications
- Chunks are **200 bytes maximum** for maximum compatibility with different MTU sizes

**Implementation note:** Both sides handle chunking transparently. Clients should:
1. Buffer incoming TX notifications
2. Set a 200ms timeout after last chunk
3. Attempt JSON parse when timeout fires
4. If parsing fails, extract first complete JSON object by brace-counting
5. Clear buffer after successful parse or if it exceeds ~5KB

**Critical:** The firmware uses BOTH `PROPERTY_NOTIFY` and `PROPERTY_INDICATE` on the TX characteristic for improved reliability during large transfers. The firmware also requests MTU=512 during initialization.

---

## 2. Common Response Structure

All responses share a common shape:

```json
{
  "ok": true,
  "op": "get",
  "...": "..."
}
```

- `ok` — `true` if the operation was successful, `false` otherwise.
- `op` — echoes the operation string of the request (e.g. `"get"`, `"preview"`, etc.).
- Additional fields are specific to each command.

Error example:

```json
{
  "ok": false,
  "op": "preview",
  "err": "bad cfg"
}
```

---

## 3. Config Object (`cfg`)

Most commands operate on a **config object** named `cfg`.  
This is the same structure used by the existing web/UDP API.

Important fields (non-exhaustive):

- `mode` — animation mode ID (integer, firmware-defined).
- `brightness` — overall brightness (0–255).
- `speed` — animation speed (0–255).
- `intensity` — animation intensity (0–255, effect depends on mode).
- `width` — pattern width (0–255, effect depends on mode).
- `count` — array of 4 integers: LED counts per channel [ch1, ch2, ch3, ch4].
- `paletteCount` — number of active palette colors (1–4).
- `colorA`, `colorB`, `colorC`, `colorD` — 32-bit RGB colors as integers (0xRRGGBB format).
- `masterOff` — `true` = LEDs off, `false` = LEDs enabled.
- `reverse` — array of 4 booleans controlling direction per channel.
- `enableCpu` — `true` = enable CPU temperature SMBus LED.
- `enableFan` — `true` = enable fan speed SMBus LED.
- `resumeOnBoot` — resume last effect on power-up if `true`.
- `kratosMode` — special passthrough mode (firmware-specific).
- `customSeq` — string-encoded JSON for custom playlists/sequences.
- `customLoop` — `true` to loop the custom sequence.

The exact set of fields may grow over time; the app should ignore unknown fields.

### Color Format

Colors are 24-bit RGB integers in the format `0xRRGGBB`:
- `colorA: 16711680` = `0xFF0000` = red
- `colorB: 65280` = `0x00FF00` = green
- `colorC: 255` = `0x0000FF` = blue

When converting from hex color strings (e.g. `"#FF0000"`):
```javascript
// JavaScript example
function hexToRgb24(hex) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return (r << 16) | (g << 8) | b;
}
```

---

## 4. Commands

### 4.1 `discover`

Query basic device info (name, version, transport, MAC).

**Request:**

```json
{"op": "discover"}
```

**Response:**

```json
{
  "ok": true,
  "op": "discover",
  "name": "XBOX RGB",
  "ver": "1.8.0 BLE Edition",
  "transport": "ble",
  "mac": "AA:BB:CC:DD:EE:FF"
}
```

Notes:

- `name` and `ver` are aligned with the existing UDP discover payload.
- `transport` is `"ble"` here to distinguish from the UDP path.

---

### 4.2 `get` — Read Current Config

Fetch the full current LED configuration.

**Request:**

```json
{"op": "get"}
```

**Response:**

```json
{
  "ok": true,
  "op": "get",
  "cfg": {
    "mode": 4,
    "brightness": 180,
    "speed": 128,
    "intensity": 128,
    "width": 4,
    "count": [50, 50, 50, 50],
    "paletteCount": 2,
    "colorA": 16711680,
    "colorB": 16752640,
    "colorC": 65280,
    "colorD": 255,
    "masterOff": false,
    "reverse": [true, false, false, true],
    "enableCpu": true,
    "enableFan": true,
    "resumeOnBoot": true,
    "kratosMode": false,
    "customSeq": "[]",
    "customLoop": true
  }
}
```

**Note:** This response may exceed 512 bytes and will be automatically chunked. Client must buffer notifications.

The app should store this `cfg` locally and use it to drive the UI.

---

### 4.3 `preview` — Apply Changes (No Save)

Apply a configuration to the LEDs **without saving** it to persistent storage.

The payload can either:

- Put fields directly under `cfg`, or
- Send a full `cfg` object.

#### Partial update example

```json
{
  "op": "preview",
  "cfg": {
    "mode": 5,
    "brightness": 220
  }
}
```

#### Full update example

```json
{
  "op": "preview",
  "cfg": {
    "mode": 5,
    "brightness": 220,
    "speed": 160,
    "intensity": 200,
    "width": 3,
    "count": [50, 50, 50, 50],
    "paletteCount": 3,
    "colorA": 16711680,
    "colorB": 16752640,
    "colorC": 255,
    "colorD": 65280,
    "masterOff": false,
    "reverse": [true, false, true, false],
    "enableCpu": true,
    "enableFan": true,
    "resumeOnBoot": true,
    "kratosMode": false,
    "customSeq": "[]",
    "customLoop": true
  }
}
```

**Note:** Large preview payloads (especially with `customSeq` data) will be automatically chunked by the client.

**Response:**

```json
{"ok": true, "op": "preview"}
```

Notes:

- If the config fails validation, you may get:

  ```json
  {"ok": false, "op": "preview", "err": "bad cfg"}
  ```

---

### 4.4 `save` — Apply & Persist

Apply a configuration and **save** it to persistent storage so it survives reboot.

Usage is identical to `preview`, but with `op: "save"`.

**Request:**

```json
{
  "op": "save",
  "cfg": {
    "mode": 4,
    "brightness": 200,
    "speed": 150,
    "intensity": 180,
    "count": [50, 50, 50, 50]
    // ...other fields as needed
  }
}
```

**Response:**

```json
{"ok": true, "op": "save"}
```

On error:

```json
{"ok": false, "op": "save", "err": "bad cfg"}
```

---

### 4.5 `reset` — Reset to Defaults

Reset all configuration to firmware defaults.

**Request:**

```json
{"op": "reset"}
```

**Response:**

```json
{"ok": true, "op": "reset"}
```

App should typically follow a successful reset with a `get` to refresh the UI:

```json
{"op": "get"}
```

---

### 4.6 `setCounts` — Per-channel LED Counts

Set the number of LEDs for each of the 4 channels.

**Request:**

```json
{
  "op": "setCounts",
  "c": [50, 50, 50, 50]
}
```

- `c[0]` — channel 1 count (Front)
- `c[1]` — channel 2 count (Left)
- `c[2]` — channel 3 count (Rear)
- `c[3]` — channel 4 count (Right)

**Response:**

```json
{"ok": true, "op": "setCounts"}
```

On error (e.g. wrong array length):

```json
{"ok": false, "op": "setCounts", "err": "need 4 ints"}
```

**Note:** This command is redundant with the `count` array in `cfg`. For consistency, prefer using the `count` field in `preview`/`save` operations.

---

## 5. Animation Modes

The firmware supports the following animation modes (referenced by `mode` field):

| ID | Name | Description |
|----|------|-------------|
| 0 | Solid | Single static color |
| 1 | Breathe | Fade in/out effect |
| 2 | Color Wipe | Progressive fill animation |
| 3 | Larson Scanner | Knight Rider style scanner |
| 4 | Rainbow | Classic rainbow cycle |
| 5 | Theater Chase | Marquee chase effect |
| 6 | Twinkle | Random sparkle effect |
| 7 | Comet | Comet tail animation |
| 8 | Meteor Shower | Multiple meteor tails |
| 9 | Clock Spin | Rotating clock hand |
| 10 | Plasma | Smooth plasma waves |
| 11 | Fire | Realistic fire simulation |
| 12 | Palette Cycle | Rotate through palette colors |
| 13 | Palette Chase | Chase blocks of palette colors |
| 14 | Custom Playlist | User-defined sequence (see `customSeq`) |
| 15 | UNSC vs Covenant | Halo-themed standoff animation |

---

## 6. Custom Playlist Format

When `mode: 14` (Custom Playlist), the `customSeq` field contains a JSON-encoded string representing an array of animation steps.

Each step is an object with the following fields:

```json
{
  "mode": 4,           // Animation mode for this step (0-13, NOT 14)
  "duration": 2000,    // Duration in milliseconds
  "speed": 128,        // Speed override for this step
  "intensity": 128,    // Intensity override
  "width": 4,          // Width override
  "paletteCount": 2,   // Palette count override
  "colorA": 16711680,  // Color overrides
  "colorB": 16752640,
  "colorC": 65280,
  "colorD": 255
}
```

### Example `customSeq`:

```json
"customSeq": "[{\"mode\":4,\"duration\":3000,\"speed\":150,\"intensity\":128,\"width\":4,\"paletteCount\":2,\"colorA\":16711680,\"colorB\":65280,\"colorC\":255,\"colorD\":16776960},{\"mode\":1,\"duration\":2000,\"speed\":100,\"intensity\":200,\"width\":4,\"paletteCount\":1,\"colorA\":255,\"colorB\":65280,\"colorC\":16711680,\"colorD\":16776960}]"
```

This would:
1. Play Rainbow (mode 4) for 3 seconds with specified colors
2. Play Breathe (mode 1) for 2 seconds with blue emphasis
3. Loop if `customLoop: true`

**Important:** The `customSeq` string must be valid JSON when parsed. Each step's `mode` must be 0-13 (not 14, to avoid recursion).

---

## 7. Recommended App Flow

### 7.1 Connect

1. Scan for BLE devices.
2. Show any device with the advertised name `XBOX-RGB`.
3. On user selection:
   - Connect over BLE.
   - Discover the UART-style service and characteristics.
   - Enable notifications on TX.
   - **Initialize RX buffer** for chunked response handling.
   - **Set timeout handler** (200ms) for completion detection.

### 7.2 Initialize State

On a successful connection:

1. **Clear any stale RX buffer**
2. Send `{"op":"discover"}` to identify the device.
3. Send `{"op":"get"}` to fetch the current config.
4. **Buffer incoming notifications** for up to 200ms after last chunk
5. **Extract first valid JSON** if buffer contains corruption/duplicates
6. Store the returned `cfg` and drive the UI from it.

### 7.3 Editing & Preview

When the user changes a control (e.g. brightness slider, mode dropdown, palette colors):

1. Update the local `cfg` model.
2. **Clear RX buffer** before sending
3. On user action (e.g. slider release or "Preview" button), send a `preview` packet:

   ```json
   {
     "op": "preview",
     "cfg": {
       "brightness": 220
     }
   }
   ```

   Or send the full `cfg` for complete updates.

4. **Client automatically chunks** if payload exceeds 200 bytes

Changes are applied live but not stored permanently.

### 7.4 Saving

When the user presses a "Save" button:

1. **Clear RX buffer** before sending
2. Send the full current `cfg`:

  ```json
  {
    "op": "save",
    "cfg": { ...full cfg object... }
  }
  ```

3. **May be chunked** automatically if large (e.g. with custom playlist)

This ensures the firmware has a complete picture, and the state persists across reboots.

### 7.5 Reset

Provide a "Reset to defaults" button that sends:

```json
{"op": "reset"}
```

On success, follow with `{"op":"get"}` to refresh the UI.

### 7.6 Channel Configuration

LED counts and direction can be configured via the `cfg` object:

```json
{
  "op": "preview",
  "cfg": {
    "count": [50, 50, 30, 10],
    "reverse": [true, false, true, false]
  }
}
```

- `count[0-3]`: Number of LEDs per channel (max 50 each)
- `reverse[0-3]`: Direction per channel (Front, Left, Rear, Right)

---

## 8. Notes & Best Practices

### Chunking Handling

- **Always buffer incoming TX notifications** until complete JSON is received
- **Use 200ms timeout** after last chunk to detect completion
- **Extract first valid JSON object** via brace-counting if parse fails
- Clear buffer after successful parse or if it exceeds ~5KB
- **Clear buffer before sending** new requests (get/preview/save)
- Implement 50ms delays between RX chunk writes when sending large payloads
- Firmware uses 200-byte chunks with 150ms delays for reliability

### Data Preservation

- Unknown fields in `cfg` should be preserved and passed through when possible
- Always assume the firmware may add new fields in later versions
- When building `cfg` for `save`, include ALL fields from the last `get` response

### Safety & UX

- **Preview first**, then let the user explicitly **Save**
- Handle error replies (`ok: false`) gracefully and display `err` messages to the user
- Show connection status and handle disconnects gracefully
- Consider rate-limiting preview updates (e.g. throttle slider changes)
- Use touch-friendly controls on mobile (44px minimum tap targets)

### Mode-Specific Behavior

- Only include `customSeq` when `mode: 14` to reduce packet size
- Validate `customSeq` JSON before sending to device
- Keep mode mapping (ID ↔ friendly name) in sync with firmware
- **Note:** `customSeq` field may cause issues with ArduinoJson double-escaping
  - See troubleshooting section for firmware patches

### Performance

- Use partial updates in `preview` when only a few fields change
- Full `cfg` updates are recommended for `save` operations
- Monitor BLE connection quality and implement reconnection logic
- **Expect chunking** for any response over 200 bytes
- Connection may drop during large transfers - firmware logs which chunk failed

### Mobile Optimization

- Disable user zoom with `maximum-scale=1,user-scalable=no` in viewport
- Use 44px minimum height for all interactive elements
- Disable webkit tap highlight for cleaner touch feedback
- Implement responsive grids (2-column mobile, 4-column desktop)
- Test on actual devices, not just browser dev tools

---

## 9. Reference Implementation

A **mobile-optimized web-based BLE client** is available (`webble.html`) that demonstrates:
- Chunked transmission and reception (200-byte chunks)
- Complete UI for all config fields with touch-friendly controls
- Custom playlist editor
- Proper buffering and error handling
- Smart JSON extraction to handle corrupted/duplicate packets
- Responsive design that works on phones, tablets, and desktop
- 200ms timeout-based completion detection

The web client is designed to work on **Chrome and Edge browsers** (desktop and mobile) with Web Bluetooth support.

---

## Appendix A: Troubleshooting

### Connection Drops During Large Transfers

**Cause:** BLE connection timing out or buffer overflow during multi-chunk transmissions.

**Solution:** 
- Firmware now uses 200-byte chunks (down from 400/500)
- 150ms delays between TX chunks
- Connection check before each chunk
- INDICATE property added for acknowledgments
- MTU=512 requested during init

### "Value can't exceed 512 bytes" Error

**Cause:** Attempting to write >512 bytes in a single BLE characteristic write.

**Solution:** Client-side chunking implemented:
1. Split JSON into ≤200 byte chunks
2. Write each chunk with 50ms delay
3. Device buffers and parses when complete

### Config Not Updating in UI

**Cause:** Response chunked but not buffered/parsed correctly on client.

**Solution:** 
1. Buffer all TX notifications
2. Set 200ms timeout after each chunk
3. When timeout fires, attempt JSON.parse()
4. If parse fails, extract first complete JSON object via brace-counting
5. Check Serial output for "notify() sent" to verify chunks transmitted

### Corrupted/Duplicate JSON in Buffer

**Cause:** BLE MTU fragmentation or timing issues causing chunks to overlap or duplicate.

**Solution:**
1. Web client now uses smart JSON extraction
2. Scans buffer for first complete `{...}` object
3. Attempts to parse extracted portion
4. Ignores trailing garbage/duplicates
5. Buffer cleared before each new request

### Parse Errors at Position ~370

**Cause:** ArduinoJson double-escaping `customSeq` field when it contains nested JSON.

**Solution:**
- Apply RGBCtrl patch to fix `customSeq` serialization
- See `RGBCtrl_customSeq_fix.patch` for proper implementation
- Or use `RGBCtrl_simple_fix.patch` to send empty array for BLE

---

## Version History

- **v1.8.0 BLE Edition** - Added chunking support, custom playlist mode, SMBus controls, channel direction
- **v1.4.x** - Initial BLE API implementation