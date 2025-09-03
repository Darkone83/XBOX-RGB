# rgbctrl_desktop.py
# XBOX RGB Control – Desktop controller for RGBCtrl firmware (v1.6.x)
# - Auto-discovers devices via UDP broadcast
# - Mirrors Web UI controls 1:1 (incl. per-channel Reverse, Master Off, Custom Playlist)
# - UDP preview/save/reset/get + HTTP fallback to /config/api/*
# - Live preview with debounce on change
#
# Deps: pip install PySide6 requests

import sys, socket, json, time
from dataclasses import dataclass
from typing import List, Optional

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QIcon
from PySide6.QtWidgets import (
    QApplication, QWidget, QLabel, QComboBox, QSlider, QSpinBox, QGridLayout,
    QHBoxLayout, QVBoxLayout, QPushButton, QLineEdit, QCheckBox, QGroupBox,
    QColorDialog, QMessageBox, QTextEdit
)

APP_NAME = "XBOX RGB Control"
ICON_PATH = "dc.ico"
DEFAULT_UDP_PORT = 7777
HTTP_BASE_TEMPLATE = "http://{ip}/config/api"

# ------------------- Firmware mirrors ---------------------
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
    ("Custom (Playlist)", 14),   # NEW
]

# Visibility map (same as Web UI JS showOptsFor)
VIS = {
    "colorA":   set(range(0,14)),                # 0..13
    "colorB":   {8,9,10,12,13},
    "colorC":   {12,13},
    "colorD":   {12,13},
    "palette":  {12,13},
    "width":    {3,5,7,8,9,13},
    "intensity":{3,5,6,7,8,11,12,13},
    # For mode 14 (Custom), we show the Custom group explicitly and leave main sliders visible.
}

def default_cfg():
    return {
        "count": [50,50,50,50],
        "brightness": 180,
        "mode": 4,  # Rainbow
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
        # Mirror firmware compile-time defaults (Front & Right reversed)
        "reverse": [True, False, False, True],
        # NEW:
        "masterOff": False,
        "customLoop": True,
        "customSeq": "[]",
    }

# ------------------- Utilities ----------------------------
def clamp(v, lo, hi): return max(lo, min(hi, v))
def rgb_to_qcolor(rgb):
    r = (rgb >> 16) & 0xFF; g = (rgb >> 8) & 0xFF; b = rgb & 0xFF
    return QColor(r,g,b)
def qcolor_to_rgb(c):
    return ((c.red() & 0xFF) << 16) | ((c.green() & 0xFF) << 8) | (c.blue() & 0xFF)
def hex_to_rgb(s):
    try:
        s = s.strip().lstrip("#")
        return int(s, 16) & 0xFFFFFF
    except:
        return None
def rgb_to_hex(rgb):
    return "#{:06X}".format(rgb & 0xFFFFFF)

# ------------------- Discovery ----------------------------
@dataclass
class Device:
    ip: str
    port: int
    name: str = "XBOX RGB"
    ver: str = "?"
    mac: str = ""

def discover_devices(timeout=1.2, psk: Optional[str]=None, port=DEFAULT_UDP_PORT) -> List[Device]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.settimeout(timeout)
        sock.bind(("", 0))

        payload = {"op":"discover"}
        if psk: payload["key"] = psk
        msg_json = json.dumps(payload).encode("utf-8")
        msg_txt  = b"RGBDISC?"

        sock.sendto(msg_json, ("255.255.255.255", port))
        sock.sendto(msg_txt,  ("255.255.255.255", port))

        found = {}
        t0 = time.time()
        while (time.time() - t0) < timeout:
            try:
                data, (rip, rport) = sock.recvfrom(4096)
            except socket.timeout:
                break
            txt = data.decode("utf-8", errors="ignore").strip()

            meta = None
            if txt.startswith("{"):
                try:
                    j = json.loads(txt)
                    if j.get("op") == "discover":
                        meta = {
                            "ip": j.get("ip", rip),
                            "port": int(j.get("port", port)),
                            "name": j.get("name", "XBOX RGB"),
                            "ver": j.get("ver", "?"),
                            "mac": j.get("mac", "")
                        }
                except Exception:
                    pass
            elif txt.startswith("RGBDISC!"):
                try:
                    after = txt.split("!",1)[1].strip()
                    j = json.loads(after)
                    meta = {
                        "ip": j.get("ip", rip),
                        "port": int(j.get("port", port)),
                        "name": j.get("name","XBOX RGB"),
                        "ver": j.get("ver","?"),
                        "mac": j.get("mac","")
                    }
                except Exception:
                    pass

            if meta:
                key = (meta["ip"], meta["port"])
                found[key] = Device(**meta)

        return list(found.values())
    finally:
        sock.close()

# ------------------- Transport layer ----------------------
class Transport(object):
    def __init__(self):
        self.ip: Optional[str] = None
        self.udp_port: int = DEFAULT_UDP_PORT
        self.use_udp: bool = True
        self.psk: Optional[str] = None

    def target_ok(self) -> bool:
        return bool(self.ip)

    def udp_send_recv(self, payload, expect_reply=True, timeout=1.2):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        try:
            if self.psk:
                payload = dict(payload)
                payload["key"] = self.psk
            data = json.dumps(payload).encode("utf-8")
            s.sendto(data, (self.ip, self.udp_port))
            if expect_reply:
                resp, _ = s.recvfrom(65535)
                return resp
            return None
        finally:
            s.close()

    def http_get(self, path):
        import requests
        base = HTTP_BASE_TEMPLATE.format(ip=self.ip)
        r = requests.get(base + path, timeout=2.5)
        r.raise_for_status()
        return r.text

    def http_post(self, path, body):
        import requests
        base = HTTP_BASE_TEMPLATE.format(ip=self.ip)
        r = requests.post(base + path, json=body, timeout=3.0)
        r.raise_for_status()
        return r.text

    def get_config(self):
        if self.use_udp and self.target_ok():
            try:
                resp = self.udp_send_recv({"op":"get"}, expect_reply=True)
                if resp:
                    j = json.loads(resp.decode("utf-8","ignore"))
                    return j.get("cfg", j)
            except Exception:
                pass
        try:
            txt = self.http_get("/ledconfig")
            return json.loads(txt)
        except Exception:
            return default_cfg()

    def preview(self, cfg):
        if self.use_udp and self.target_ok():
            try:
                self.udp_send_recv({"op":"preview","cfg":cfg}, expect_reply=False)
                return True
            except Exception:
                pass
        try:
            self.http_post("/ledpreview", cfg)
            return True
        except Exception:
            return False

    def save(self, cfg):
        if self.use_udp and self.target_ok():
            try:
                self.udp_send_recv({"op":"save","cfg":cfg}, expect_reply=False)
                return True
            except Exception:
                pass
        try:
            self.http_post("/ledsave", cfg)
            return True
        except Exception:
            return False

    def reset(self):
        if self.use_udp and self.target_ok():
            try:
                self.udp_send_recv({"op":"reset"}, expect_reply=False)
                return True
            except Exception:
                pass
        try:
            self.http_post("/ledreset", {})
            return True
        except Exception:
            return False

# ------------------- Color control widget -----------------
class ColorField(QWidget):
    def __init__(self, title, initial=0x000000, parent=None):
        super(ColorField, self).__init__(parent)
        self.rgb = initial & 0xFFFFFF
        self.label = QLabel(title)
        self.btn   = QPushButton("Pick")
        self.hex   = QLineEdit(rgb_to_hex(self.rgb))
        self.hex.setMaxLength(7)

        self.swatch = QLabel()
        self.swatch.setFixedSize(28, 20)
        self.swatch.setAutoFillBackground(True)

        row = QHBoxLayout()
        row.addWidget(self.label, 1)
        row.addWidget(self.swatch)
        row.addWidget(self.btn)
        row.addWidget(self.hex)
        self.setLayout(row)

        self.btn.clicked.connect(self.pick)
        self.hex.editingFinished.connect(self.from_hex)
        self.update_swatch()

    def update_swatch(self):
        c = rgb_to_qcolor(self.rgb)
        self.swatch.setStyleSheet("background: %s; border:1px solid #444" % c.name())

    def pick(self):
        c = QColorDialog.getColor(rgb_to_qcolor(self.rgb), self, "Pick Color")
        if c.isValid():
            self.rgb = qcolor_to_rgb(c)
            self.hex.setText(rgb_to_hex(self.rgb))
            self.update_swatch()
            self.parentChanged()

    def from_hex(self):
        v = hex_to_rgb(self.hex.text())
        if v is not None:
            self.rgb = v
            self.hex.setText(rgb_to_hex(self.rgb))
            self.update_swatch()
            self.parentChanged()
        else:
            self.hex.setText(rgb_to_hex(self.rgb))

    def parentChanged(self): pass

    def set_value(self, rgb):
        self.rgb = rgb & 0xFFFFFF
        self.hex.setText(rgb_to_hex(self.rgb))
        self.update_swatch()

    def value(self):
        return self.rgb

# ------------------- Main window --------------------------
class MainWindow(QWidget):
    def __init__(self):
        super(MainWindow, self).__init__()
        self.setWindowTitle(APP_NAME)
        self.setWindowIcon(QIcon(ICON_PATH))
        self.transport = Transport()
        self.cfg = default_cfg()
        self.devices: List[Device] = []

        self._build_ui()
        self._wire_signals()

        self._debounce = QTimer(self)
        she = self._debounce
        she.setInterval(120)
        she.setSingleShot(True)
        she.timeout.connect(self._send_preview)

        # auto-discover at startup
        self.rescan_devices(auto=True)

    # --------------- UI construction ----------------------
    def _build_ui(self):
        root = QVBoxLayout()

        # Connection group (auto; manual override optional)
        conn = QGroupBox("Connection")
        connL = QGridLayout()

        self.deviceList = QComboBox()
        self.rescanBtn  = QPushButton("Rescan")
        self.useUDP     = QCheckBox("Use UDP (fallback to HTTP)")
        self.useUDP.setChecked(True)

        self.manualOverride = QCheckBox("Manual IP override")
        self.ipEdit = QLineEdit("")
        self.ipEdit.setPlaceholderText("e.g. 192.168.1.123")
        self.ipEdit.setEnabled(False)
        self.portEdit = QLineEdit(str(DEFAULT_UDP_PORT))
        self.portEdit.setEnabled(False)

        self.loadBtn = QPushButton("Reload from device")
        self.status  = QLabel("status: discovering…")

        row = 0
        connL.addWidget(QLabel("Device"), row,0)
        connL.addWidget(self.deviceList, row,1)
        connL.addWidget(self.rescanBtn,  row,2); row += 1

        connL.addWidget(self.useUDP, row,0,1,3); row += 1

        connL.addWidget(self.manualOverride, row,0,1,1)
        connL.addWidget(QLabel("IP"), row,1)
        connL.addWidget(self.ipEdit, row,2); row += 1

        connL.addWidget(QLabel("UDP Port"), row,1)
        connL.addWidget(self.portEdit, row,2); row += 1

        connL.addWidget(self.loadBtn, row,0,1,3); row += 1
        connL.addWidget(self.status,  row,0,1,3)

        conn.setLayout(connL)

        # Controls group
        ctrl = QGroupBox("Controls")
        g = QGridLayout()

        self.mode = QComboBox()
        for name, val in MODES: self.mode.addItem(name, val)

        def mk_slider(minv, maxv, val):
            s = QSlider(Qt.Horizontal); s.setMinimum(minv); s.setMaximum(maxv); s.setValue(val); return s

        self.brightness = mk_slider(1,255,180)
        self.speed      = mk_slider(0,255,128)
        self.intensity  = mk_slider(0,255,128)
        self.width      = mk_slider(1,20,4)

        # Keep label refs so we can hide specific widgets (not the whole group)
        self.lblBrightness = QLabel("Brightness")
        self.lblSpeed      = QLabel("Speed")
        self.lblIntensity  = QLabel("Intensity")
        self.lblWidth      = QLabel("Width / Gap")
        self.lblPalette    = QLabel("Palette Size")

        self.brightnessV = QLabel("180")
        self.speedV      = QLabel("128")
        self.intensityV  = QLabel("128")
        self.widthV      = QLabel("4")

        # NEW: Master Off
        self.masterOff = QCheckBox("Master Off (all channels off)")

        self.colorA = ColorField("Color A", 0xFF0000)
        self.colorB = ColorField("Color B", 0xFFA000)
        self.colorC = ColorField("Color C", 0x00FF00)
        self.colorD = ColorField("Color D", 0x0000FF)
        for cf in (self.colorA,self.colorB,self.colorC,self.colorD):
            cf.parentChanged = self._value_changed

        self.paletteCount = QComboBox()
        for i in range(1,5): self.paletteCount.addItem(f"{i} color" + ("" if i==1 else "s"), i)

        self.c0 = QSpinBox(); self.c1 = QSpinBox(); self.c2 = QSpinBox(); self.c3 = QSpinBox()
        for s in (self.c0,self.c1,self.c2,self.c3): s.setRange(0,50)

        # NEW: per-channel reverse toggles
        self.rev0 = QCheckBox("Reverse CH1 (Front)")
        self.rev1 = QCheckBox("Reverse CH2 (Left)")
        self.rev2 = QCheckBox("Reverse CH3 (Rear)")
        self.rev3 = QCheckBox("Reverse CH4 (Right)")
        revBox = QGroupBox("Channel Direction")
        revL = QVBoxLayout()
        revL.addWidget(self.rev0)
        revL.addWidget(self.rev1)
        revL.addWidget(self.rev2)
        revL.addWidget(self.rev3)
        revBox.setLayout(revL)

        self.resume = QComboBox(); self.resume.addItems(["No","Yes"])
        self.smbusCpu = QCheckBox("Enable CPU temp LEDs (CH5)")
        self.smbusFan = QCheckBox("Enable Fan speed LEDs (CH6)")

        # NEW: Custom Playlist group
        self.customGroup = QGroupBox("Custom Playlist")
        cgL = QGridLayout()
        self.customLoop = QCheckBox("Loop playlist")
        self.customLoop.setChecked(True)
        self.customSeq = QTextEdit()
        self.customSeq.setPlaceholderText('[ {"mode":0,"duration":1000,"colorA":16711680}, {"mode":7,"duration":1200,"speed":200} ]')
        cgL.addWidget(self.customLoop, 0,0,1,3)
        cgL.addWidget(QLabel("Steps JSON"), 1,0)
        cgL.addWidget(self.customSeq, 1,1,1,2)
        self.customGroup.setLayout(cgL)

        self.previewBtn = QPushButton("Preview")
        self.saveBtn    = QPushButton("Save")
        self.resetBtn   = QPushButton("Reset Defaults")

        r = 0
        g.addWidget(QLabel("Mode"), r,0); g.addWidget(self.mode, r,1,1,2); r+=1

        g.addWidget(self.masterOff, r,0,1,3); r+=1  # NEW

        g.addWidget(self.lblBrightness, r,0)
        g.addWidget(self.brightness, r,1); g.addWidget(self.brightnessV, r,2); r+=1

        g.addWidget(self.lblSpeed, r,0)
        g.addWidget(self.speed, r,1); g.addWidget(self.speedV, r,2); r+=1

        g.addWidget(self.lblIntensity, r,0)
        g.addWidget(self.intensity, r,1); g.addWidget(self.intensityV, r,2); r+=1

        g.addWidget(self.lblWidth, r,0)
        g.addWidget(self.width, r,1); g.addWidget(self.widthV, r,2); r+=1

        g.addWidget(self.colorA, r,0,1,3); r+=1
        g.addWidget(self.colorB, r,0,1,3); r+=1
        g.addWidget(self.colorC, r,0,1,3); r+=1
        g.addWidget(self.colorD, r,0,1,3); r+=1

        g.addWidget(self.lblPalette, r,0); g.addWidget(self.paletteCount, r,1,1,2); r+=1

        g.addWidget(QLabel("CH1 (Front) Count"), r,0); g.addWidget(self.c0, r,1,1,2); r+=1
        g.addWidget(QLabel("CH2 (Left) Count"),  r,0); g.addWidget(self.c1, r,1,1,2); r+=1
        g.addWidget(QLabel("CH3 (Rear) Count"),  r,0); g.addWidget(self.c2, r,1,1,2); r+=1
        g.addWidget(QLabel("CH4 (Right) Count"), r,0); g.addWidget(self.c3, r,1,1,2); r+=1

        # Insert Reverse group
        g.addWidget(revBox, r,0,1,3); r+=1

        # NEW: Custom Playlist group (visible when mode==14)
        g.addWidget(self.customGroup, r,0,1,3); r+=1

        g.addWidget(QLabel("Resume last mode on boot"), r,0); g.addWidget(self.resume, r,1,1,2); r+=1
        g.addWidget(self.smbusCpu, r,0,1,3); r+=1
        g.addWidget(self.smbusFan, r,0,1,3); r+=1

        btnRow = QHBoxLayout()
        btnRow.addWidget(self.previewBtn)
        btnRow.addWidget(self.saveBtn)
        btnRow.addWidget(self.resetBtn)
        g.addLayout(btnRow, r,0,1,3); r+=1

        ctrl.setLayout(g)

        # Footer (copyright / version if provided)
        self.footer = QLabel("© Darkone Customs 2025")
        self.footer.setAlignment(Qt.AlignCenter)

        root.addWidget(conn)
        root.addWidget(ctrl)
        root.addWidget(self.footer)
        self.setLayout(root)

    def _wire_signals(self):
        # Connection
        self.rescanBtn.clicked.connect(self.rescan_devices)
        self.deviceList.currentIndexChanged.connect(self._device_chosen)
        self.useUDP.stateChanged.connect(self._toggle_udp)

        self.manualOverride.stateChanged.connect(self._toggle_manual)
        self.ipEdit.editingFinished.connect(self._apply_manual)
        self.portEdit.editingFinished.connect(self._apply_manual)
        self.loadBtn.clicked.connect(self.reload_from_device)

        # Controls → debounce preview
        self.mode.currentIndexChanged.connect(self._mode_changed)
        for s in (self.brightness, self.speed, self.intensity, self.width):
            s.valueChanged.connect(self._slider_label_sync)
            s.valueChanged.connect(self._value_changed)
        for s in (self.c0,self.c1,self.c2,self.c3):
            s.valueChanged.connect(self._value_changed)

        # New toggles
        for cb in (self.rev0, self.rev1, self.rev2, self.rev3, self.masterOff):
            cb.stateChanged.connect(self._value_changed)

        self.paletteCount.currentIndexChanged.connect(self._palette_changed)
        self.resume.currentIndexChanged.connect(self._value_changed)
        self.smbusCpu.stateChanged.connect(self._value_changed)
        self.smbusFan.stateChanged.connect(self._value_changed)

        # Custom playlist changes
        self.customLoop.stateChanged.connect(self._value_changed)
        self.customSeq.textChanged.connect(self._value_changed)

        self.previewBtn.clicked.connect(self._send_preview)
        self.saveBtn.clicked.connect(self._send_save)
        self.resetBtn.clicked.connect(self._send_reset)

    # --------------- Discovery / selection ----------------
    def rescan_devices(self, auto=False):
        self.status.setText("status: discovering…")
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            self.devices = discover_devices(timeout=1.2, psk=self.transport.psk, port=DEFAULT_UDP_PORT)
            self.deviceList.blockSignals(True)
            self.deviceList.clear()
            for d in self.devices:
                self.deviceList.addItem(f"{d.name}  •  {d.ip}  •  v{d.ver}", userData=d)
            self.deviceList.blockSignals(False)

            if self.devices:
                self.deviceList.setCurrentIndex(0)
                self._device_chosen()
                self.status.setText(f"status: {len(self.devices)} device(s) found")
            else:
                self.transport.ip = None
                self.status.setText("status: no devices found (you can enable Manual IP override)")
        finally:
            QApplication.restoreOverrideCursor()

    def _device_chosen(self):
        d: Device = self.deviceList.currentData()
        if not d:
            self.transport.ip = None
            self.status.setText("status: no device selected")
            return
        self.transport.ip = d.ip
        self.transport.udp_port = d.port
        self.ipEdit.setText(d.ip)
        self.portEdit.setText(str(d.port))
        self.reload_from_device()  # load immediately

    def _toggle_udp(self, _):
        self.transport.use_udp = self.useUDP.isChecked()
        self.status.setText("status: transport=%s" % ("UDP" if self.transport.use_udp else "HTTP"))

    def _toggle_manual(self, _):
        en = self.manualOverride.isChecked()
        self.ipEdit.setEnabled(en)
        self.portEdit.setEnabled(en)
        if en and self.ipEdit.text().strip():
            self._apply_manual()

    def _apply_manual(self):
        if not self.manualOverride.isChecked():
            return
        ip = self.ipEdit.text().strip()
        try:
            port = int(self.portEdit.text().strip())
        except:
            port = DEFAULT_UDP_PORT
            self.portEdit.setText(str(port))
        self.transport.ip = ip or None
        self.transport.udp_port = port
        self.status.setText(f"status: manual target {ip or '—'}:{port}")

    # --------------- Event handlers -----------------------
    def _slider_label_sync(self):
        self.brightnessV.setText(str(self.brightness.value()))
        self.speedV.setText(str(self.speed.value()))
        self.intensityV.setText(str(self.intensity.value()))
        self.widthV.setText(str(self.width.value()))

    def _mode_changed(self):
        self._update_visibility()
        self._value_changed()

    def _palette_changed(self):
        self._update_visibility()
        self._value_changed()

    def _value_changed(self):
        self._debounce.start()

    # --------------- Device I/O ---------------------------
    def reload_from_device(self):
        if not self.transport.target_ok():
            self.status.setText("status: no device target")
            return
        self.status.setText("status: loading…")
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            cfg = self.transport.get_config()
            m = cfg.get("mode", 4)
            if not isinstance(m, int) or m < 0 or m > 14:
                cfg["mode"] = 4  # Rainbow default

            # footer text (copyright + version if provided)
            cpy = cfg.get("copyright", "© Darkone Customs 2025")
            ver = cfg.get("buildVersion")
            self.footer.setText(f"{cpy}" + (f"  •  v{ver}" if ver else ""))

            self._cfg_to_ui(cfg)
            self.status.setText("status: ready")
        except Exception as e:
            self.status.setText("status: error loading")
            QMessageBox.warning(self, "Load Failed", str(e))
        finally:
            QApplication.restoreOverrideCursor()

    def _send_preview(self):
        if not self.transport.target_ok(): return
        cfg = self._ui_to_cfg()
        ok = self.transport.preview(cfg)
        self.status.setText("status: live" if ok else "status: preview error")

    def _send_save(self):
        if not self.transport.target_ok(): return
        cfg = self._ui_to_cfg()
        ok = self.transport.save(cfg)
        self.status.setText("status: saved" if ok else "status: save error")

    def _send_reset(self):
        if not self.transport.target_ok(): return
        if QMessageBox.question(self, "Reset", "Reset to defaults?") != QMessageBox.Yes:
            return
        ok = self.transport.reset()
        if ok:
            self.reload_from_device()
            self.status.setText("status: reset")
        else:
            self.status.setText("status: reset error")

    # --------------- Mapping UI <-> JSON ------------------
    def _cfg_to_ui(self, cfg):
        self.cfg = {**default_cfg(), **cfg}  # ensure new keys exist

        desired = self.cfg.get("mode", 4)
        idx = 0
        for i,(name,val) in enumerate(MODES):
            if val == desired: idx=i; break
        self.mode.setCurrentIndex(idx)

        self.masterOff.setChecked(bool(self.cfg.get("masterOff", False)))

        self.brightness.setValue(clamp(self.cfg.get("brightness",180),1,255))
        self.speed.setValue(clamp(self.cfg.get("speed",128),0,255))
        self.intensity.setValue(clamp(self.cfg.get("intensity",128),0,255))
        self.width.setValue(clamp(self.cfg.get("width",4),1,20))
        self._slider_label_sync()

        self.colorA.set_value(self.cfg.get("colorA",0))
        self.colorB.set_value(self.cfg.get("colorB",0))
        self.colorC.set_value(self.cfg.get("colorC",0))
        self.colorD.set_value(self.cfg.get("colorD",0))

        pc = clamp(self.cfg.get("paletteCount",2),1,4)
        self.paletteCount.setCurrentIndex(pc-1)

        c = self.cfg.get("count",[50,50,50,50]) + [50,50,50,50]
        self.c0.setValue(clamp(c[0],0,50))
        self.c1.setValue(clamp(c[1],0,50))
        self.c2.setValue(clamp(c[2],0,50))
        self.c3.setValue(clamp(c[3],0,50))

        # reverse flags (fallback to defaults if missing)
        rev = self.cfg.get("reverse", default_cfg()["reverse"])
        self.rev0.setChecked(bool(rev[0] if len(rev)>0 else False))
        self.rev1.setChecked(bool(rev[1] if len(rev)>1 else False))
        self.rev2.setChecked(bool(rev[2] if len(rev)>2 else False))
        self.rev3.setChecked(bool(rev[3] if len(rev)>3 else False))

        self.resume.setCurrentIndex(1 if self.cfg.get("resumeOnBoot",True) else 0)
        self.smbusCpu.setChecked(bool(self.cfg.get("enableCpu",True)))
        self.smbusFan.setChecked(bool(self.cfg.get("enableFan",True)))

        # Custom playlist
        self.customLoop.setChecked(bool(self.cfg.get("customLoop", True)))
        try:
            # Pretty print if valid; otherwise leave as-is
            seq = self.cfg.get("customSeq", "[]")
            steps = json.loads(seq) if isinstance(seq, str) else seq
            self.customSeq.setPlainText(json.dumps(steps, indent=2))
        except Exception:
            self.customSeq.setPlainText(self.cfg.get("customSeq", "[]"))

        self._update_visibility()

    def _ui_to_cfg(self):
        mval = MODES[self.mode.currentIndex()][1]
        # Try to keep customSeq valid JSON; on error, fall back to "[]"
        seq_txt = self.customSeq.toPlainText().strip()
        try:
            _parsed = json.loads(seq_txt)  # validate
            seq_out = json.dumps(_parsed)  # compact
        except Exception:
            seq_out = "[]"

        cfg = {
            "count": [self.c0.value(), self.c1.value(), self.c2.value(), self.c3.value()],
            "brightness": self.brightness.value(),
            "mode": mval,
            "speed": self.speed.value(),
            "intensity": self.intensity.value(),
            "width": self.width.value(),
            "colorA": self.colorA.value(),
            "colorB": self.colorB.value(),
            "colorC": self.colorC.value(),
            "colorD": self.colorD.value(),
            "paletteCount": self.paletteCount.currentData() or (self.paletteCount.currentIndex()+1),
            "reverse": [
                self.rev0.isChecked(),
                self.rev1.isChecked(),
                self.rev2.isChecked(),
                self.rev3.isChecked(),
            ],
            "resumeOnBoot": (self.resume.currentIndex()==1),
            "enableCpu": self.smbusCpu.isChecked(),
            "enableFan": self.smbusFan.isChecked(),
            # NEW:
            "masterOff": self.masterOff.isChecked(),
            "customLoop": self.customLoop.isChecked(),
            "customSeq": seq_out,
        }
        return cfg

    # --------------- Visibility rules ---------------------
    def _update_visibility(self):
        mode = MODES[self.mode.currentIndex()][1]
        def vis(key): return (mode in VIS.get(key,set()))
        # individual widgets (do NOT hide the entire group)
        use_int = vis("intensity")
        self.lblIntensity.setVisible(use_int)
        self.intensity.setVisible(use_int)
        self.intensityV.setVisible(use_int)

        use_w = vis("width")
        self.lblWidth.setVisible(use_w)
        self.width.setVisible(use_w)
        self.widthV.setVisible(use_w)

        # Colors / palette
        self.colorA.setVisible(mode != 14 and vis("colorA"))   # in Custom, colors are per-step; but keep visible if you want globals
        self.colorB.setVisible(mode != 14 and vis("colorB"))
        palActive = (mode != 14 and vis("palette"))
        self.lblPalette.setVisible(palActive)
        self.paletteCount.setVisible(palActive)

        pc = self.paletteCount.currentData() or 2
        self.colorC.setVisible(palActive and pc >= 3 and vis("colorC"))
        self.colorD.setVisible(palActive and pc >= 4 and vis("colorD"))

        # Custom group visibility
        self.customGroup.setVisible(mode == 14)

# ------------------- main -------------------------------
def main():
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setWindowIcon(QIcon(ICON_PATH))
    w = MainWindow()
    w.resize(820, 920)
    w.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
