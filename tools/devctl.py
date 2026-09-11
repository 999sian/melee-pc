#!/usr/bin/env python3
"""Drive the running game window for testing (Xwayland: run the game with
SDL_VIDEO_DRIVER=x11).

  devctl.py shot OUT.png            capture the game window
  devctl.py key KEY[:HOLD_MS] ...   press keys in sequence (keysym names,
                                    e.g. Return, x, Right); default hold 120ms
  devctl.py hold KEY MS             hold one key for MS milliseconds
"""
import subprocess
import sys
import time

from Xlib import X, XK, display
from Xlib.ext import xtest

WINDOW_NAME = "melee-pc"


def find_window(dpy):
    root = dpy.screen().root
    for wid in root.get_full_property(dpy.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType).value:
        w = dpy.create_resource_object("window", wid)
        if w.get_wm_name() == WINDOW_NAME:
            return w
    sys.exit("game window not found")


def send_key(dpy, win, keysym, down):
    code = dpy.keysym_to_keycode(XK.string_to_keysym(keysym))
    if not code:
        sys.exit(f"unknown key {keysym}")
    win.set_input_focus(X.RevertToParent, X.CurrentTime)
    xtest.fake_input(dpy, X.KeyPress if down else X.KeyRelease, code)
    dpy.sync()


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    dpy = display.Display()
    win = find_window(dpy)
    if cmd == "shot":
        subprocess.check_call(["import", "-window", hex(win.id), args[0]])
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
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
