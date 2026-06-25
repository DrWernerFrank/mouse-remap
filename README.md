# Mouse Remap

A fast, low‑level mouse button remapper for Linux with a simple GUI and saveable
profiles. Remap any button or scroll direction to any click, double‑click, hold,
or scroll action.

The remapping engine is written in **C** and talks straight to the kernel via
`evdev` (read raw input) and `uinput` (emit remapped input). That means
near‑zero latency, no dependencies, and it works the same on **X11 and Wayland**.

<p align="center">
  <img src="mouse-remap.png" width="120" alt="Mouse Remap icon">
</p>

---

## Features

- **Per‑button customization** — pick exactly what each input does:
  left / right / middle / side‑back / side‑forward buttons and scroll up / down.
- **Action types:** single click, double click, *hold* (press‑through, so
  dragging works), *tap‑vs‑hold* (a quick tap fires a double‑click, holding acts
  as a real held button), scroll, or disabled.
- **Profiles** — save, load, and delete named setups (stored as JSON in
  `~/.config/mouse-remap/profiles/`). Ships with **Default** and **Passthrough**.
- **Adjustable hold time** — the threshold that separates a tap from a hold.
- **Smart device detection** — scores input devices so it grabs your real mouse,
  not a keyboard’s phantom mouse interface. Safe `--watch` mode to confirm.
- **Tiny & dependency‑free engine** — a single C file, no libraries.
- **GUI** in Python/Tkinter, packaged as a portable **AppImage**.

---

## Compatibility

Runs on mainstream **x86‑64** desktop distros — Linux Mint, Ubuntu, Debian,
Arch / Manjaro, Fedora, openSUSE — as long as these are present:

| Requirement | Notes |
|-------------|-------|
| `python3` + `tkinter` | GUI dependency, **not bundled**. Debian/Ubuntu/Mint: `sudo apt install python3-tk`. Arch: `sudo pacman -S tk`. Fedora: `sudo dnf install python3-tkinter`. |
| `pkexec` (polkit) | For the one‑time password prompt. Standard on desktops. |
| `libfuse2` | To mount the AppImage. If missing, run with `--appimage-extract-and-run`. |

**Not supported as‑is:** ARM devices (Raspberry Pi, Apple Silicon) and
non‑glibc systems (Alpine/musl) — those need a recompile of the C engine for the
target architecture (`cc -O2 -o mouse-remap mouse-remap.c`). The engine has no
library dependencies beyond the C runtime, so building it anywhere is trivial.

---

## Install

### Option A — AppImage (recommended)

Download `Mouse_Remap-x86_64.AppImage` from the
[Releases](../../releases) page, then:

```bash
chmod +x Mouse_Remap-x86_64.AppImage
./Mouse_Remap-x86_64.AppImage
```

It asks for your password once (via `pkexec`) because grabbing the mouse
requires root. Requires `python3` with `tkinter` (present on most desktops;
on Debian/Ubuntu/Mint: `sudo apt install python3-tk`).

### Option B — build from source

```bash
git clone https://github.com/<you>/mouse-remap.git
cd mouse-remap
cc -O2 -o mouse-remap mouse-remap.c     # build the engine
python3 mouse-remap-gui.py               # launch the GUI
```

---

## Using the GUI

1. **Profile** — choose a saved profile, or **Save / Save As / New / Delete**.
2. **Mouse** — pick your device (auto‑detect marks the best guess).
3. **Button mappings** — one dropdown per button/scroll choosing its action.
4. **Hold time** — tap‑vs‑hold threshold for any `tapdouble` mapping.
5. **Start / Stop**. Changing any control while running re‑applies it instantly.

Closing the window stops remapping. The AppImage also installs nicely into your
app menu if you integrate it (e.g. with Gear Lever or AppImageLauncher).

---

## Command‑line use

The engine runs fine on its own:

```bash
sudo ./mouse-remap                       # auto‑pick the best mouse, default mapping
sudo ./mouse-remap /dev/input/event12    # target a specific device
sudo ./mouse-remap --config my.conf      # use a config file
sudo ./mouse-remap --hold-ms 150         # override the hold threshold
sudo ./mouse-remap --list                # list candidate mice with scores
sudo ./mouse-remap --watch               # show which device fires (never grabs)
```

Stop it with **Ctrl‑C**, or simply unplug/replug the mouse.

### Config format

One `source=action` per line (`#` comments allowed):

```ini
# sources:  left  right  middle  side  extra  wheel_up  wheel_down
# actions:
#   none
#   click:left | click:right | click:middle
#   double:left | double:right | double:middle
#   hold:left | hold:right | hold:middle          (press‑through, enables drag)
#   tapdouble:left | tapdouble:right | ...         (tap = double, hold = held button)
#   scroll:up | scroll:down
left=tapdouble:left
right=hold:right
middle=click:right
side=hold:left
extra=hold:left
wheel_up=click:right
wheel_down=click:right
hold_ms=150
```

Movement and any unmapped button always pass through untouched.

---

## Start automatically at login (optional)

Save your mapping once from the GUI (it writes `~/.config/mouse-remap/active.conf`),
then create a systemd service. Use a stable `/dev/input/by-id/...` path so it
survives replugging — find yours with `ls /dev/input/by-id/`.

```ini
# /etc/systemd/system/mouse-remap.service
[Unit]
Description=Mouse button remapper
After=multi-user.target

[Service]
ExecStart=/usr/local/bin/mouse-remap \
  --config /home/USER/.config/mouse-remap/active.conf \
  /dev/input/by-id/usb-YOUR_MOUSE-event-mouse
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo cp mouse-remap /usr/local/bin/
sudo systemctl enable --now mouse-remap.service
```

---

## Troubleshooting

**The mouse feels stuck / a click does nothing.** The engine has exclusive
control while running. Stop it: focus the terminal and press **Ctrl‑C**,
**unplug/replug the mouse**, or switch to a TTY (**Ctrl+Alt+F3**), log in, run
`sudo pkill mouse-remap`, and return (**Ctrl+Alt+F1**).

**It grabbed the wrong device.** Some keyboards expose a fake “mouse” interface.
Run `sudo ./mouse-remap --watch`, move your mouse, and use the device that
prints — then select it in the GUI or pass its path on the command line.

**“tkinter not found.”** Install it: `sudo apt install python3-tk`
(or your distro’s equivalent).

---

## How it works

```
physical mouse ──(EVIOCGRAB: exclusive read)──► mouse-remap (C)
                                                     │ translate per config
                                                     ▼
                              virtual uinput mouse ──► desktop (X11 / Wayland)
```

The engine grabs the real device so the desktop never sees raw events, then
emits translated events through a virtual mouse it creates. Tap‑vs‑hold is
decided purely by time, so a quick click always double‑clicks — even while
moving the pointer — and only a sustained hold engages a real button‑down.

---

## License

MIT — see [LICENSE](LICENSE).
