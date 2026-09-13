#!/usr/bin/env python3
"""Boot a private no-card session and require WM_DELETE_WINDOW to exit cleanly.

Usage: python3 tools/test_window_close.py /path/to/disc [--seconds 10]
Requires the same X11 display and python-xlib as devctl.py.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time

from Xlib import X, display, protocol

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("disc", type=Path)
parser.add_argument("--seconds", type=float, default=10)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
title = f"melee-close-test-{os.getpid()}"
dpy = display.Display()
with tempfile.TemporaryDirectory(prefix="melee-close-test-") as directory:
    env = dict(os.environ, SDL_VIDEO_DRIVER="x11", MELEE_WINDOW_TITLE=title,
               XDG_DATA_HOME=directory, XDG_CACHE_HOME=directory,
               MELEE_HEAP_CHECK="1")
    with tempfile.TemporaryFile(mode="w+") as log:
        proc = subprocess.Popen([str(root / "build/melee"), "--no-card",
                                 str(args.disc.resolve())], env=env,
                                stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 30
            win = None
            while time.monotonic() < deadline and proc.poll() is None:
                clients = dpy.screen().root.get_full_property(
                    dpy.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType)
                for wid in clients.value if clients else []:
                    candidate = dpy.create_resource_object("window", wid)
                    if candidate.get_wm_name() == title:
                        win = candidate
                        break
                if win is not None:
                    break
                time.sleep(0.1)
            assert win is not None, "game window never appeared"
            time.sleep(args.seconds)
            win.send_event(protocol.event.ClientMessage(
                window=win, client_type=dpy.intern_atom("WM_PROTOCOLS"),
                data=(32, [dpy.intern_atom("WM_DELETE_WINDOW"), 0, 0, 0, 0])))
            dpy.sync()
            assert proc.wait(timeout=20) == 0, "normal window close crashed"
        except Exception:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            log.seek(0)
            print(log.read())
            raise
        finally:
            dpy.close()
print("PASS: normal window close exits with status 0")
