#!/usr/bin/env python3
"""Move the P1 hand cursor on the character select screen to a target.

usage: csscursor.py X Y        (window pixels, 1280x960 frame)

Closed loop: screenshot, locate the white glove (largest white blob in the
lower two thirds of the screen), hold the stick toward the target, repeat.
The hotspot is the glove's top-left, which is where the fingertip is.
"""
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DEVCTL = os.path.join(HERE, "devctl.py")


def devctl(*args):
    subprocess.check_call([sys.executable, DEVCTL, *args])


def find_hand():
    """Returns the glove's fingertip in 1280x960 reference coordinates."""
    devctl("shot", "/tmp/_css.png")
    img = np.asarray(Image.open("/tmp/_css.png").convert("RGB")).astype(int)
    white = (img[:, :, 0] > 235) & (img[:, :, 1] > 235) & (img[:, :, 2] > 235)
    # the glove is a solid blob; text isn't. Erode to drop glyph strokes.
    h, w = white.shape
    scale = w / 1280.0
    r = max(1, round(3 * scale))
    e = white.copy()
    for dy, dx in ((0, r), (0, -r), (r, 0), (-r, 0)):
        e &= np.roll(np.roll(white, dy, 0), dx, 1)
    white = e
    # coarse grid; pick the densest block, then refine around it
    b = max(8, int(16 * scale))
    grid = white[: h // b * b, : w // b * b].reshape(h // b, b, w // b, b).sum(axis=(1, 3))
    gy, gx = np.unravel_index(np.argmax(grid), grid.shape)
    if grid[gy, gx] < b * b // 3:
        return None
    pad, win = int(80 * scale), int(96 * scale)
    y0, y1 = max(0, gy * b - pad), min(h, gy * b + win)
    x0, x1 = max(0, gx * b - pad), min(w, gx * b + win)
    ys, xs = np.nonzero(white[y0:y1, x0:x1])
    return int((xs.min() + x0) * 1280 / w), int((ys.min() + y0) * 960 / h)


def main():
    tx, ty = int(sys.argv[1]), int(sys.argv[2])
    # Portraits are ~90px wide and tolerate a loose approach; the port tags
    # are small, so callers aiming at one must ask for a tighter fit.
    tol = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    for _ in range(20):
        pos = find_hand()
        if pos is None:
            sys.exit("hand cursor not found")
        dx, dy = tx - pos[0], ty - pos[1]
        if abs(dx) < tol and abs(dy) < tol:
            print(f"at {pos}")
            return
        # ~1300 px/s at full deflection; short pulses so momentum stays small
        for delta, neg, posk in ((dx, "Left", "Right"), (dy, "Down", "Up")):
            if abs(delta) >= 12:
                ms = max(30, min(220, int(abs(delta) * 0.55)))
                devctl("hold", posk if (delta > 0) == (posk == "Right") else neg, str(ms))
        time.sleep(0.25)
    sys.exit(f"gave up at {pos} target {(tx, ty)}")


if __name__ == "__main__":
    main()
