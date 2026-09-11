#!/usr/bin/env python3
"""Exit 0 if two captures differ materially, 1 if they look the same.

Used by the match harness to tell whether a button press actually advanced
the game (e.g. Start moved from character select to stage select) instead of
silently doing nothing.

usage: framediff.py A.png B.png [MIN_FRACTION]
"""
import sys

import numpy as np
from PIL import Image


def main():
    a, b = (np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)
            for p in sys.argv[1:3])
    thresh = float(sys.argv[3]) if len(sys.argv) > 3 else 0.15
    if a.shape != b.shape:
        sys.exit(0)
    changed = (np.abs(a - b).max(axis=2) > 24).mean()
    print(f"changed={changed:.3f} threshold={thresh}")
    sys.exit(0 if changed > thresh else 1)


if __name__ == "__main__":
    main()
