#!/usr/bin/env python3
"""key.py TITLE KEY[:HOLD_MS]... -- activate the window (KWin honours
_NET_ACTIVE_WINDOW where XSetInputFocus is ignored under Xwayland), then
press keys through XTest."""
import sys, time
from Xlib import X, XK, display
from Xlib.ext import xtest
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
ev = event.ClientMessage(window=win, client_type=dpy.intern_atom("_NET_ACTIVE_WINDOW"),
                         data=(32, [2, X.CurrentTime, 0, 0, 0]))
root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
dpy.sync()
time.sleep(0.15)
for spec in specs:
    key, _, hold = spec.partition(":")
    code = dpy.keysym_to_keycode(XK.string_to_keysym(key))
    xtest.fake_input(dpy, X.KeyPress, code)
    dpy.sync()
    time.sleep(int(hold or 120) / 1000)
    xtest.fake_input(dpy, X.KeyRelease, code)
    dpy.sync()
    time.sleep(0.1)
