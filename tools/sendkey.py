#!/usr/bin/env python3
"""sendkey.py TITLE KEY[:HOLD_MS]... -- deliver synthetic X KeyPress/KeyRelease
events straight to the window whose name contains TITLE (no focus change, so
several instances driven by several scripts do not steal each other's keys)."""
import sys, time
from Xlib import X, XK, display
from Xlib.protocol import event

title, specs = sys.argv[1], sys.argv[2:]
dpy = display.Display()
root = dpy.screen().root
win = None
for wid in root.get_full_property(dpy.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType).value:
    w = dpy.create_resource_object("window", wid)
    name = w.get_wm_name()
    if name and title in name:
        win = w
        break
if win is None:
    sys.exit("window not found: " + title)


def send(kind, code):
    ev = kind(time=X.CurrentTime, root=root, window=win, same_screen=1, child=X.NONE,
              root_x=0, root_y=0, event_x=0, event_y=0, state=0, detail=code)
    win.send_event(ev, propagate=False)
    dpy.sync()


for spec in specs:
    key, _, hold = spec.partition(":")
    code = dpy.keysym_to_keycode(XK.string_to_keysym(key))
    send(event.KeyPress, code)
    time.sleep(int(hold or 120) / 1000)
    send(event.KeyRelease, code)
    time.sleep(0.1)
