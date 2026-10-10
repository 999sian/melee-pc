#!/usr/bin/env python3
"""Procedurally generate a level description for stagebuild.py.

    python procgen_stage.py 1985 > level.json
    python stagebuild.py level.json --disc Melee.iso --out mymod/

Each seed gives a different topology: a main island whose top is a run of
flat steps and slopes with a carved underside, optionally floating side
islands, and two to five pass-through platforms (some sloped), plus spawns,
item points, camera bounds and blast zones fitted around it."""
import json
import random
import sys


def generate(seed):
    rng = random.Random(seed)
    half = rng.uniform(55, 85)
    # top surface: symmetric run of segments from the left ledge to the middle
    segs = rng.randint(2, 4)
    xs = sorted(rng.uniform(-half + 12, -6) for _ in range(segs - 1))
    left = [(-half, 0.0)]
    y = 0.0
    for x in xs:
        y = max(-6.0, min(10.0, y + rng.choice([-1, 0, 1]) * rng.uniform(2, 7)))
        if rng.random() < 0.5:
            left.append((x, left[-1][1]))  # step: flat then a rise
        left.append((x + rng.uniform(2, 5), y))
    left.append((0.0, y))
    top = left[:-1] + [(-x, yy) for x, yy in reversed(left)]
    # underside: ledge lip, then a tapering, notched keel
    depth = rng.uniform(25, 55)
    lip = rng.uniform(4, 9)
    under = [(half, -lip), (half * rng.uniform(0.75, 0.9), -lip - rng.uniform(4, 10))]
    for k in range(rng.randint(1, 3)):
        fx = rng.uniform(0.3, 0.7) * (1 - k * 0.2)
        under.append((half * fx, -lip - depth * (0.4 + 0.2 * k)))
    under.append((half * rng.uniform(0.08, 0.2), -lip - depth))
    keel = under + [(-x, yy) for x, yy in reversed(under)]
    solids = [top + keel]

    floor_y = {}  # where things can stand, for spawns

    def surface(x):
        best = None
        for (x0, y0), (x1, y1) in zip(top, top[1:]):
            if min(x0, x1) <= x <= max(x0, x1) and x1 != x0:
                yy = y0 + (y1 - y0) * (x - x0) / (x1 - x0)
                best = yy if best is None else max(best, yy)
        return best if best is not None else 0.0

    islands = rng.random() < 0.55
    if islands:
        ix = half + rng.uniform(25, 40)
        iy = rng.uniform(5, 30)
        iw = rng.uniform(12, 20)
        for sgn in (-1, 1):
            c = sgn * ix
            solids.append([(c - iw, iy), (c + iw, iy), (c + iw * 0.6, iy - 8), (c, iy - 14), (c - iw * 0.6, iy - 8)])

    platforms = []
    count = rng.randint(2, 5)
    for k in range(count):
        if k == 0 or (count % 2 == 1 and k == count - 1):
            cx = 0.0
        else:
            cx = (half * rng.uniform(0.35, 0.7)) * (1 if k % 2 else -1)
        y = surface(cx) + rng.uniform(25, 40) + (25 if k >= 3 else 0)
        w = rng.uniform(14, 26)
        tilt = rng.choice([0, 0, 0, rng.uniform(-6, 6)])
        platforms.append([[round(cx - w, 2), round(y - tilt, 2)], [round(cx + w, 2), round(y + tilt, 2)]])
        if cx != 0 and k % 2:  # mirror side platforms
            platforms.append([[round(-cx - w, 2), round(y + tilt, 2)], [round(-cx + w, 2), round(y - tilt, 2)]])

    spawn_x = [-0.6 * half, 0.6 * half, -0.25 * half, 0.25 * half]
    spawns = [[round(x, 2), round(surface(x) + 1, 2)] for x in spawn_x]
    items = [[round(x, 2), round(surface(x) + 10, 2)] for x in (-0.8 * half, -0.4 * half, 0, 0.4 * half, 0.8 * half)]
    items += [[round((p[0][0] + p[1][0]) / 2, 2), round((p[0][1] + p[1][1]) / 2 + 10, 2)] for p in platforms[:3]]
    span = (half + (40 + 20 if islands else 0))
    top_y = max(p[1] for pl in platforms for p in pl) + 60
    return {
        'name': 'Test Level %d' % seed,
        'seed': seed,
        'base_file': 'GrNBa.dat',
        'solids': [[[round(x, 2), round(y, 2)] for x, y in s] for s in solids],
        'platforms': platforms,
        'spawns': spawns,
        'respawns': [[s[0], round(s[1] + 50, 2)] for s in spawns],
        'items': items[:8],
        'camera': [round(-span - 60, 1), round(top_y + 40, 1), round(span + 60, 1), round(-lip - depth - 20, 1)],
        'blastzone': [round(-span - 140, 1), round(top_y + 140, 1), round(span + 140, 1),
                      round(-lip - depth - 100, 1)],
    }


if __name__ == '__main__':
    print(json.dumps(generate(int(sys.argv[1]) if len(sys.argv) > 1 else 1), indent=1))
