# -*- coding: utf-8 -*-
# XBMC4Gamers RGB Controller – full UI, UDP (default) + HTTP fallback
# Works with RGBCtrl firmware JSON contract (preview/save/reset/get).
#
# Place as default.py in your XBMC script dir. Set DEVICE_IP below.

import sys, socket, time

# ---------- CONFIG ----------
DEVICE_IP   = "192.168.1.123"   # <-- set your ESP32 IP
UDP_PORT    = 7777              # <-- set to your firmware's UDP port
HTTP_BASE   = "http://%s/config/api" % DEVICE_IP
USE_UDP     = True              # toggle transport here if needed

# ---------- Imports (Python 2.4+ on XBMC) ----------
try:
    import json
except:
    import simplejson as json

import xbmc
import xbmcgui

# ---------- Constants / Mappings ----------
MODES = [
    ("Solid", 0),
    ("Breathe", 1),
    ("Color Wipe", 2),
    ("Larson", 3),
    ("Rainbow", 4),
    ("Theater Chase", 5),
    ("Twinkle", 6),
    ("Comet", 7),
    ("Meteor", 8),
    ("Clock Spin", 9),
    ("Plasma", 10),
    ("Fire / Flicker", 11),
    ("Palette Cycle", 12),
    ("Palette Chase", 13),
]

# 16 preset colors (name, 0xRRGGBB)
PRESET_COLORS = [
    ("Red",        0xFF0000),
    ("Orange",     0xFFA000),
    ("Amber",      0xFF7E00),
    ("Yellow",     0xFFFF00),
    ("Lime",       0x00FF00),
    ("Mint",       0x98FF98),
    ("Cyan",       0x00FFFF),
    ("Ice",        0xA0E6FF),
    ("Sky",        0x00A4FF),
    ("Blue",       0x0000FF),
    ("Purple",     0x8000FF),
    ("Magenta",    0xFF00FF),
    ("Pink",       0xFF4081),
    ("Gold",       0xFFD700),
    ("Warm White", 0xFFD6A5),
    ("Cool White", 0xD0E0FF),
]
COLOR_PICK_EXTRA = "Custom HEX…"

# Default config mirrors firmware fields
def default_cfg():
    return {
        "count": [50,50,50,50],
        "brightness": 180,
        "mode": 4,
        "speed": 128,
        "intensity": 128,
        "width": 4,
        "colorA": 0xFF0000,
        "colorB": 0xFFA000,
        "colorC": 0x00FF00,
        "colorD": 0x0000FF,
        "paletteCount": 2,
        "resumeOnBoot": True,
        "enableCpu": True,
        "enableFan": True
    }

# ---------- Transport helpers ----------
def _udp_send_recv(payload, expect_reply=True, timeout=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.settimeout(timeout)
        data = json.dumps(payload)
        s.sendto(data, (DEVICE_IP, UDP_PORT))
        if expect_reply:
            resp, _ = s.recvfrom(8192)
            return resp
        return None
    finally:
        s.close()

def _http_get(path):
    try:
        import urllib2
        req = urllib2.Request(HTTP_BASE + path)
        f = urllib2.urlopen(req, timeout=2.5)
        out = f.read()
        f.close()
        return out
    except Exception as e:
        xbmc.log("HTTP GET failed: %s" % e)
        return None

def _http_post(path, body_dict):
    try:
        import urllib2
        body = json.dumps(body_dict)
        req = urllib2.Request(HTTP_BASE + path, data=body, headers={'Content-Type':'application/json'})
        f = urllib2.urlopen(req, timeout=2.5)
        out = f.read()
        f.close()
        return out
    except Exception as e:
        xbmc.log("HTTP POST failed: %s" % e)
        return None

# ---------- Device API ----------
def get_config():
    if USE_UDP:
        try:
            resp = _udp_send_recv({"op":"get"}, expect_reply=True)
            if resp:
                return json.loads(resp)
        except Exception as e:
            xbmc.log("UDP get_config failed: %s" % e)
    # HTTP fallback
    resp = _http_get("/ledconfig")
    if resp:
        try: return json.loads(resp)
        except: pass
    return default_cfg()

def send_preview(cfg):
    if USE_UDP:
        try:
            _udp_send_recv({"op":"preview", "cfg": cfg}, expect_reply=False)
            return True
        except Exception as e:
            xbmc.log("UDP preview failed: %s" % e)
    # HTTP fallback
    out = _http_post("/ledpreview", cfg)
    return bool(out)

def send_save(cfg):
    if USE_UDP:
        try:
            _udp_send_recv({"op":"save", "cfg": cfg}, expect_reply=False)
            return True
        except Exception as e:
            xbmc.log("UDP save failed: %s" % e)
    out = _http_post("/ledsave", cfg)
    return bool(out)

def send_reset():
    if USE_UDP:
        try:
            _udp_send_recv({"op":"reset"}, expect_reply=False)
            return True
        except Exception as e:
            xbmc.log("UDP reset failed: %s" % e)
    out = _http_post("/ledreset", {})
    return bool(out)

# ---------- UI Helpers ----------
D = xbmcgui.Dialog()

def pick_mode(cur):
    names = [n for (n, v) in MODES]
    idx = 0
    for i, it in enumerate(MODES):
        if it[1] == cur: idx = i; break
    sel = D.select("Select Mode", names)
    if sel < 0: return cur
    return MODES[sel][1]

def pick_int(title, cur, lo, hi):
    # Numeric dialog (XBMC classic): use keyboard numeric as fallback
    try:
        # Newer: D.numeric(0, title, str(cur)) – not always in old builds
        pass
    except:
        pass
    kb = xbmc.Keyboard(str(cur), title)
    kb.doModal()
    if kb.isConfirmed():
        try:
            v = int(kb.getText())
            if v < lo: v = lo
            if v > hi: v = hi
            return v
        except:
            return cur
    return cur

def _format_hex(v):
    s = "%06X" % (int(v) & 0xFFFFFF)
    return s

def pick_color(title, cur_val):
    items = ["%s  (#%06X)" % (name, val) for (name,val) in PRESET_COLORS]
    items.append(COLOR_PICK_EXTRA)
    # Preselect closest match
    sel = -1
    for i,(n,v) in enumerate(PRESET_COLORS):
        if v == (cur_val & 0xFFFFFF): sel = i; break
    if sel < 0: sel = 0
    choice = D.select(title, items)
    if choice < 0:
        return cur_val
    if choice == len(items)-1:
        # Custom HEX
        kb = xbmc.Keyboard(_format_hex(cur_val), "Enter HEX (RRGGBB)")
        kb.doModal()
        if kb.isConfirmed():
            txt = kb.getText().strip().lstrip("#")
            if len(txt) == 6:
                try: return int(txt, 16) & 0xFFFFFF
                except: pass
        return cur_val
    else:
        return PRESET_COLORS[choice][1]

def pick_bool(title, cur):
    return D.yesno("Toggle", "%s\n\nCurrently: %s" % (title, "On" if cur else "Off"), yeslabel="On", nolabel="Off")

def pick_palette_count(cur):
    sel = D.select("Palette Size", ["1 color","2 colors","3 colors","4 colors"])
    if sel < 0: return cur
    return sel+1

def quick_toast(msg):
    xbmcgui.Dialog().notification("RGB Controller", msg, time=1500)

# ---------- Main UI ----------
def format_cfg_summary(cfg):
    return (
        "Mode: %s\n"
        "Brightness: %d   Speed: %d\n"
        "Intensity: %d    Width: %d\n"
        "Palette: %d colors\n"
        "A:#%06X  B:#%06X  C:#%06X  D:#%06X\n"
        "Counts: CH1=%d CH2=%d CH3=%d CH4=%d\n"
        "Resume: %s   SMBus CPU:%s  Fan:%s\n"
        "Transport: %s"
    ) % (
        [n for (n,v) in MODES if v==cfg["mode"]][0],
        cfg["brightness"], cfg["speed"],
        cfg["intensity"], cfg["width"],
        cfg["paletteCount"],
        cfg["colorA"], cfg["colorB"], cfg.get("colorC",0), cfg.get("colorD",0),
        cfg["count"][0], cfg["count"][1], cfg["count"][2], cfg["count"][3],
        "Yes" if cfg["resumeOnBoot"] else "No",
        "On" if cfg["enableCpu"] else "Off",
        "On" if cfg["enableFan"] else "Off",
        "UDP" if USE_UDP else "HTTP"
    )

def main_menu():
    global USE_UDP
    cfg = get_config()

    # Live preview on load
    send_preview(cfg)

    while True:
        choice = D.select(
            "RGB Controller – %s" % DEVICE_IP,
            [
                "Mode: %s" % [n for (n,v) in MODES if v==cfg["mode"]][0],
                "Brightness: %d" % cfg["brightness"],
                "Speed: %d" % cfg["speed"],
                "Intensity: %d" % cfg["intensity"],
                "Width: %d" % cfg["width"],
                "Palette Size: %d" % cfg["paletteCount"],
                "Color A: #%06X" % cfg["colorA"],
                "Color B: #%06X" % cfg["colorB"],
                "Color C: #%06X" % cfg.get("colorC",0),
                "Color D: #%06X" % cfg.get("colorD",0),
                "Counts (CH1..CH4): %d/%d/%d/%d" % (cfg["count"][0],cfg["count"][1],cfg["count"][2],cfg["count"][3]),
                "Resume on Boot: %s" % ("Yes" if cfg["resumeOnBoot"] else "No"),
                "SMBus CPU LEDs: %s" % ("On" if cfg["enableCpu"] else "Off"),
                "SMBus Fan LEDs: %s" % ("On" if cfg["enableFan"] else "Off"),
                "Transport: %s" % ("UDP" if USE_UDP else "HTTP"),
                "Preview Now",
                "Save",
                "Reset Defaults",
                "Show Summary",
                "Exit"
            ]
        )
        if choice < 0 or choice == 19:
            break

        changed = False

        if choice == 0:
            cfg["mode"] = pick_mode(cfg["mode"]); changed = True
        elif choice == 1:
            cfg["brightness"] = pick_int("Brightness (1..255)", cfg["brightness"], 1, 255); changed = True
        elif choice == 2:
            cfg["speed"] = pick_int("Speed (0..255)", cfg["speed"], 0, 255); changed = True
        elif choice == 3:
            cfg["intensity"] = pick_int("Intensity (0..255)", cfg["intensity"], 0, 255); changed = True
        elif choice == 4:
            cfg["width"] = pick_int("Width (1..20)", cfg["width"], 1, 20); changed = True
        elif choice == 5:
            cfg["paletteCount"] = pick_palette_count(cfg["paletteCount"]); changed = True
        elif choice == 6:
            cfg["colorA"] = pick_color("Pick Color A", cfg["colorA"]); changed = True
        elif choice == 7:
            cfg["colorB"] = pick_color("Pick Color B", cfg["colorB"]); changed = True
        elif choice == 8:
            cfg["colorC"] = pick_color("Pick Color C", cfg.get("colorC",0)); changed = True
        elif choice == 9:
            cfg["colorD"] = pick_color("Pick Color D", cfg.get("colorD",0)); changed = True
        elif choice == 10:
            ch1 = pick_int("CH1 count (0..50)", cfg["count"][0], 0, 50)
            ch2 = pick_int("CH2 count (0..50)", cfg["count"][1], 0, 50)
            ch3 = pick_int("CH3 count (0..50)", cfg["count"][2], 0, 50)
            ch4 = pick_int("CH4 count (0..50)", cfg["count"][3], 0, 50)
            cfg["count"] = [ch1,ch2,ch3,ch4]; changed = True
        elif choice == 11:
            cfg["resumeOnBoot"] = pick_bool("Resume last mode on boot?", cfg["resumeOnBoot"]); changed = True
        elif choice == 12:
            cfg["enableCpu"] = pick_bool("Enable CPU SMBus LEDs (CH5)?", cfg["enableCpu"]); changed = True
        elif choice == 13:
            cfg["enableFan"] = pick_bool("Enable Fan SMBus LEDs (CH6)?", cfg["enableFan"]); changed = True
        elif choice == 14:
            USE_UDP = not USE_UDP
            quick_toast("Transport: %s" % ("UDP" if USE_UDP else "HTTP"))
        elif choice == 15:  # Preview Now
            ok = send_preview(cfg)
            quick_toast("Preview %s" % ("OK" if ok else "Failed"))
        elif choice == 16:  # Save
            ok = send_save(cfg)
            quick_toast("Saved" if ok else "Save failed")
        elif choice == 17:  # Reset
            if D.yesno("Reset", "Reset to defaults?", yeslabel="Reset", nolabel="Cancel"):
                ok = send_reset()
                if ok:
                    cfg = get_config()
                    quick_toast("Defaults applied")
                else:
                    quick_toast("Reset failed")
        elif choice == 18:  # Summary
            D.textviewer("Current Settings", format_cfg_summary(cfg))

        if changed:
            # Live preview on any change
            send_preview(cfg)

if __name__ == "__main__":
    try:
        main_menu()
    except Exception as e:
        xbmc.log("RGB Script error: %s" % e)
        D.ok("RGB Controller", "Error:\n%s" % e)
