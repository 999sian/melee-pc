#!/usr/bin/env python3
"""Detect the in-match pause overlay. Exits 0 if paused, 1 if not.

usage: is_paused.py FRAME.png [THRESHOLD]

Positive detection of the pause banner, rather than inferring pause from a
static picture or from a movement probe. Both of those are unsound: a live
match can be momentarily static, and a character against a wall or in
hitstun will not visibly move -- in which case the corrective Start PAUSES a
running match and the two states oscillate.

The banner occupies a fixed top-right band. Measured separation is wide:
paused ~0.185 near-white coverage, playing ~0.041.
"""
import sys

import numpy as np
from PIL import Image

THRESHOLD = 0.12


def paused_fraction(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    band = im.crop((int(0.52 * w), int(0.05 * h), int(0.90 * w), int(0.15 * h)))
    return float(((np.asarray(band) > 225).all(axis=2)).mean())


def main():
    frac = paused_fraction(sys.argv[1])
    thresh = float(sys.argv[2]) if len(sys.argv) > 2 else THRESHOLD
    print(f"pause_band={frac:.4f} threshold={thresh}")
    sys.exit(0 if frac > thresh else 1)


if __name__ == "__main__":
    main()
