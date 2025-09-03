# JSON Format for Custom Animations

This document explains how to define and upload **custom LED animations** using JSON with the RGBCtrl firmware.

---

## Top-Level Structure

A custom animation is expressed as a JSON object with a `playlist` array:

```json
{
  "mode": "custom",
  "playlist": [
    { "mode": 7, "duration": 2000, "colorA": "#0000FF", "width": 6 },
    { "mode": 0, "duration": 1500, "colorA": "#FF0000" }
  ],
  "loop": true
}
```

### Fields

- **mode**  
  Always `"custom"` for user-defined playlists.

- **playlist**  
  An array of **steps**. Each step is a JSON object containing parameters for a built-in effect.

- **loop**  
  Boolean (`true` or `false`). If true, playlist repeats.

---

## Step Object

Each step can include the following fields (depending on the chosen mode):

| Field       | Type    | Description |
|-------------|---------|-------------|
| `mode`      | int     | Effect ID (see table below). |
| `duration`  | int ms  | How long this step runs before moving on. |
| `speed`     | int 0–255 | Animation speed (if applicable). |
| `intensity` | int 0–255 | Effect strength (spark density, flicker, etc.). |
| `width`     | int     | Width or gap (for comet, chase, etc.). |
| `colorA`    | hex `#RRGGBB` | Primary color. |
| `colorB`    | hex `#RRGGBB` | Secondary color. |
| `colorC`    | hex `#RRGGBB` | Tertiary color (palette cycle/chase). |
| `colorD`    | hex `#RRGGBB` | Fourth color (palette cycle/chase). |
| `paletteCount` | int (1–4) | Number of active colors for palette modes. |

---

## Mode IDs

| ID | Name            |
|----|-----------------|
| 0  | Solid           |
| 1  | Breathe         |
| 2  | Color Wipe      |
| 3  | Larson          |
| 4  | Rainbow         |
| 5  | Theater Chase   |
| 6  | Twinkle         |
| 7  | Comet           |
| 8  | Meteor          |
| 9  | Clock Spin      |
| 10 | Plasma          |
| 11 | Fire / Flicker  |
| 12 | Palette Cycle   |
| 13 | Palette Chase   |

---

## Example: Multi-Step Custom Animation

```json
{
  "mode": "custom",
  "playlist": [
    { "mode": 7, "duration": 3000, "colorA": "#00FFFF", "width": 5 },
    { "mode": 11, "duration": 4000, "intensity": 180 },
    { "mode": 12, "duration": 5000, "paletteCount": 3, "colorA": "#FF0000", "colorB": "#00FF00", "colorC": "#0000FF" }
  ],
  "loop": true
}
```

This creates:
1. **Comet** effect (cyan tail, 3 seconds).  
2. **Fire/Flicker** effect (4 seconds).  
3. **Palette Cycle** with 3 colors (5 seconds).  
Then loops.

---

## Uploading JSON

1. Open the **Web UI → Config → Custom Mode**.  
2. Paste the JSON playlist into the editor (or build via the upcoming visual editor).  
3. Press **Preview** to test.  
4. Press **Save** to make persistent.

---

## Notes

- Any missing fields fall back to defaults.  
- If `loop` is false, LEDs stop after the last step.  
- Use short durations (500–2000ms) for snappy transitions, or longer (5000ms+) for ambient effects.  
