#!/usr/bin/env python3
"""Report colour variety inside a region of a capture, in 1280x960 reference
coordinates.

Used to tell "a character was selected" from "something else changed": an
empty character-select card is a flat colour block, while a card holding a
portrait has many distinct colours. A plain frame-difference cannot make that
distinction -- it only sees that pixels moved, which is how a run that had
merely switched game mode got mistaken for a successful selection.

usage: regioncheck.py FRAME.png X0 Y0 X1 Y1 [MIN_COLOURS]
exits 0 if the region has at least MIN_COLOURS distinct quantised colours
"""
import sys

import numpy as np
from PIL import Image


def main():
    path = sys.argv[1]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
    need = int(sys.argv[6]) if len(sys.argv) > 6 else 40

    img = Image.open(path).convert("RGB")
    w, h = img.size
    sx, sy = w / 1280.0, h / 960.0
    crop = np.asarray(img.crop((int(x0 * sx), int(y0 * sy),
                                int(x1 * sx), int(y1 * sy))))
    # quantise to 5 bits per channel so anti-aliasing noise is not counted
    q = (crop // 8).reshape(-1, 3)
    distinct = len(np.unique(q, axis=0))
    print(f"{path} region=({x0},{y0})-({x1},{y1}) distinct_colours={distinct} need={need}")
    sys.exit(0 if distinct >= need else 1)


if __name__ == "__main__":
    main()
