# xboxrgb_sim.py
# XBOX RGB Simulator – updated for RGBCtrl v1.6.x
# New:
#   - masterOff support (global blackout)
#   - Custom (Playlist) mode = 14 with customLoop + customSeq parsing & playback
#
# Layout:
#   CH1 = Front  (BOTTOM edge)
#   CH2 = Left   (LEFT edge)
#   CH3 = Rear   (TOP edge)
#   CH4 = Right  (RIGHT edge)
# Ring order: CH1 -> CH2 -> CH3 -> CH4 (indices follow a continuous loop).
#
# Protocol:
#   Broadcast discover probes:
#     - JSON {"op":"discover"}  → reply JSON {"op":"discover", "ip","port","name","ver","mac"}
#     - Text "RGBDISC?"         → reply "RGBDISC! {JSON...}"
#   UDP ops:
#     - {"op":"get"}                    → reply {"ok":true,"op":"get","cfg":{...}}
#     - {"op":"preview","cfg":{...}}    → apply live (no reply)
#     - {"op":"save","cfg":{...}}       → apply live (no reply)
#     - {"op":"reset"}                  → reply {"ok":true,"op":"reset"} and reset defaults
#
# Requires: PySide6  (pip install PySide6)

import sys, json, socket, time, math, random
from typing import List, Dict, Any
from PySide6.QtCore import Qt, QTimer, QPointF, QRectF
from PySide6.QtGui  import QPainter, QColor, QPen, QBrush, QFont
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout, QLabel

UDP_PORT  = 7777
NAME      = "XBOX RGB"
VER       = "1.6.sim"
COPYRIGHT = "© Darkone Customs 2025"
BUILD_VERSION = "1.6.0"

def clamp(v, lo, hi): return lo if v < lo else hi if v > hi else v

def hsv2rgb(h, s, v):
    i = int(h*6.0)
    f = h*6.0 - i
    p = int(v*(1.0-s)*255)
    q = int(v*(1.0-f*s)*255)
    t = int(v*(1.0-(1.0-f)*s)*255)
    v = int(v*255)
    i %= 6
    if i==0: return (v,t,p)
    if i==1: return (q,v,p)
    if i==2: return (p,v,t)
    if i==3: return (p,q,v)
    if i==4: return (t,p,v)
    return (v,p,q)

def wheel(pos):
    pos &= 255
    if pos < 85:  return (255 - pos*3, pos*3, 0)
    if pos < 170:
        pos -= 85; return (0, 255 - pos*3, pos*3)
    pos -= 170;   return (pos*3, 0, 255 - pos*3)

def local_ip_guess() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

# -------------------- Simulator core --------------------
class RGBSim(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("XBOX RGB Simulator")
        self.setMinimumSize(1000, 600)

        self.info = QLabel("UDP: listening on 0.0.0.0:7777  •  Discovery ready")
        self.info.setAlignment(Qt.AlignLeft)

        lay = QVBoxLayout(self)
        lay.setContentsMargins(12,12,12,12)
        self.canvas = Canvas()
        lay.addWidget(self.canvas, 1)
        foot = QLabel(COPYRIGHT); foot.setAlignment(Qt.AlignCenter)
        lay.addWidget(foot)
        lay.addWidget(self.info)

        # UDP server
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try: self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except Exception: pass
        self.sock.bind(("0.0.0.0", UDP_PORT))
        self.sock.setblocking(False)

        # default config mirrors firmware (incl. reverse flags)
        self.cfg: Dict[str, Any] = {
            "count": [8,12,12,12],       # CH1,CH2,CH3,CH4
            "brightness": 180,
            "mode": 4,                   # Rainbow default
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
            "enableFan": True,
            "reverse": [True, False, False, True],  # matches RGBCtrl default REVERSE
            # New 1.6.x:
            "masterOff": False,
            "customLoop": True,
            "customSeq": "[]",
            "copyright": COPYRIGHT,
            "buildVersion": BUILD_VERSION,
        }
        self.apply_cfg(self.cfg, initial=True)

        # Custom playlist runtime state
        self._cust_active = False
        self._cust_seq: List[Dict[str, Any]] = []
        self._cust_loop = True
        self._cust_idx = 0
        self._cust_step_started = time.time()

        # timers
        self.anim = QTimer(self); self.anim.timeout.connect(self.tick_anim); self.anim.start(16)
        self.poll = QTimer(self); self.poll.timeout.connect(self.poll_udp); self.poll.start(20)
        self.ticks = 0

    # ---------- UDP ----------
    def poll_udp(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(4096)
            except BlockingIOError:
                break
            except Exception:
                break
            txt = data.decode("utf-8", errors="ignore").strip()
            rip, rport = addr

            # Text discovery
            if txt.startswith("RGBDISC?"):
                reply = {"op":"discover","ip":local_ip_guess(),"port":UDP_PORT,"name":NAME,"ver":VER,"mac":"sim"}
                out = ("RGBDISC! " + json.dumps(reply)).encode("utf-8")
                self.sock.sendto(out, addr)
                continue

            # JSON paths
            try: j = json.loads(txt)
            except Exception: continue

            op = j.get("op","")
            if op == "discover":
                reply = {"op":"discover","ip":local_ip_guess(),"port":UDP_PORT,"name":NAME,"ver":VER,"mac":"sim"}
                self.sock.sendto(json.dumps(reply).encode("utf-8"), addr)

            elif op == "get":
                payload = {"ok":True,"op":"get","cfg": self.current_cfg_for_reply()}
                self.sock.sendto(json.dumps(payload).encode("utf-8"), addr)

            elif op in ("preview","save"):
                cfg = j.get("cfg", {})
                self.apply_cfg(cfg)

            elif op == "reset":
                self.apply_cfg({
                    "count":[8,12,12,12],
                    "brightness":180,"mode":4,"speed":128,
                    "intensity":128,"width":4,
                    "colorA":0xFF0000,"colorB":0xFFA000,"colorC":0x00FF00,"colorD":0x0000FF,
                    "paletteCount":2,"resumeOnBoot":True,"enableFan":True,"enableCpu":True,
                    "reverse":[True,False,False,True],
                    "masterOff": False,
                    "customLoop": True,
                    "customSeq": "[]",
                })
                self.sock.sendto(json.dumps({"ok":True,"op":"reset"}).encode("utf-8"), addr)

    def current_cfg_for_reply(self):
        c = dict(self.cfg)
        c["copyright"] = COPYRIGHT
        c["buildVersion"] = BUILD_VERSION
        return c

    # ---------- Config / animation ----------
    def _parse_custom_seq(self, seq_raw) -> List[Dict[str, Any]]:
        try:
            if isinstance(seq_raw, str):
                arr = json.loads(seq_raw)
            else:
                arr = seq_raw
            if not isinstance(arr, list): return []
            out = []
            for step in arr:
                if not isinstance(step, dict): continue
                # sanitize
                d = {
                    "mode": int(clamp(step.get("mode", 0), 0, 13)),
                    "duration": int(clamp(step.get("duration", 1000), 1, 60000)),
                }
                # optional overrides
                for k in ("speed","intensity","width","paletteCount","colorA","colorB","colorC","colorD"):
                    if k in step: d[k] = step[k]
                out.append(d)
            return out
        except Exception:
            return []

    def apply_cfg(self, cfg, initial=False):
        # Merge
        c = self.cfg
        if "count" in cfg:
            v = cfg.get("count",[8,12,12,12])
            c["count"] = [clamp(int(v[0]),0,50), clamp(int(v[1]),0,50),
                          clamp(int(v[2]),0,50), clamp(int(v[3]),0,50)]
            self.canvas.set_counts(c["count"])

        for k in ("brightness","mode","speed","intensity","width",
                  "colorA","colorB","colorC","colorD","paletteCount",
                  "resumeOnBoot","enableCpu","enableFan","masterOff"):
            if k in cfg: c[k] = cfg[k]

        # per-channel reverse
        if "reverse" in cfg and isinstance(cfg["reverse"], (list,tuple)) and len(cfg["reverse"])>=4:
            c["reverse"] = [bool(cfg["reverse"][i]) for i in range(4)]
            self.canvas.set_reverse(c["reverse"])

        # Custom playlist related
        if "customLoop" in cfg: c["customLoop"] = bool(cfg["customLoop"])
        if "customSeq" in cfg:  c["customSeq"]  = cfg["customSeq"]

        # Meta (optional)
        if "buildVersion" in cfg: c["buildVersion"] = cfg["buildVersion"]

        # Base (non-custom) timing/params
        spd = clamp(int(c["speed"]),0,255)
        self.canvas.base_frame_ms = max(10, 10 + (255 - spd)//2)
        self.canvas.brightness = clamp(int(c["brightness"]),1,255)/255.0
        self.canvas.colors = [c["colorA"], c["colorB"], c["colorC"], c["colorD"]]
        self.canvas.palette_count = int(clamp(c.get("paletteCount",2),1,4))
        self.canvas.master_off = bool(c.get("masterOff", False))

        # Custom playlist activation & parsing
        if int(c["mode"]) == 14:
            self._cust_seq  = self._parse_custom_seq(c.get("customSeq","[]"))
            self._cust_loop = bool(c.get("customLoop", True))
            self._cust_active = len(self._cust_seq) > 0
            self._cust_idx = 0
            self._cust_step_started = time.time()
        else:
            self._cust_active = False
            self.canvas.mode = int(c["mode"])
            self.canvas.width = int(clamp(c.get("width",4),1,255))
            self.canvas.intensity = int(clamp(c.get("intensity",128),0,255))
            self.canvas.frame_ms = self.canvas.base_frame_ms

        if not initial: self.canvas.invalidate_all()

    def _effective_from_step(self, base: Dict[str, Any], step: Dict[str, Any]) -> Dict[str, Any]:
        eff = dict(base)
        # Apply overrides from step
        eff["mode"] = int(step.get("mode", eff["mode"]))
        for k in ("speed","intensity","width","paletteCount","colorA","colorB","colorC","colorD"):
            if k in step: eff[k] = step[k]
        # Clamp common fields
        eff["speed"] = int(clamp(eff.get("speed",128), 0, 255))
        eff["intensity"] = int(clamp(eff.get("intensity",128), 0, 255))
        eff["width"] = int(clamp(eff.get("width",4), 1, 255))
        eff["paletteCount"] = int(clamp(eff.get("paletteCount",2), 1, 4))
        return eff

    def tick_anim(self):
        self.ticks += 1

        # Handle custom playlist playback
        if self._cust_active and self._cust_seq:
            now = time.time()
            step = self._cust_seq[self._cust_idx]
            dur_ms = int(step.get("duration", 1000))
            if (now - self._cust_step_started) * 1000.0 >= dur_ms:
                # advance
                if self._cust_idx + 1 < len(self._cust_seq):
                    self._cust_idx += 1
                    self._cust_step_started = now
                else:
                    if self._cust_loop:
                        self._cust_idx = 0
                        self._cust_step_started = now
                    else:
                        # Hold last frame/effect; keep active but no advance
                        self._cust_step_started = now  # stop drift
            step = self._cust_seq[self._cust_idx]

            # Build effective settings from base cfg + step overrides
            base = {
                "mode": 0,
                "speed": self.cfg.get("speed",128),
                "intensity": self.cfg.get("intensity",128),
                "width": self.cfg.get("width",4),
                "paletteCount": self.cfg.get("paletteCount",2),
                "colorA": self.cfg.get("colorA",0xFF0000),
                "colorB": self.cfg.get("colorB",0xFFA000),
                "colorC": self.cfg.get("colorC",0x00FF00),
                "colorD": self.cfg.get("colorD",0x0000FF),
            }
            eff = self._effective_from_step(base, step)

            # Push effective params into canvas
            self.canvas.mode = int(eff["mode"])
            self.canvas.width = eff["width"]
            self.canvas.intensity = eff["intensity"]
            self.canvas.palette_count = eff["paletteCount"]
            self.canvas.colors = [eff["colorA"], eff["colorB"], eff["colorC"], eff["colorD"]]
            # Adjust frame time by effective speed
            self.canvas.frame_ms = max(10, 10 + (255 - eff["speed"])//2)
            self.canvas.master_off = bool(self.cfg.get("masterOff", False))
        else:
            # Non-custom: keep using base params
            self.canvas.frame_ms = getattr(self.canvas, "base_frame_ms", 16)

        self.canvas.advance()

# -------------------- Drawing / effects --------------------
class Canvas(QWidget):
    def __init__(self):
        super().__init__()
        self.setMinimumSize(900, 420)
        self.counts = [8,12,12,12]   # CH1,CH2,CH3,CH4
        self.reverse = [True, False, False, True]  # match device defaults
        self.points: List[QPointF] = []
        self.ring_len = sum(self.counts)
        self.pix = [(0,0,0)] * self.ring_len

        # Parameters updated from sim
        self.mode = 4
        self.colors = [0xFF0000,0xFFA000,0x00FF00,0x0000FF]
        self.palette_count = 2
        self.intensity = 128
        self.width = 4
        self.brightness = 180/255.0
        self.master_off = False

        # Timing
        self.base_frame_ms = 16
        self.frame_ms = 16
        self._accum = 0
        self._tick  = 0
        self.rebuild_points()

        self.timer = QTimer(self)
        self.timer.timeout.connect(self._frame)
        self.timer.start(16)

    def set_counts(self, counts):
        self.counts = [int(counts[0]),int(counts[1]),int(counts[2]),int(counts[3])]
        self.ring_len = sum(self.counts)
        self.pix = [(0,0,0)] * self.ring_len
        self.rebuild_points()
        self.update()

    def set_reverse(self, rev_flags):
        self.reverse = [bool(rev_flags[0]), bool(rev_flags[1]), bool(rev_flags[2]), bool(rev_flags[3])]
        self.update()

    def sizeHint(self): return self.minimumSize()

    def _rect_and_metrics(self):
        full = self.rect()
        rect = full.adjusted(64, 78, -64, -170)  # space for labels
        edge_pad = 26                             # corner gap to avoid overlaps
        dot_radius = max(8, min(14, min(rect.width(), rect.height())//40))
        return rect, edge_pad, dot_radius

    def rebuild_points(self):
        rect, edge_pad, _ = self._rect_and_metrics()
        self.points = []
        xL, yT, xR, yB = rect.left(), rect.top(), rect.right(), rect.bottom()

        def centers(a, b, n):
            if n <= 0: return []
            step = (b - a) / n
            return [a + step*(i + 0.5) for i in range(n)]

        c1, c2, c3, c4 = self.counts  # CH1,CH2,CH3,CH4

        # Canonical ring path (clockwise, continuous):
        # CH1 = bottom RIGHT->LEFT
        xs = centers(xL+edge_pad, xR-edge_pad, c1)[::-1]
        for x in xs: self.points.append(QPointF(x, yB))

        # CH2 = left  BOTTOM->TOP
        ys = centers(yT+edge_pad, yB-edge_pad, c2)[::-1]
        for y in ys: self.points.append(QPointF(xL, y))

        # CH3 = top   LEFT->RIGHT
        xs = centers(xL+edge_pad, xR-edge_pad, c3)
        for x in xs: self.points.append(QPointF(x, yT))

        # CH4 = right TOP->BOTTOM
        ys = centers(yT+edge_pad, yB-edge_pad, c4)
        for y in ys: self.points.append(QPointF(xR, y))

        self.points = self.points[:self.ring_len]

    def invalidate_all(self):
        self._accum = 0
        self._tick  = 0

    def advance(self):
        self._accum += 16
        if self._accum >= self.frame_ms:
            self._accum = 0
            self._tick += 1

    def _frame(self):
        L = self.ring_len if self.ring_len>0 else 1
        t = self._tick

        # Master Off: full blackout
        if self.master_off:
            self.pix = [(0,0,0)]*L
            self.update()
            return

        mode = self.mode
        width = max(1, min(64, int(self.width)))
        intensity = max(0, min(255, int(self.intensity)))

        if mode == 0:  # Solid
            c = self.colors[0]; r,g,b = (c>>16)&255,(c>>8)&255,c&255
            self.pix = [(r,g,b)]*L

        elif mode == 1:  # Breathe (use intensity as depth)
            base = self.colors[0]
            depth = 0.10 + 0.90*(intensity/255.0)
            br = (1.0-depth) + depth*(math.sin(t*0.06)*0.5+0.5)
            r,g,b = (base>>16)&255,(base>>8)&255,base&255
            self.pix = [(int(r*br),int(g*br),int(b*br)) for _ in range(L)]

        elif mode == 2:  # Color Wipe (width controls trail length)
            idx = (t//2)%L; off=(0,0,0)
            self.pix = [off]*L
            c = self.colors[0]; cr,cg,cb=(c>>16)&255,(c>>8)&255,c&255
            for k in range(width):
                p = (idx - k) % L
                fall = 1.0 - k/float(max(1,width))
                self.pix[p] = (int(cr*fall), int(cg*fall), int(cb*fall))

        elif mode == 3:  # Larson (width controls bar half-span)
            pos = (t//3)%(L*2)
            if pos >= L: pos = 2*L-1-pos
            fade = 0.80 + 0.19*(intensity/255.0)
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            head = self.colors[0]; r,g,b=(head>>16)&255,(head>>8)&255,head&255
            w = max(1, min(20, width))
            for k in range(-w,w+1):
                p = pos+k
                if 0 <= p < L:
                    alpha = 1.0 - abs(k)/(w+0.0001)
                    self.pix[p] = (int(r*alpha),int(g*alpha),int(b*alpha))

        elif mode == 4:  # Rainbow
            denom = max(2, 8 - int(6*(intensity/255.0)))
            offset = (t//denom) & 255
            self.pix = [ wheel((i*256//L + offset) & 255) for i in range(L) ]

        elif mode == 5:  # Theater
            gap = max(2, min(10, width))
            q = (t//max(2, 12 - int(10*(intensity/255.0)))) % gap
            base = self.colors[0]; r,g,b=(base>>16)&255,(base>>8)&255,base&255
            fade = 0.6
            self.pix = [(int(pr*fade),int(pg*fade),int(pb*fade)) for (pr,pg,pb) in self.pix]
            for i in range(q, L, gap): self.pix[i] = (r,g,b)

        elif mode == 6:  # Twinkle
            fade = 0.90 + 0.09*(1.0 - intensity/255.0)
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            pops = 1 + max(1, L//max(12, 30 - width*2))
            base = self.colors[0]; r,g,b=(base>>16)&255,(base>>8)&255,base&255
            for _ in range(pops): self.pix[random.randrange(L)] = (r,g,b)

        elif mode == 7:  # Comet
            pos = (t//max(1, 4 - int(3*(intensity/255.0))))%L
            head = self.colors[0]; hr,hg,hb=(head>>16)&255,(head>>8)&255,head&255
            fade = 0.80 + 0.19*(1.0 - intensity/255.0)
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            w = max(2, min(30, width*2))
            for k in range(w):
                tail = 1.0 - (k/float(w))
                p = (pos - k) % L
                self.pix[p] = (int(hr*tail),int(hg*tail),int(hb*tail))

        elif mode == 8:  # Meteor
            pos = (t//3)%L
            a = self.colors[0]; b = self.colors[1]
            ar,ag,ab=(a>>16)&255,(a>>8)&255,a&255
            br,bg,bb=(b>>16)&255,(b>>8)&255,b&255
            w = max(3, min(30, width+3))
            self.pix = [(0,0,0)]*L
            for k in range(w):
                tt = k/(w-1 if w>1 else 1)
                r = int(ar*(1-tt)+br*tt); g = int(ag*(1-tt)+bg*tt); b_ = int(ab*(1-tt)+bb*tt)
                self.pix[(pos+k)%L] = (r,g,b_)

        elif mode == 9:  # Clock Spin
            pos = (t//3)%L; span = max(3, min(40, width*2+1))
            bg = self.colors[1]; fg = self.colors[0]
            bgr,bgg,bgb=(bg>>16)&255,(bg>>8)&255,bg&255
            fgr,fgg,fgb=(fg>>16)&255,(fg>>8)&255,fg&255
            self.pix = [(bgr,bgg,bgb)]*L
            for k in range(span): self.pix[(pos+k)%L]=(fgr,fgg,fgb)

        elif mode == 10:  # Plasma
            sat = 0.5 + 0.5*(intensity/255.0)
            out = []
            for i in range(L):
                a = (i/L)*math.tau
                v = 0.5 + 0.5*(math.sin(3*a + t*0.08)*0.5 + math.sin(5*a - t*0.05)*0.5)
                hue = (v*0.7 + (t*0.01)) % 1.0
                out.append(hsv2rgb(hue, sat, 1.0))
            self.pix = out

        elif mode == 11:  # Fire (stylized)
            fade = 0.86
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            sparks = 1 + max(1, L//max(10, 24 - width))
            for _ in range(sparks):
                k = random.randrange(L)
                self.pix[k] = (255, random.randrange(160,255), 0)

        elif mode == 12:  # Palette Cycle
            n = max(1, self.palette_count)
            pal = [self.colors[i] for i in range(n)]
            off = (t*0.01) % 1.0
            out = []
            blend = intensity / 255.0
            for i in range(L):
                x = (i/L + off) % 1.0
                pos = x*n; i0 = int(pos) % n; i1 = (i0+1)%n; frac = pos - int(pos)
                a = pal[i0]; b = pal[i1]
                ar,ag,ab=(a>>16)&255,(a>>8)&255,a&255; br,bg,bb=(b>>16)&255,(b>>8)&255,b&255
                r = int(ar*(1-frac*blend)+br*(frac*blend))
                g = int(ag*(1-frac*blend)+bg*(frac*blend))
                b_ = int(ab*(1-frac*blend)+bb*(frac*blend))
                out.append((r,g,b_))
            self.pix = out

        elif mode == 13:  # Palette Chase
            n = max(1, self.palette_count)
            pal = [self.colors[i] for i in range(n)]
            block = max(1, min(40, width))
            pos = (t//3)%self.ring_len
            out = []
            soft = intensity/255.0
            for i in range(L):
                k = (i + L - pos) % L
                which = (k // block) % n
                c = pal[which]; r,g,b = (c>>16)&255,(c>>8)&255,c&255
                edge = k % block
                tEdge = abs(edge - (block-1)/2.0) / (block/2.0 if block>1 else 1)
                s = 1.0 - soft * tEdge
                if s < 0: s = 0
                out.append((int(r*s),int(g*s),int(b*s)))
            self.pix = out

        # brightness
        if self.brightness < 0.999:
            br = self.brightness
            self.pix = [(int(r*br),int(g*br),int(b*br)) for (r,g,b) in self.pix]

        self.update()

    def paintEvent(self, ev):
        qp = QPainter(self); qp.setRenderHint(QPainter.Antialiasing, True)
        qp.fillRect(self.rect(), QColor(14,17,22))

        rect, edge_pad, dot_radius = self._rect_and_metrics()

        # frame
        qp.setPen(QPen(QColor(30,40,55), 3))
        qp.drawRoundedRect(rect, 18, 18)

        # labels
        lbl_pen = QPen(QColor(150,170,200)); qp.setPen(lbl_pen)
        font = QFont(); font.setPointSizeF(11); qp.setFont(font)

        qp.drawText(QRectF(rect.left(), rect.top()-30, rect.width(), 20),
                    Qt.AlignHCenter|Qt.AlignVCenter, "Rear (CH3)")
        qp.drawText(QRectF(rect.left(), rect.bottom()+10, rect.width(), 20),
                    Qt.AlignHCenter|Qt.AlignVCenter, "Front (CH1)")

        qp.save()
        qp.translate(rect.left()-30, (rect.top()+rect.bottom())/2)
        qp.rotate(-90)
        qp.drawText(QRectF(-60, -10, 120, 20), Qt.AlignCenter, "Left (CH2)")
        qp.restore()

        qp.save()
        qp.translate(rect.right()+30, (rect.top()+rect.bottom())/2)
        qp.rotate(90)
        qp.drawText(QRectF(-60, -10, 120, 20), Qt.AlignCenter, "Right (CH4)")
        qp.restore()

        # LEDs with per-channel reverse mapping
        if len(self.points) != self.ring_len:
            self.rebuild_points()

        qp.setPen(QPen(QColor(0,0,0,140), 1))

        offs = [0,
                self.counts[0],
                self.counts[0] + self.counts[1],
                self.counts[0] + self.counts[1] + self.counts[2]]

        for ch in range(4):
            cnt = self.counts[ch]
            base = offs[ch]
            rev = self.reverse[ch]
            for k in range(cnt):
                point_idx = base + k
                src_idx   = base + (cnt - 1 - k) if rev else (base + k)
                if point_idx >= len(self.points) or src_idx >= len(self.pix): break
                p = self.points[point_idx]
                r,g,b = self.pix[src_idx]
                qp.setBrush(QBrush(QColor(r,g,b)))
                qp.drawEllipse(p, dot_radius, dot_radius)

    def resizeEvent(self, ev):
        super().resizeEvent(ev)
        self.rebuild_points()

# -------------------- main --------------------
def main():
    app = QApplication(sys.argv)
    w = RGBSim()
    w.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
