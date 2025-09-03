# -*- coding: utf-8 -*-
# XBOX RGB - Simple one-file menu script for XBMC/XBMC4Gamers (Python 2.x)
# Place at: Q:\system\scripts\XBOX RGB\default.py
from __future__ import print_function

import socket, time
try:
    import json as _json
except Exception:
    try:
        import simplejson as _json
    except Exception:
        _json = None

import xbmc
import xbmcgui

# -------- protocol / constants (ASCII only text) --------
UDP_PORT = 7777
DISCOVERY_PREFIX = 'RGBDISC! '  # unit may prefix adverts like "RGBDISC! {json}"

MODES = [
    (0, 'Solid'), (1, 'Breathe'), (2, 'Color Wipe'), (3, 'Larson'),
    (4, 'Rainbow'), (5, 'Theater Chase'), (6, 'Twinkle'), (7, 'Comet'),
    (8, 'Meteor'), (9, 'Clock Spin'), (10, 'Plasma'), (11, 'Fire / Flicker'),
    (12, 'Palette Cycle'), (13, 'Palette Chase'),
    (14, 'Custom (Playlist)'),
]
MODE_VALUES = [v for v,_ in MODES]
MODE_NAMES  = [n for _,n in MODES]

# 16-color preset palette (RRGGBB ints)
COLOR_PALETTE = [
    ('White',       0xFFFFFF),
    ('Black',       0x000000),
    ('Red',         0xFF0000),
    ('Green',       0x00FF00),
    ('Blue',        0x0000FF),
    ('Yellow',      0xFFFF00),
    ('Magenta',     0xFF00FF),
    ('Cyan',        0x00FFFF),
    ('Orange',      0xFFA500),
    ('Purple',      0x800080),
    ('Pink',        0xFFC0CB),
    ('Lime',        0xBFFF00),
    ('Teal',        0x008080),
    ('Sky',         0x87CEEB),
    ('WarmWhite',   0xFFD8B0),
    ('ColdWhite',   0xD0E7FF),
]
COLOR_NAMES = [n for n,_ in COLOR_PALETTE]

# Defaults mirror firmware (incl. per-channel reverse)
DEFAULT_CFG = {
    'mode': 4,
    'brightness': 180,
    'speed': 128,
    'intensity': 128,
    'width': 4,
    'colorA': 0xFF0000,
    'colorB': 0xFFA000,
    'colorC': 0x00FF00,
    'colorD': 0x0000FF,
    'paletteCount': 2,
    'count': [50,50,50,50],
    'resumeOnBoot': True,
    'enableCpu': True,
    'enableFan': True,
    'reverse': [True, False, False, True],  # NEW
    # v1.6 additions:
    'masterOff': False,
    'customLoop': True,
    'customSeq': '[]',  # JSON array of steps; edited here (power users) or via WebUI builder
}

# -------------------- helpers (ASCII-safe) --------------------
def _notify(title, msg):
    try:
        xbmc.executebuiltin('XBMC.Notification(%s,%s,2000)' % (str(title).replace(',', ' '), str(msg).replace(',', ' ')))
    except Exception:
        pass

def _hex24(n): return '#%06X' % (int(n) & 0xFFFFFF)

# ---- Dialog compatibility wrappers (no keyword args) ----
def dlg_select(heading, options):
    try: return xbmcgui.Dialog().select(heading, options)
    except TypeError: return xbmcgui.Dialog().select(heading, options)

def dlg_input(heading, default_text='', itype=None):
    d = xbmcgui.Dialog()
    try:
        if itype is None: return d.input(heading, default_text)
        else:             return d.input(heading, default_text, itype)
    except TypeError:
        try: return d.input(heading)
        except Exception:
            try:
                kb = xbmc.Keyboard(default_text, heading)
                kb.doModal()
                return kb.getText() if kb.isConfirmed() else ''
            except Exception:
                return ''

def dlg_numeric(n_type, heading, default_text=''):
    d = xbmcgui.Dialog()
    try: return d.numeric(n_type, heading, default_text)
    except TypeError: return d.numeric(n_type, heading)

# -------------------- UDP client --------------------
class UdpClient(object):
    def __init__(self, psk=''):
        self.psk = psk or ''

    def _bind_sock(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try: s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        except Exception: pass
        try: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except Exception: pass
        try: s.bind(('', UDP_PORT))
        except Exception: pass
        s.settimeout(0.5)
        return s

    def discover(self, timeout=3.5):
        s = self._bind_sock()
        devices, seen = [], set()
        end = time.time() + timeout
        # Probe both JSON and plain-text forms for compatibility
        try: s.sendto(self._dumps_bytes({'op':'discover'}), ('255.255.255.255', UDP_PORT))
        except Exception: pass
        try: s.sendto(b'RGBDISC?', ('255.255.255.255', UDP_PORT))
        except Exception: pass
        while time.time() < end:
            try:
                data, (rip, rport) = s.recvfrom(2048)
            except socket.timeout:
                continue
            except Exception:
                break
            if not data: continue
            js = self._loads_robust(data)
            if not isinstance(js, dict):
                # handle "RGBDISC! {json}"
                try:
                    txt = data.decode('utf-8')
                except Exception:
                    try: txt = data.decode('latin-1')
                    except Exception: txt = ''
                if txt.startswith(DISCOVERY_PREFIX):
                    js = self._loads_robust(txt[len(DISCOVERY_PREFIX):])
            if not isinstance(js, dict):
                continue
            # Accept with or without "ok": true
            if js.get('op') == 'discover' and (js.get('ok') in (None, True)):
                ip   = js.get('ip') or rip
                port = int(js.get('port') or UDP_PORT)
                mac  = js.get('mac') or ''
                name = js.get('name') or 'XBOX RGB'
                key  = (ip, port, mac)
                if key in seen: continue
                seen.add(key)
                devices.append({'name': name, 'ip': ip, 'port': port, 'mac': mac, 'ver': js.get('ver','')})
        try: s.close()
        except Exception: pass
        return devices

    def _send_recv(self, ip, port, body, timeout=1.2):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(timeout)
        try:
            payload = dict(body)
            if self.psk: payload['key'] = self.psk
            msg = self._dumps_bytes(payload)
            sock.sendto(msg, (ip, port))
            data, _ = sock.recvfrom(4096)
            return self._loads_robust(data)
        except Exception:
            return None
        finally:
            try: sock.close()
            except Exception: pass

    def _send_only(self, ip, port, body):
        """Fire-and-forget (no reply expected)."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            payload = dict(body)
            if self.psk: payload['key'] = self.psk
            msg = self._dumps_bytes(payload)
            sock.sendto(msg, (ip, port))
            return True
        except Exception:
            return False
        finally:
            try: sock.close()
            except Exception: pass

    def get(self, ip, port):
        r = self._send_recv(ip, port, {'op':'get'})
        if not r: return None
        return r.get('cfg') if isinstance(r, dict) and 'cfg' in r else r

    # preview/save: do NOT wait for a reply (firmware typically sends none)
    def preview(self, ip, port, cfg): return self._send_only(ip, port, {'op':'preview','cfg':cfg})
    def save(self, ip, port, cfg):    return self._send_only(ip, port, {'op':'save','cfg':cfg})
    def reset(self, ip, port):        r=self._send_recv(ip, port, {'op':'reset'}); return bool(r and r.get('ok'))

    # json helpers kept inside the class to avoid name clashes
    def _dumps_bytes(self, obj):
        if _json:
            s = _json.dumps(obj, separators=(',',':'))
            try:
                if isinstance(s, unicode): s = s.encode('utf-8')
            except NameError:
                pass
            return s
        # very small fallback
        parts = []
        for k, v in obj.items():
            if isinstance(v, (int, long)): vv = str(v)
            elif isinstance(v, bool):       vv = 'true' if v else 'false'
            elif isinstance(v, dict):
                vv = self._dumps_bytes(v)
                if isinstance(vv, bytes):
                    try: vv = vv.decode('utf-8')
                    except Exception: vv = str(vv)
            elif isinstance(v, (list, tuple)):
                vv = '[' + ','.join([self._to_json_scalar(x) for x in v]) + ']'
            else: vv = '"%s"' % str(v).replace('"','\\"')
            parts.append('"%s":%s' % (k, vv))
        s = '{' + ','.join(parts) + '}'
        try: return s.encode('utf-8')
        except Exception: return s

    def _to_json_scalar(self, v):
        if isinstance(v, (int, long)): return str(v)
        if isinstance(v, bool): return 'true' if v else 'false'
        return '"%s"' % str(v).replace('"','\\"')

    def _loads_robust(self, b):
        if not _json: return None
        try:
            if isinstance(b, unicode):
                t = b
            else:
                try: t = b.decode('utf-8')
                except Exception:
                    try: t = b.decode('latin-1')
                    except Exception: return None
        except NameError:
            try: t = b.decode('utf-8')
            except Exception:
                try: t = b.decode('latin-1')
                except Exception: return None
        try: return _json.loads(t)
        except Exception: return None

# -------------------- Simple menu app --------------------
class App(object):
    def __init__(self):
        self.net = UdpClient()
        self.dev = None
        self.cfg = dict(DEFAULT_CFG)

    # ---- device discovery / selection ----
    def pick_device(self):
        _notify('XBOX RGB', 'Searching...')
        devs = self.net.discover(timeout=3.5)
        if not devs:
            ip = dlg_input('Enter device IP', '', xbmcgui.INPUT_IPADDRESS)
            if not ip:
                _notify('XBOX RGB', 'No device on UDP %d' % UDP_PORT)
                return False
            self.dev = {'ip': ip, 'port': UDP_PORT, 'name': 'XBOX RGB', 'mac': '', 'ver': ''}
        elif len(devs) == 1:
            self.dev = devs[0]
        else:
            opts = ['%s  (%s)  %s' % (d.get('name','XBOX RGB'), d['ip'], d.get('mac','')) for d in devs]
            idx = dlg_select('Select device', opts)
            if idx < 0: return False
            self.dev = devs[idx]

        _notify('XBOX RGB', 'Using %s @ %s' % (self.dev.get('name','XBOX RGB'), self.dev.get('ip','?')))
        c = self.net.get(self.dev['ip'], int(self.dev['port']))
        if not isinstance(c, dict):
            psk = dlg_input('Enter PSK (if set)', '', xbmcgui.INPUT_ALPHANUM)
            if psk:
                self.net.psk = (psk or '').strip()
                c = self.net.get(self.dev['ip'], int(self.dev['port']))
        if isinstance(c, dict):
            # ensure required keys exist / normalized
            self.cfg.update(c)
            rv = self.cfg.get('reverse')
            if not (isinstance(rv, list) and len(rv) >= 4):
                # default to firmware's compiled defaults if missing
                self.cfg['reverse'] = [True, False, False, True]
            # ensure new keys exist if device is older
            if 'masterOff'   not in self.cfg: self.cfg['masterOff'] = False
            if 'customLoop'  not in self.cfg: self.cfg['customLoop'] = True
            if 'customSeq'   not in self.cfg: self.cfg['customSeq']  = '[]'
        else:
            _notify('XBOX RGB', 'Could not fetch config; using defaults')
        return True

    # ---- menu helpers ----
    def _title(self):
        dev = '%s @ %s' % (self.dev.get('name','?'), self.dev.get('ip','?')) if self.dev else 'No device'
        ver = self.cfg.get('buildVersion') or self.dev.get('ver') or ''
        if ver:
            return 'XBOX RGB - %s  [v%s]' % (dev, ver)
        return 'XBOX RGB - %s' % dev

    def _mode_name(self, v):
        try: return MODE_NAMES[MODE_VALUES.index(int(v))]
        except Exception: return 'Solid'

    # ---- color palette picker (no custom) ----
    def pick_color_from_palette(self, title, current_value):
        """Show 16 preset colors only. Returns int 0xRRGGBB or None (cancel)."""
        items = ['%-10s  %s' % (name, _hex24(rgb)) for name, rgb in COLOR_PALETTE]
        sel = dlg_select(title, items)
        if sel < 0:
            return None
        return COLOR_PALETTE[sel][1]

    # ---- edit operations ----
    def set_mode(self):
        sel = dlg_select('Mode', MODE_NAMES)
        if sel >= 0:
            self.cfg['mode'] = MODE_VALUES[sel]
            self.preview()

    def set_slider(self, key, title, lo, hi):
        cur = int(self.cfg.get(key, lo))
        val = dlg_numeric(0, '%s (%d-%d)' % (title, lo, hi), str(cur))
        try:
            if val not in (None, ''):
                v = max(lo, min(hi, int(val)))
                self.cfg[key] = v
                self.preview()
        except Exception:
            pass

    def set_color(self, key, title):
        cur = int(self.cfg.get(key, 0))
        val = self.pick_color_from_palette(title, cur)
        if val is not None:
            self.cfg[key] = val
            self.preview()

    def set_palette(self):
        sel = dlg_select('Palette Size', ['1','2','3','4'])
        if sel >= 0:
            self.cfg['paletteCount'] = sel+1
            self.preview()

    def set_counts(self):
        counts = list(self.cfg.get('count') or [0,0,0,0]) + [0,0,0,0]
        labels = ['CH1 Count','CH2 Count','CH3 Count','CH4 Count']
        for i in range(4):
            cur = int(counts[i])
            val = dlg_numeric(0, '%s (0-50)' % labels[i], str(cur))
            try:
                if val not in (None, ''):
                    counts[i] = max(0, min(50, int(val)))
            except Exception:
                pass
        self.cfg['count'] = counts[:4]
        self.preview()

    def toggle(self, key, title):
        self.cfg[key] = not bool(self.cfg.get(key, False))
        _notify('XBOX RGB', '%s: %s' % (title, 'On' if self.cfg[key] else 'Off'))
        self.preview()

    def toggle_reverse(self, ch_index):
        rv = list(self.cfg.get('reverse') or [False,False,False,False])
        while len(rv) < 4: rv.append(False)
        rv[ch_index] = not bool(rv[ch_index])
        self.cfg['reverse'] = rv
        _notify('XBOX RGB', 'Reverse CH%d: %s' % (ch_index+1, 'On' if rv[ch_index] else 'Off'))
        self.preview()

    # ---- new: master off / custom playlist ----
    def toggle_master_off(self):
        self.cfg['masterOff'] = not bool(self.cfg.get('masterOff', False))
        _notify('XBOX RGB', 'Master Off: %s' % ('On' if self.cfg['masterOff'] else 'Off'))
        self.preview()

    def toggle_custom_loop(self):
        self.cfg['customLoop'] = not bool(self.cfg.get('customLoop', True))
        _notify('XBOX RGB', 'Custom Loop: %s' % ('On' if self.cfg['customLoop'] else 'Off'))
        # keep mode as-is; preview to reflect new loop flag (if already in custom)
        self.preview()

    def edit_custom_seq(self):
        """
        Edit customSeq JSON (advanced). For friendly editing, use the device WebUI builder,
        but this keeps power-user control available inside XBMC as well.
        """
        cur = self.cfg.get('customSeq') or '[]'
        txt = dlg_input('customSeq JSON (see WebUI builder)', cur, xbmcgui.INPUT_ALPHANUM)
        if txt is None or txt == '':  # cancel or blank
            return
        # accept as-is (device validates). Keep it ASCII-only field.
        self.cfg['customSeq'] = txt
        # If user intends to run it now, switch to Custom mode.
        if int(self.cfg.get('mode', 4)) != 14:
            self.cfg['mode'] = 14
        self.preview()

    # ---- device actions ----
    def preview(self):
        if not self.dev: return
        try: self.net.preview(self.dev['ip'], int(self.dev['port']), self.cfg)
        except Exception: pass

    def save(self):
        if not self.dev: return
        _notify('XBOX RGB', 'Saving...')
        if self.net.save(self.dev['ip'], int(self.dev['port']), self.cfg):
            _notify('XBOX RGB', 'Saved')
        else:
            # Even if no reply, changes were sent; tell user to verify visually
            _notify('XBOX RGB', 'Save sent (no reply)')

    def reset(self):
        if not self.dev: return
        if xbmcgui.Dialog().yesno('Reset', 'Reset device to defaults?'):
            if self.net.reset(self.dev['ip'], int(self.dev['port'])):
                c = self.net.get(self.dev['ip'], int(self.dev['port'])) or {}
                if isinstance(c, dict): self.cfg.update(c)
                _notify('XBOX RGB', 'Reset OK')
            else:
                _notify('XBOX RGB', 'Reset failed')

    def set_manual_ip(self):
        ip = dlg_input('Device IP', (self.dev or {}).get('ip',''), xbmcgui.INPUT_IPADDRESS)
        if ip:
            self.dev = {'ip': ip, 'port': UDP_PORT, 'name':'XBOX RGB', 'mac':'', 'ver': ''}
            c = self.net.get(self.dev['ip'], int(self.dev['port'])) or {}
            if isinstance(c, dict): self.cfg.update(c)
            # ensure reverse exists
            if not (isinstance(self.cfg.get('reverse'), list) and len(self.cfg['reverse']) >= 4):
                self.cfg['reverse'] = [True, False, False, True]
            # ensure new keys exist if device is older
            if 'masterOff'   not in self.cfg: self.cfg['masterOff'] = False
            if 'customLoop'  not in self.cfg: self.cfg['customLoop'] = True
            if 'customSeq'   not in self.cfg: self.cfg['customSeq']  = '[]'

    def set_psk(self):
        psk = dlg_input('PSK', '', xbmcgui.INPUT_ALPHANUM)
        if psk is not None:
            self.net.psk = (psk or '').strip()
            if self.dev:
                c = self.net.get(self.dev['ip'], int(self.dev['port'])) or {}
                if isinstance(c, dict): self.cfg.update(c)

    def rescan(self):
        self.pick_device()

    # ---- main loop ----
    def run(self):
        if not self.pick_device():
            return
        while True:
            c = self.cfg
            rv = list(c.get('reverse') or [False,False,False,False]) + [False,False,False,False]
            entries = [
                'Device:  %s @ %s' % (self.dev.get('name','XBOX RGB'), self.dev.get('ip','?')),
                'Rescan',
                'Manual IP',
                'Set PSK',
                '-----',
                'Mode: %s' % self._mode_name(c.get('mode',0)),
                'Master Off: %s' % ('On' if c.get('masterOff') else 'Off'),
                'Brightness: %d' % int(c.get('brightness',128)),
                'Speed: %d' % int(c.get('speed',128)),
                'Intensity: %d' % int(c.get('intensity',128)),
                'Width / Gap: %d' % int(c.get('width',5)),
                '-----',
                'Primary: %s' % _hex24(c.get('colorA',0x00FF00)),
                'Secondary: %s' % _hex24(c.get('colorB',0x0000FF)),
                'Color C: %s' % _hex24(c.get('colorC',0x000000)),
                'Color D: %s' % _hex24(c.get('colorD',0x000000)),
                'Palette Size: %d' % int(c.get('paletteCount',2)),
                'Channel Counts: %s' % (c.get('count') or [0,0,0,0]),
                # NEW: per-channel reverse toggles
                'Reverse CH1: %s' % ('On' if rv[0] else 'Off'),
                'Reverse CH2: %s' % ('On' if rv[1] else 'Off'),
                'Reverse CH3: %s' % ('On' if rv[2] else 'Off'),
                'Reverse CH4: %s' % ('On' if rv[3] else 'Off'),
                # Custom mode controls (always visible; useful to prep then switch modes)
                '-----',
                'Custom: Loop: %s' % ('On' if c.get('customLoop') else 'Off'),
                'Custom: Edit playlist (JSON)',
                '-----',
                'Resume On Boot: %s' % ('On' if c.get('resumeOnBoot') else 'Off'),
                'SMBus CPU: %s' % ('On' if c.get('enableCpu') else 'Off'),
                'SMBus FAN: %s' % ('On' if c.get('enableFan') else 'Off'),
                '-----',
                'Preview now',
                'Save to device',
                'Reset device',
                'Quit',
            ]
            sel = dlg_select(self._title(), entries)
            if sel < 0: break

            label = entries[sel]
            if   label.startswith('Device:'):      pass
            elif label == 'Rescan':                self.rescan()
            elif label == 'Manual IP':             self.set_manual_ip()
            elif label == 'Set PSK':               self.set_psk()
            elif label.startswith('Mode:'):        self.set_mode()
            elif label.startswith('Master Off:'):  self.toggle_master_off()
            elif label.startswith('Brightness:'):  self.set_slider('brightness','Brightness',1,255)
            elif label.startswith('Speed:'):       self.set_slider('speed','Speed',0,255)
            elif label.startswith('Intensity:'):   self.set_slider('intensity','Intensity',0,255)
            elif label.startswith('Width / Gap:'): self.set_slider('width','Width / Gap',1,20)
            elif label.startswith('Primary:'):     self.set_color('colorA','Primary')
            elif label.startswith('Secondary:'):   self.set_color('colorB','Secondary')
            elif label.startswith('Color C:'):     self.set_color('colorC','Color C')
            elif label.startswith('Color D:'):     self.set_color('colorD','Color D')
            elif label.startswith('Palette Size:'):self.set_palette()
            elif label.startswith('Channel Counts:'): self.set_counts()
            elif label.startswith('Reverse CH1:'): self.toggle_reverse(0)
            elif label.startswith('Reverse CH2:'): self.toggle_reverse(1)
            elif label.startswith('Reverse CH3:'): self.toggle_reverse(2)
            elif label.startswith('Reverse CH4:'): self.toggle_reverse(3)
            elif label.startswith('Custom: Loop:'): self.toggle_custom_loop()
            elif label.startswith('Custom: Edit playlist'): self.edit_custom_seq()
            elif label.startswith('Resume On Boot:'): self.toggle('resumeOnBoot','Resume On Boot')
            elif label.startswith('SMBus CPU:'):      self.toggle('enableCpu','SMBus CPU')
            elif label.startswith('SMBus FAN:'):      self.toggle('enableFan','SMBus FAN')
            elif label == 'Preview now':              self.preview()
            elif label == 'Save to device':           self.save()
            elif label == 'Reset device':             self.reset()
            elif label == 'Quit':                     break

# -------------------- run --------------------
if __name__ == '__main__':
    try:
        App().run()
    except Exception as e:
        try:
            xbmcgui.Dialog().ok('XBOX RGB', 'Script crashed', str(e))
        except Exception:
            pass
