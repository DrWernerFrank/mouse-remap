#!/usr/bin/env python3
"""
GUI for mouse-remap with full per-button customization and profiles.

Run as your normal user:  python3 mouse-remap-gui.py
It re-launches itself with pkexec (one password prompt) so it can grab the
mouse, then everything works with no terminal.
"""

import os
import re
import sys
import pwd
import json
import shutil
import signal
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

# Path to the compiled engine: env override (set by the AppImage AppRun),
# otherwise the binary sitting next to this script (dev / plain-script use).
BINARY = os.environ.get("MOUSE_REMAP_BIN") or os.path.join(HERE, "mouse-remap")

# Read-only template profiles shipped alongside the script / inside the AppImage.
TEMPLATE_DIR = os.path.join(HERE, "profiles")


def real_user():
    """Return (home, uid, gid) of the human user, even when re-run as root."""
    uid = os.environ.get("PKEXEC_UID") or os.environ.get("SUDO_UID")
    if uid:
        try:
            p = pwd.getpwuid(int(uid))
            return p.pw_dir, p.pw_uid, p.pw_gid
        except (KeyError, ValueError):
            pass
    return os.path.expanduser("~"), os.getuid(), os.getgid()


HOME, REAL_UID, REAL_GID = real_user()
CONFIG_DIR = os.path.join(HOME, ".config", "mouse-remap")
PROFILE_DIR = os.path.join(CONFIG_DIR, "profiles")
ACTIVE_CONF = os.path.join(CONFIG_DIR, "active.conf")


def chown_back(path):
    """Give a file/dir we created as root back to the human user."""
    try:
        os.chown(path, REAL_UID, REAL_GID)
    except OSError:
        pass


def _stage_for_root(script, binary):
    """AppImages live on a per-user FUSE mount that root CANNOT read. Before
    elevating, copy the script + engine + templates onto real disk (in the
    user's ~/.cache, which root can read) and return the staged paths."""
    in_appimage = os.environ.get("APPIMAGE") or os.environ.get("APPDIR") \
        or "/.mount_" in script
    if not in_appimage:
        return script, binary
    stage = os.path.join(os.path.expanduser("~"), ".cache", "mouse-remap", "run")
    os.makedirs(os.path.join(stage, "profiles"), exist_ok=True)
    s_script = os.path.join(stage, "mouse-remap-gui.py")
    s_binary = os.path.join(stage, "mouse-remap")
    shutil.copyfile(script, s_script)
    shutil.copyfile(binary, s_binary)
    os.chmod(s_binary, 0o755)
    if os.path.isdir(TEMPLATE_DIR):
        for f in os.listdir(TEMPLATE_DIR):
            if f.endswith(".json"):
                shutil.copyfile(os.path.join(TEMPLATE_DIR, f),
                                os.path.join(stage, "profiles", f))
    return s_script, s_binary


# --- elevate to root while keeping the user's X display ----------------------
if os.geteuid() != 0:
    display = os.environ.get("DISPLAY", ":0")
    xauth = os.environ.get("XAUTHORITY", os.path.expanduser("~/.Xauthority"))
    _script, _binary = _stage_for_root(os.path.abspath(__file__), BINARY)
    os.execvp("pkexec", [
        "pkexec", "env",
        f"DISPLAY={display}", f"XAUTHORITY={xauth}",
        f"MOUSE_REMAP_BIN={_binary}",
        sys.executable, _script,
    ])

import tkinter as tk
from tkinter import ttk, messagebox, simpledialog

# source key -> friendly label (defines display order)
SOURCES = [
    ("left",       "Left button"),
    ("right",      "Right button"),
    ("middle",     "Middle / wheel click"),
    ("side",       "Side back button"),
    ("extra",      "Side forward button"),
    ("wheel_up",   "Scroll up"),
    ("wheel_down", "Scroll down"),
]

# action label -> config value, for normal buttons
BUTTON_ACTIONS = [
    ("Left click",                 "click:left"),
    ("Right click",                "click:right"),
    ("Middle click",               "click:middle"),
    ("Double left click",          "double:left"),
    ("Double right click",         "double:right"),
    ("Double middle click",        "double:middle"),
    ("Hold left (drag)",           "hold:left"),
    ("Hold right",                 "hold:right"),
    ("Hold middle",                "hold:middle"),
    ("Tap = double, Hold = left",  "tapdouble:left"),
    ("Tap = double, Hold = right", "tapdouble:right"),
    ("Scroll up",                  "scroll:up"),
    ("Scroll down",                "scroll:down"),
    ("Disabled",                   "none"),
]

# action label -> config value, for scroll directions
WHEEL_ACTIONS = [
    ("Right click",       "click:right"),
    ("Left click",        "click:left"),
    ("Middle click",      "click:middle"),
    ("Double left click", "double:left"),
    ("Scroll up",         "scroll:up"),
    ("Scroll down",       "scroll:down"),
    ("Disabled",          "none"),
]

DEFAULT_MAPPING = {
    "left": "tapdouble:left",
    "right": "hold:right",
    "middle": "click:right",
    "side": "hold:left",
    "extra": "hold:left",
    "wheel_up": "click:right",
    "wheel_down": "click:right",
}
DEFAULT_HOLD_MS = 150


def actions_for(src):
    return WHEEL_ACTIONS if src in ("wheel_up", "wheel_down") else BUTTON_ACTIONS


def list_devices():
    devices = [("Auto-detect (recommended)", None)]
    try:
        out = subprocess.run([BINARY, "--list"], capture_output=True,
                             text=True, timeout=5).stdout
    except Exception:
        return devices
    best = None
    m = re.search(r"Best guess:\s*(/dev/input/event\d+)", out)
    if m:
        best = m.group(1)
    for line in out.splitlines():
        m = re.match(r"\s*(/dev/input/event\d+)\s+score=(-?\d+).*?\"(.*)\"", line)
        if not m or m.group(2) == "-1":
            continue
        path, name = m.group(1), m.group(3).strip()
        tag = "  ← best" if path == best else ""
        devices.append((f"{name or path}  [{os.path.basename(path)}]{tag}", path))
    return devices


class App:
    def __init__(self, root):
        self.root = root
        self.proc = None
        self.ensure_config_dir()
        root.title("Mouse Remap")
        root.resizable(False, False)

        frm = ttk.Frame(root, padding=16)
        frm.grid(sticky="nsew")
        r = 0

        ttk.Label(frm, text="Mouse Remap",
                  font=("Sans", 16, "bold")).grid(row=r, column=0, columnspan=3,
                                                  sticky="w")
        r += 1

        # ---- profile bar ----
        prof = ttk.LabelFrame(frm, text="Profile", padding=8)
        prof.grid(row=r, column=0, columnspan=3, sticky="we", pady=(8, 4))
        self.profile_var = tk.StringVar()
        self.profile_combo = ttk.Combobox(prof, textvariable=self.profile_var,
                                          state="readonly", width=22)
        self.profile_combo.pack(side="left")
        self.profile_combo.bind("<<ComboboxSelected>>",
                                lambda e: self.load_profile(self.profile_var.get()))
        ttk.Button(prof, text="Save", width=6, command=self.save_profile).pack(side="left", padx=3)
        ttk.Button(prof, text="Save As", width=8, command=self.save_profile_as).pack(side="left", padx=3)
        ttk.Button(prof, text="New", width=5, command=self.new_profile).pack(side="left", padx=3)
        ttk.Button(prof, text="Delete", width=7, command=self.delete_profile).pack(side="left", padx=3)
        r += 1

        # ---- device ----
        ttk.Label(frm, text="Mouse:").grid(row=r, column=0, sticky="w", pady=(8, 2))
        self.devices = list_devices()
        self.device_var = tk.StringVar(value=self.devices[0][0])
        self.device_combo = ttk.Combobox(frm, textvariable=self.device_var,
                                         state="readonly", width=34,
                                         values=[d[0] for d in self.devices])
        self.device_combo.grid(row=r, column=1, columnspan=2, sticky="w", pady=(8, 2))
        r += 1

        # ---- per-source mapping rows ----
        maps = ttk.LabelFrame(frm, text="Button mappings", padding=10)
        maps.grid(row=r, column=0, columnspan=3, sticky="we", pady=(8, 4))
        self.map_vars = {}
        for i, (src, label) in enumerate(SOURCES):
            ttk.Label(maps, text=label).grid(row=i, column=0, sticky="w", pady=2)
            ttk.Label(maps, text="→").grid(row=i, column=1, padx=8)
            var = tk.StringVar()
            labels = [a[0] for a in actions_for(src)]
            cb = ttk.Combobox(maps, textvariable=var, state="readonly",
                              width=26, values=labels)
            cb.grid(row=i, column=2, sticky="w", pady=2)
            cb.bind("<<ComboboxSelected>>", lambda e: self.apply_if_running())
            self.map_vars[src] = var
        r += 1

        # ---- hold slider ----
        ttk.Label(frm, text="Hold time:").grid(row=r, column=0, sticky="w", pady=4)
        hr = ttk.Frame(frm)
        hr.grid(row=r, column=1, columnspan=2, sticky="w", pady=4)
        self.hold_scale = ttk.Scale(hr, from_=50, to=600, orient="horizontal",
                                    length=200,
                                    command=lambda v: self.hold_label.config(
                                        text=f"{int(float(v))} ms"))
        self.hold_scale.set(DEFAULT_HOLD_MS)
        self.hold_scale.pack(side="left")
        self.hold_label = ttk.Label(hr, text=f"{DEFAULT_HOLD_MS} ms", width=7)
        self.hold_label.pack(side="left", padx=(8, 0))
        self.hold_scale.bind("<ButtonRelease-1>", lambda e: self.apply_if_running())
        r += 1

        # ---- start/stop ----
        bar = ttk.Frame(frm)
        bar.grid(row=r, column=0, columnspan=3, sticky="we", pady=(12, 4))
        self.dot = tk.Label(bar, text="●", fg="#c0392b", font=("Sans", 13))
        self.dot.pack(side="left")
        self.status = ttk.Label(bar, text="Stopped", width=9)
        self.status.pack(side="left", padx=(2, 10))
        self.start_btn = ttk.Button(bar, text="Start", command=self.start)
        self.start_btn.pack(side="left", expand=True, fill="x", padx=3)
        self.stop_btn = ttk.Button(bar, text="Stop", command=self.stop, state="disabled")
        self.stop_btn.pack(side="left", expand=True, fill="x", padx=3)
        ttk.Button(bar, text="Rescan", command=self.rescan).pack(side="left", padx=3)

        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.refresh_profiles()
        self.set_mapping(DEFAULT_MAPPING, DEFAULT_HOLD_MS)
        self.poll()

    # ---- config dir / template seeding ----
    def ensure_config_dir(self):
        os.makedirs(PROFILE_DIR, exist_ok=True)
        chown_back(CONFIG_DIR)
        chown_back(PROFILE_DIR)
        # On first run, copy bundled template profiles into the user's dir.
        if os.path.isdir(TEMPLATE_DIR) and not any(
                f.endswith(".json") for f in os.listdir(PROFILE_DIR)):
            for f in os.listdir(TEMPLATE_DIR):
                if f.endswith(".json"):
                    dst = os.path.join(PROFILE_DIR, f)
                    try:
                        shutil.copyfile(os.path.join(TEMPLATE_DIR, f), dst)
                        chown_back(dst)
                    except OSError:
                        pass

    # ---- mapping <-> widgets ----
    def label_for_value(self, src, value):
        for lbl, val in actions_for(src):
            if val == value:
                return lbl
        return actions_for(src)[0][0]

    def value_for_label(self, src, label):
        for lbl, val in actions_for(src):
            if lbl == label:
                return val
        return "none"

    def set_mapping(self, mapping, hold_ms):
        for src, var in self.map_vars.items():
            var.set(self.label_for_value(src, mapping.get(src, "none")))
        self.hold_scale.set(hold_ms)
        self.hold_label.config(text=f"{int(hold_ms)} ms")

    def current_mapping(self):
        return {src: self.value_for_label(src, var.get())
                for src, var in self.map_vars.items()}

    def current_hold(self):
        return int(round(float(self.hold_scale.get())))

    # ---- profiles ----
    def profile_path(self, name):
        return os.path.join(PROFILE_DIR, name + ".json")

    def refresh_profiles(self):
        names = sorted(f[:-5] for f in os.listdir(PROFILE_DIR) if f.endswith(".json"))
        self.profile_combo["values"] = names
        return names

    def load_profile(self, name):
        if not name:
            return
        try:
            with open(self.profile_path(name)) as f:
                data = json.load(f)
        except Exception as e:
            messagebox.showerror("Mouse Remap", f"Could not load profile:\n{e}")
            return
        self.set_mapping(data.get("mapping", DEFAULT_MAPPING),
                         data.get("hold_ms", DEFAULT_HOLD_MS))
        dev = data.get("device")
        if dev:
            for lbl, path in self.devices:
                if path == dev:
                    self.device_var.set(lbl)
                    break
        self.apply_if_running()

    def _write_profile(self, name):
        data = {
            "mapping": self.current_mapping(),
            "hold_ms": self.current_hold(),
            "device": self.selected_path(),
        }
        path = self.profile_path(name)
        with open(path, "w") as f:
            json.dump(data, f, indent=2)
        chown_back(path)

    def save_profile(self):
        name = self.profile_var.get()
        if not name:
            self.save_profile_as()
            return
        self._write_profile(name)
        messagebox.showinfo("Mouse Remap", f"Saved profile “{name}”.")

    def save_profile_as(self):
        name = simpledialog.askstring("Save profile as", "Profile name:",
                                      parent=self.root)
        if not name:
            return
        name = re.sub(r"[^\w\- ]", "", name).strip()
        if not name:
            return
        self._write_profile(name)
        self.refresh_profiles()
        self.profile_var.set(name)

    def new_profile(self):
        self.set_mapping(DEFAULT_MAPPING, DEFAULT_HOLD_MS)
        self.profile_var.set("")
        self.apply_if_running()

    def delete_profile(self):
        name = self.profile_var.get()
        if not name:
            return
        if not messagebox.askyesno("Mouse Remap", f"Delete profile “{name}”?"):
            return
        try:
            os.remove(self.profile_path(name))
        except OSError:
            pass
        self.refresh_profiles()
        self.profile_var.set("")

    # ---- run control ----
    def selected_path(self):
        for label, path in self.devices:
            if label == self.device_var.get():
                return path
        return None

    def write_active_conf(self):
        with open(ACTIVE_CONF, "w") as f:
            f.write("# generated by mouse-remap-gui\n")
            for src, val in self.current_mapping().items():
                f.write(f"{src}={val}\n")
            f.write(f"hold_ms={self.current_hold()}\n")
        chown_back(ACTIVE_CONF)

    def start(self):
        if self.proc and self.proc.poll() is None:
            return
        if not os.path.exists(BINARY):
            messagebox.showerror("Mouse Remap",
                                 f"Binary not found:\n{BINARY}\n\n"
                                 "Build:  cc -O2 -o mouse-remap mouse-remap.c")
            return
        self.write_active_conf()
        cmd = [BINARY, "--config", ACTIVE_CONF]
        path = self.selected_path()
        if path:
            cmd.append(path)
        try:
            self.proc = subprocess.Popen(cmd)
        except Exception as e:
            messagebox.showerror("Mouse Remap", f"Could not start:\n{e}")
            return
        self.set_running(True)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
        self.set_running(False)

    def apply_if_running(self):
        if self.proc and self.proc.poll() is None:
            self.stop()
            self.start()

    def rescan(self):
        was = self.selected_path()
        self.devices = list_devices()
        labels = [d[0] for d in self.devices]
        self.device_combo["values"] = labels
        keep = next((l for l, p in self.devices if p == was), labels[0])
        self.device_var.set(keep)

    def set_running(self, running):
        self.dot.config(fg="#27ae60" if running else "#c0392b")
        self.status.config(text="Running" if running else "Stopped")
        self.start_btn.config(state="disabled" if running else "normal")
        self.stop_btn.config(state="normal" if running else "disabled")
        self.device_combo.config(state="disabled" if running else "readonly")

    def poll(self):
        if self.proc and self.proc.poll() is not None:
            self.proc = None
            self.set_running(False)
        self.root.after(500, self.poll)

    def on_close(self):
        self.stop()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
