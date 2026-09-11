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
    devctl("shot", "/tmp/_css.png")
    img = np.asarray(Image.open("/tmp/_css.png").convert("RGB")).astype(int)
    white = (img[:, :, 0] > 235) & (img[:, :, 1] > 235) & (img[:, :, 2] > 235)
    white[: img.shape[0] // 4] = False  # skip the "MELEE / VS" header
    # coarse 16px grid; pick the densest block, then refine around it
    b = 16
    h, w = white.shape
    grid = white[: h // b * b, : w // b * b].reshape(h // b, b, w // b, b).sum(axis=(1, 3))
    gy, gx = np.unravel_index(np.argmax(grid), grid.shape)
    if grid[gy, gx] < b * b // 3:
        return None
    y0, y1 = max(0, gy * b - 80), min(h, gy * b + 96)
    x0, x1 = max(0, gx * b - 80), min(w, gx * b + 96)
    ys, xs = np.nonzero(white[y0:y1, x0:x1])
    return int(xs.min() + x0), int(ys.min() + y0)


def main():
    tx, ty = int(sys.argv[1]), int(sys.argv[2])
    for _ in range(14):
        pos = find_hand()
        if pos is None:
            sys.exit("hand cursor not found")
        dx, dy = tx - pos[0], ty - pos[1]
        if abs(dx) < 12 and abs(dy) < 12:
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
