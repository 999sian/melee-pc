#!/usr/bin/env python3
"""Drive the running game window for testing (Xwayland: run the game with
SDL_VIDEO_DRIVER=x11).

  devctl.py shot OUT.png            capture the game window
  devctl.py key KEY[:HOLD_MS] ...   press keys in sequence (keysym names,
                                    e.g. Return, x, Right); default hold 120ms
  devctl.py hold KEY MS             hold one key for MS milliseconds

Keys may be logical GC buttons (A, B, X, Y, Z, L, R, Start, DUp...) which are
mapped per target, so one script can drive both the port and Dolphin:

  MELEE_WINDOW_TITLE  window name, matched as a substring
  MELEE_KEYMAP        "port" (default) or "dolphin"
"""
import os
import subprocess
import sys
import time

from Xlib import X, XK, display
from Xlib.ext import xtest

WINDOW_NAME = os.environ.get("MELEE_WINDOW_TITLE", "melee-pc")

# Logical GC button -> keysym. The two targets only differ on Y/Z/R.
KEYMAPS = {
    "port": {"Y": "v", "Z": "Tab", "R": "e"},
    "dolphin": {"Y": "s", "Z": "d", "R": "w"},
}
BUTTONS = {
    "A": "x", "B": "z", "X": "c", "L": "q", "Start": "Return",
    "Up": "Up", "Down": "Down", "Left": "Left", "Right": "Right",
    "CUp": "i", "CDown": "k", "CLeft": "j", "CRight": "l",
    "DUp": "t", "DDown": "g", "DLeft": "f", "DRight": "h",
}


def keysym_for(name):
    """Logical button name -> keysym; anything else is already a keysym."""
    table = dict(BUTTONS, **KEYMAPS[os.environ.get("MELEE_KEYMAP", "port")])
    return table.get(name, name)


def find_window(dpy):
    root = dpy.screen().root
    for wid in root.get_full_property(dpy.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType).value:
        w = dpy.create_resource_object("window", wid)
        name = w.get_wm_name()
        if name and WINDOW_NAME in name:
            return w
    sys.exit("game window not found")


def send_key(dpy, win, keysym, down):
    code = dpy.keysym_to_keycode(XK.string_to_keysym(keysym_for(keysym)))
    if not code:
        sys.exit(f"unknown key {keysym}")
    win.set_input_focus(X.RevertToParent, X.CurrentTime)
    xtest.fake_input(dpy, X.KeyPress if down else X.KeyRelease, code)
    dpy.sync()

def _import(win, out):
    for _ in range(5):  # capture occasionally yields an empty frame; retry
        subprocess.check_call(["import", "-window", hex(win.id), out])
        if os.path.getsize(out) > 4096:
            return open(out, "rb").read()
        time.sleep(0.2)
    return open(out, "rb").read()


def grab(dpy, win, out):
    """Capture the window, working around stale X drawables.

    Under Xwayland the window's drawable can keep returning the last
    composited frame while the game presents normally, which reads as a
    frozen game and is not one. Two identical captures 60ms apart are the
    tell; a one-pixel resize forces the compositor to hand back a fresh
    one. Only pay for the nudge when the frames actually match.
    """
    first = _import(win, out)
    if os.environ.get("MELEE_SHOT_NO_NUDGE"):
        # Capturing a game halted under gdb: two frames always match because
        # nothing is presenting, and the nudge cannot be repainted. The stale
        # drawable is exactly the frame we want.
        return
    time.sleep(0.06)
    if _import(win, out) != first:
        return
    geom = win.get_geometry()
    win.configure(width=geom.width + 1, height=geom.height)
    dpy.sync()
    time.sleep(0.25)
    win.configure(width=geom.width, height=geom.height)
    dpy.sync()
    time.sleep(0.25)
    _import(win, out)


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    dpy = display.Display()
    win = find_window(dpy)
    if cmd == "shot":
        grab(dpy, win, args[0])
    elif cmd == "key":
        for spec in args:
            key, _, hold = spec.partition(":")
            send_key(dpy, win, key, True)
            time.sleep(int(hold or 120) / 1000)
            send_key(dpy, win, key, False)
            time.sleep(0.12)
    elif cmd == "hold":
        send_key(dpy, win, args[0], True)
        time.sleep(int(args[1]) / 1000)
        send_key(dpy, win, args[0], False)
        time.sleep(0.05)
        send_key(dpy, win, args[0], False)  # releases get dropped now and then
    elif cmd == "combo":
        for spec in args:
            keys_part, _, hold = spec.partition(":")
            keys = keys_part.split("+")
            for k in keys:
                send_key(dpy, win, k, True)
            time.sleep(int(hold or 200) / 1000)
            for k in reversed(keys):
                send_key(dpy, win, k, False)
            time.sleep(0.12)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
