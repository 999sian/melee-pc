"""Send a simultaneous forward-smash input to the isolated test window."""
import time
from Xlib import display
from devctl import find_window, send_key
dpy = display.Display()
win = find_window(dpy)
for key in ("Right", "A"):
    send_key(dpy, win, key, True)
time.sleep(0.10)
for key in ("Right", "A"):
    send_key(dpy, win, key, False)
