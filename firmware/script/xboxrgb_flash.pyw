# xbox_rgb_flasher.py
# XBOX RGB Flasher — simple Tk GUI for flashing a merged BIN to ESP32
# - Auto-detects the first ESP32 on available serial ports
# - User only selects the BIN and clicks "Flash"
# - Uses esptool via "python -m esptool" (streamed output)
#
# Deps: pip install esptool pyserial
# Icon: place dc.ico in the same folder (Windows). On mac/Linux it's ignored if not found.

import sys
import os
import threading
import queue
import subprocess
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from typing import Optional, Tuple

try:
    from serial.tools import list_ports
except Exception:
    list_ports = None  # user must install pyserial


APP_NAME = "XBOX RGB Flasher"
ICON_PATH = "dc.ico"
DEFAULT_BAUD = "460800"   # conservative + fast enough
FLASH_OFFSET = "0x0"      # merged image starts at 0x0

# --------------------------------------------------------------------
# Detection
# --------------------------------------------------------------------
def detect_first_esp32() -> Optional[Tuple[str, str]]:
    """
    Return (port, chip) for the first connected ESP chip, else None.
    Heuristic: prefer ports that look like USB UARTs. We don't open ports here;
    esptool will toggle boot mode automatically during flashing.
    """
    if list_ports is None:
        return None

    ports = list(list_ports.comports())
    if not ports:
        return None

    # Light preferencing for typical USB UARTs
    prefer_substrings = ("usb", "uart", "wch", "silab", "cp210", "ch34", "ftdi")
    ranked = []
    for p in ports:
        score = 0
        desc = f"{p.description or ''} {p.hwid or ''}".lower()
        for sub in prefer_substrings:
            if sub in desc:
                score += 1
        ranked.append((score, p))
    ranked.sort(key=lambda x: (-x[0], x[1].device))

    # We don't truly know chip type until esptool talks to it; we label "ESP32-family"
    # esptool with --chip auto will identify actual variant.
    for _, p in ranked:
        return (p.device, "ESP32-family")

    return None

# --------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------
class FlasherApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_NAME)
        # Icon (Windows)
        if os.path.exists(ICON_PATH):
            try:
                self.iconbitmap(ICON_PATH)
            except Exception:
                pass

        # State
        self.selected_file: Optional[str] = None
        self.detected_port: Optional[str] = None
        self.detected_chip: Optional[str] = None

        # Build UI
        self._build_ui()

        # Try detection at startup
        self._detect_and_show()

    def _build_ui(self):
        root = ttk.Frame(self, padding=12)
        root.grid(row=0, column=0, sticky="nsew")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        root.columnconfigure(1, weight=1)

        # Row 0: Detected device (read-only)
        ttk.Label(root, text="Detected device:").grid(row=0, column=0, sticky="w", pady=(0,6))
        self.detect_label = ttk.Label(root, text="(none)")
        self.detect_label.grid(row=0, column=1, sticky="w", pady=(0,6))

        # Row 1: File picker
        ttk.Label(root, text="Merged BIN file:").grid(row=1, column=0, sticky="w")
        pick_row = ttk.Frame(root)
        pick_row.grid(row=1, column=1, sticky="ew")
        pick_row.columnconfigure(0, weight=1)
        self.file_entry = ttk.Entry(pick_row)
        self.file_entry.grid(row=0, column=0, sticky="ew", padx=(0,6))
        ttk.Button(pick_row, text="Browse…", command=self._browse).grid(row=0, column=1, sticky="e")

        # Row 2: Actions
        action_row = ttk.Frame(root)
        action_row.grid(row=2, column=0, columnspan=2, pady=(10,6), sticky="ew")
        action_row.columnconfigure(0, weight=1)
        ttk.Button(action_row, text="Flash", command=self._on_flash).grid(row=0, column=0, sticky="ew")
        ttk.Button(action_row, text="Rescan Device", command=self._detect_and_show).grid(row=0, column=1, padx=(6,0))

        # Row 3: Log
        ttk.Label(root, text="Log:").grid(row=3, column=0, columnspan=2, sticky="w", pady=(10,4))
        self.log = tk.Text(root, height=16, width=80, state="disabled")
        self.log.grid(row=4, column=0, columnspan=2, sticky="nsew")
        root.rowconfigure(4, weight=1)

        # Footer: copyright
        footer = ttk.Label(root, text="© Darkone Customs 2025", anchor="center")
        footer.grid(row=5, column=0, columnspan=2, pady=(8,0), sticky="ew")

    # ----------------------------------------------------------------
    # Helpers
    # ----------------------------------------------------------------
    def _detect_and_show(self):
        res = detect_first_esp32()
        if res:
            self.detected_port, self.detected_chip = res
            self._status(f"Detected {self.detected_chip} on {self.detected_port}")
            self.detect_label.config(text=f"{self.detected_chip} @ {self.detected_port}")
        else:
            self.detected_port = None
            self.detected_chip = None
            self._status("No ESP32 detected. Connect device and click Rescan.")
            self.detect_label.config(text="(none)")

    def _browse(self):
        fname = filedialog.askopenfilename(
            title="Select merged ESP32 firmware (.bin)",
            filetypes=[("BIN files","*.bin"), ("All files","*.*")]
        )
        if fname:
            self.selected_file = fname
            self.file_entry.delete(0, "end")
            self.file_entry.insert(0, fname)

    def _status(self, msg: str):
        self._append_log(msg + "\n")

    def _append_log(self, text: str):
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    # ----------------------------------------------------------------
    # Flashing
    # ----------------------------------------------------------------
    def _on_flash(self):
        # Check file
        path = self.file_entry.get().strip()
        if not path:
            messagebox.showwarning(APP_NAME, "Please select a merged .bin file.")
            return
        if not os.path.exists(path):
            messagebox.showerror(APP_NAME, "Selected file does not exist.")
            return

        # Ensure we have a target; if not, try detect again right now
        if not self.detected_port:
            self._detect_and_show()
        if not self.detected_port:
            messagebox.showerror(APP_NAME, "No ESP32 detected. Connect device and click Rescan.")
            return

        # Disable UI during flash
        self._toggle_ui(False)
        self._append_log(f"\n=== Flashing {os.path.basename(path)} to {self.detected_port} at {FLASH_OFFSET} ===\n")

        # Start worker thread
        t = threading.Thread(target=self._flash_worker, args=(self.detected_port, path), daemon=True)
        t.start()

    def _toggle_ui(self, enabled: bool):
        for w in (self.file_entry, self.log):
            w.configure(state=("normal" if enabled else "disabled"))
        # Buttons: need to reference them explicitly
        # Find all children buttons and enable/disable
        for child in self.children.values():
            self._set_buttons_enabled(child, enabled)

    def _set_buttons_enabled(self, widget, enabled):
        if isinstance(widget, (ttk.Button, tk.Button)):
            widget.configure(state=("normal" if enabled else "disabled"))
        if hasattr(widget, "children"):
            for ch in widget.children.values():
                self._set_buttons_enabled(ch, enabled)

    def _flash_worker(self, port: str, bin_path: str):
        """
        Run esptool in a subprocess so we can stream its output into the log.
        """
        # Build esptool command
        cmd = [
            sys.executable, "-m", "esptool",
            "--chip", "auto",
            "--port", port,
            "--baud", DEFAULT_BAUD,
            "--before", "default_reset",
            "--after", "hard_reset",
            "write_flash",
            "-z",
            FLASH_OFFSET, bin_path
        ]

        self._append_log("Running: " + " ".join(cmd) + "\n")

        # Stream output
        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=1,
                universal_newlines=True,
            )
        except FileNotFoundError:
            self._done_with_error("Python not found to spawn esptool. Run this script with Python.")
            return
        except Exception as e:
            self._done_with_error(f"Failed to start esptool: {e}")
            return

        # Read stdout line by line
        try:
            for line in proc.stdout:
                self._append_log(line)
        except Exception as e:
            self._append_log(f"\n[Reader error] {e}\n")
        finally:
            proc.wait()

        rc = proc.returncode
        if rc == 0:
            self._append_log("\n=== Flash complete. Device will reset. ===\n")
            self._done_ok()
        else:
            self._done_with_error(f"esptool exited with code {rc}")

    def _done_ok(self):
        self._toggle_ui(True)
        try:
            self.bell()
        except Exception:
            pass

    def _done_with_error(self, msg: str):
        self._append_log("\nERROR: " + msg + "\n")
        self._toggle_ui(True)
        messagebox.showerror(APP_NAME, msg)

# --------------------------------------------------------------------
# Main
# --------------------------------------------------------------------
def main():
    app = FlasherApp()
    # reasonable default size
    app.geometry("720x520")
    app.minsize(600, 400)
    app.mainloop()

if __name__ == "__main__":
    main()
