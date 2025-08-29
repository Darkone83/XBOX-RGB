# xboxrgb_sim.py
# XBOX RGB Simulator – acts like the real hardware over UDP.
# Layout:
#   CH1 = Front  (BOTTOM edge)
#   CH2 = Left   (LEFT edge)
#   CH3 = Rear   (TOP edge)
#   CH4 = Right  (RIGHT edge)
# Ring order (index progression) preserved: CH1 -> CH2 -> CH3 -> CH4.
#
# Protocol:
#   Broadcast discover probes received as:
#     - JSON {"op":"discover"}  → reply JSON {"op":"discover", "ip", "port", "name", "ver", "mac"}
#     - Text "RGBDISC?"         → reply "RGBDISC! {JSON...}"
#   UDP ops:
#     - {"op":"get"}            → reply {"ok":true,"op":"get","cfg":{...}}
#     - {"op":"preview","cfg":...}  (no reply; applies live)
#     - {"op":"save","cfg":...}     (no reply; applies live)
#     - {"op":"reset"}               → reply {"ok":true,"op":"reset"} and reset defaults
#
# Requires: PySide6  (pip install PySide6)

import sys, json, socket, time, math, random
from typing import List
from PySide6.QtCore import Qt, QTimer, QPointF, QRectF
from PySide6.QtGui  import QPainter, QColor, QPen, QBrush, QFont
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout, QLabel

UDP_PORT  = 7777
NAME      = "XBOX RGB"
VER       = "1.4.sim"
COPYRIGHT = "© Darkone Customs 2025"

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

        # default config mirrors firmware
        self.cfg = {
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
            "copyright": COPYRIGHT,
        }
        self.apply_cfg(self.cfg, initial=True)

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
                    "paletteCount":2,"resumeOnBoot":True,"enableFan":True,"enableCpu":True
                })
                self.sock.sendto(json.dumps({"ok":True,"op":"reset"}).encode("utf-8"), addr)

    def current_cfg_for_reply(self):
        c = dict(self.cfg)
        c["copyright"] = COPYRIGHT
        return c

    # ---------- Config / animation ----------
    def apply_cfg(self, cfg, initial=False):
        c = self.cfg
        if "count" in cfg:
            v = cfg.get("count",[8,12,12,12])
            c["count"] = [clamp(int(v[0]),0,50), clamp(int(v[1]),0,50),
                          clamp(int(v[2]),0,50), clamp(int(v[3]),0,50)]
            self.canvas.set_counts(c["count"])

        for k in ("brightness","mode","speed","intensity","width",
                  "colorA","colorB","colorC","colorD","paletteCount",
                  "resumeOnBoot","enableCpu","enableFan"):
            if k in cfg: c[k] = cfg[k]

        spd = clamp(int(c["speed"]),0,255)
        self.canvas.frame_ms = max(10, 10 + (255 - spd)//2)
        self.canvas.brightness = clamp(int(c["brightness"]),1,255)/255.0
        self.canvas.colors = [c["colorA"], c["colorB"], c["colorC"], c["colorD"]]
        self.canvas.palette_count = int(clamp(c.get("paletteCount",2),1,4))
        self.canvas.mode = int(c["mode"])
        if not initial: self.canvas.invalidate_all()

    def tick_anim(self):
        self.ticks += 1
        self.canvas.advance()

# -------------------- Drawing / effects --------------------
class Canvas(QWidget):
    def __init__(self):
        super().__init__()
        self.setMinimumSize(900, 420)
        self.counts = [8,12,12,12]   # CH1,CH2,CH3,CH4
        self.points: List[QPointF] = []
        self.ring_len = sum(self.counts)
        self.pix = [(0,0,0)] * self.ring_len
        self.mode = 4
        self.colors = [0xFF0000,0xFFA000,0x00FF00,0x0000FF]
        self.palette_count = 2
        self.brightness = 180/255.0
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

        # Ring order: CH1 -> CH2 -> CH3 -> CH4
        # CH1 = Front (BOTTOM), travel RIGHT->LEFT so it ends at bottom-left
        xs = centers(xL+edge_pad, xR-edge_pad, c1)[::-1]
        for x in xs: self.points.append(QPointF(x, yB))

        # CH2 = Left, travel BOTTOM->TOP
        ys = centers(yT+edge_pad, yB-edge_pad, c2)[::-1]
        for y in ys: self.points.append(QPointF(xL, y))

        # CH3 = Rear (TOP), travel LEFT->RIGHT
        xs = centers(xL+edge_pad, xR-edge_pad, c3)
        for x in xs: self.points.append(QPointF(x, yT))

        # CH4 = Right, travel TOP->BOTTOM
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

        if self.mode == 0:  # Solid
            c = self.colors[0]; r,g,b = (c>>16)&255,(c>>8)&255,c&255
            self.pix = [(r,g,b)]*L

        elif self.mode == 1:  # Breathe
            base = self.colors[0]
            br = 0.12 + 0.88*(math.sin(t*0.06)*0.5+0.5)
            r,g,b = (base>>16)&255,(base>>8)&255,base&255
            self.pix = [(int(r*br),int(g*br),int(b*br)) for _ in range(L)]

        elif self.mode == 2:  # Color Wipe
            idx = (t//2)%L; off=(0,0,0)
            self.pix = [off]*L
            c = self.colors[0]
            self.pix[idx] = ((c>>16)&255,(c>>8)&255,c&255)

        elif self.mode == 3:  # Larson
            pos = (t//3)%(L*2)
            if pos >= L: pos = 2*L-1-pos
            fade = 0.85
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            head = self.colors[0]; r,g,b=(head>>16)&255,(head>>8)&255,head&255
            w = 4
            for k in range(-w,w+1):
                p = pos+k
                if 0 <= p < L:
                    alpha = 1.0 - abs(k)/(w+0.0001)
                    self.pix[p] = (int(r*alpha),int(g*alpha),int(b*alpha))

        elif self.mode == 4:  # Rainbow
            denom = 6
            offset = (t//denom) & 255
            self.pix = [ wheel((i*256//L + offset) & 255) for i in range(L) ]

        elif self.mode == 5:  # Theater
            gap = 3; q = (t//8) % gap
            base = self.colors[0]; r,g,b=(base>>16)&255,(base>>8)&255,base&255
            fade = 0.6
            self.pix = [(int(pr*fade),int(pg*fade),int(pb*fade)) for (pr,pg,pb) in self.pix]
            for i in range(q, L, gap): self.pix[i] = (r,g,b)

        elif self.mode == 6:  # Twinkle
            fade = 0.92
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            pops = 1 + L//20
            base = self.colors[0]; r,g,b=(base>>16)&255,(base>>8)&255,base&255
            for _ in range(pops): self.pix[random.randrange(L)] = (r,g,b)

        elif self.mode == 7:  # Comet
            pos = (t//3)%L
            head = self.colors[0]; hr,hg,hb=(head>>16)&255,(head>>8)&255,head&255
            fade = 0.85
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            width = 6
            for w in range(width):
                tail = 1.0 - (w/width)
                p = (pos - w) % L
                self.pix[p] = (int(hr*tail),int(hg*tail),int(hb*tail))

        elif self.mode == 8:  # Meteor
            pos = (t//3)%L
            a = self.colors[0]; b = self.colors[1]
            ar,ag,ab=(a>>16)&255,(a>>8)&255,a&255
            br,bg,bb=(b>>16)&255,(b>>8)&255,b&255
            width = 5
            self.pix = [(0,0,0)]*L
            for w in range(width):
                tt = w/(width-1 if width>1 else 1)
                r = int(ar*(1-tt)+br*tt); g = int(ag*(1-tt)+bg*tt); b_ = int(ab*(1-tt)+bb*tt)
                self.pix[(pos+w)%L] = (r,g,b_)

        elif self.mode == 9:  # Clock Spin
            pos = (t//3)%L; span = 9
            bg = self.colors[1]; fg = self.colors[0]
            bgr,bgg,bgb=(bg>>16)&255,(bg>>8)&255,bg&255
            fgr,fgg,fgb=(fg>>16)&255,(fg>>8)&255,fg&255
            self.pix = [(bgr,bgg,bgb)]*L
            for w in range(span): self.pix[(pos+w)%L]=(fgr,fgg,fgb)

        elif self.mode == 10:  # Plasma
            for i in range(L):
                a = (i/L)*math.tau
                v = 0.5 + 0.5*(math.sin(3*a + t*0.08)*0.5 + math.sin(5*a - t*0.05)*0.5)
                hue = (v*0.7 + (t*0.01)) % 1.0
                self.pix[i] = hsv2rgb(hue, 0.9, 1.0)

        elif self.mode == 11:  # Fire (rough)
            fade = 0.86
            self.pix = [(int(r*fade),int(g*fade),int(b*fade)) for (r,g,b) in self.pix]
            sparks = 1 + L//12
            for _ in range(sparks):
                k = random.randrange(L)
                self.pix[k] = (255, random.randrange(160,255), 0)

        elif self.mode == 12:  # Palette Cycle
            n = max(1, self.palette_count)
            pal = [self.colors[i] for i in range(n)]
            off = (t*0.01) % 1.0
            out = []
            for i in range(L):
                x = (i/L + off) % 1.0
                pos = x*n; i0 = int(pos) % n; i1 = (i0+1)%n; frac = pos - int(pos)
                a = pal[i0]; b = pal[i1]
                ar,ag,ab=(a>>16)&255,(a>>8)&255,a&255; br,bg,bb=(b>>16)&255,(b>>8)&255,b&255
                r = int(ar*(1-frac)+br*frac); g = int(ag*(1-frac)+bg*frac); b_ = int(ab*(1-frac)+bb*frac)
                out.append((r,g,b_))
            self.pix = out

        elif self.mode == 13:  # Palette Chase
            n = max(1, self.palette_count)
            pal = [self.colors[i] for i in range(n)]
            block = 4; pos = (t//3)%self.ring_len
            out = []
            for i in range(L):
                k = (i + L - pos) % L
                which = (k // block) % n
                c = pal[which]; out.append(((c>>16)&255,(c>>8)&255,c&255))
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

        # LEDs (constant radius everywhere)
        if len(self.points) != self.ring_len:
            self.rebuild_points()
        qp.setPen(QPen(QColor(0,0,0,140), 1))
        for i,p in enumerate(self.points):
            if i >= len(self.pix): break
            r,g,b = self.pix[i]
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
