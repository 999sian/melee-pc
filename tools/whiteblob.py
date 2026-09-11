#!/usr/bin/env python3
"""Measure the largest solid near-white region in a frame.

Total white fraction is useless for comparing runs, because it is dominated
by which stage and characters the run happened to pick. The untextured-quad
artifact is specifically ONE big solid blob, so measuring the largest
connected white component isolates it from scattered stage highlights.

usage: whiteblob.py FRAME.png [FRAME.png ...]
prints "<path> <largest_blob_px> <fraction_of_frame>"
"""
import sys

import numpy as np
from PIL import Image


def largest_white_blob(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
    white = (a > 242).all(axis=2)
    # Union-find is overkill; iterative label propagation on a downsampled
    # mask is plenty for "is there one big solid blob".
    small = white[::2, ::2]
    labels = np.zeros(small.shape, dtype=np.int32)
    nxt = 0
    best = 0
    visited = np.zeros_like(small, dtype=bool)
    ys, xs = np.nonzero(small)
    for y, x in zip(ys, xs):
        if visited[y, x]:
            continue
        nxt += 1
        stack = [(y, x)]
        visited[y, x] = True
        size = 0
        while stack:
            cy, cx = stack.pop()
            size += 1
            labels[cy, cx] = nxt
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx_ = cy + dy, cx + dx
                if (0 <= ny < small.shape[0] and 0 <= nx_ < small.shape[1]
                        and small[ny, nx_] and not visited[ny, nx_]):
                    visited[ny, nx_] = True
                    stack.append((ny, nx_))
        best = max(best, size)
    return best * 4, (best * 4) / white.size  # undo the 2x2 downsample


def main():
    for p in sys.argv[1:]:
        px, frac = largest_white_blob(p)
        print(f"{p} {px} {frac:.4f}")


if __name__ == "__main__":
    main()
