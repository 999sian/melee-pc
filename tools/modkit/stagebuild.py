#!/usr/bin/env python3
"""Build a map pack stage file with new geometry from a level description.

    python stagebuild.py level.json --disc Melee.iso --out mymod/

The level (JSON) describes the stage in Melee units (a character is ~15 tall):

    {
      "base_file": "GrNBa.dat",          // base stage's file (Battlefield)
      "name": "My Level",
      "solids": [ [[x, y], ...], ... ],  // closed outlines of solid ground
      "platforms": [ [[x, y], ...], ... ], // pass-through platforms (polylines)
      "spawns": [[x, y] x4], "respawns": [[x, y] x4],
      "items": [[x, y], ...],            // up to 8 item spawn points
      "camera": [left, top, right, bottom],
      "blastzone": [left, top, right, bottom],
      "seed": 1                          // optional, shown on the preview
    }

Solids can be any shape -- slopes, steps, overhangs, several islands; each
edge becomes a floor, ceiling or wall line by its facing (floors up to 45
degrees), and floor edges that end at a wall get grabbable ledges. Platforms
are pass-through floors and may have several segments (slopes too). There is
no fixed vertex or line count: melee-pc sizes collision from the stage file.

The output is a copy of the base stage file with new collision, general
points (spawns, camera, blast zones, item spawns) and visible geometry built
from the collision with generated test textures, plus stage-select art
(sss_icon.png, sss_preview.png, sss_name.png) for the map pack."""
import argparse
import json
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import hsd
from disc import Disc
from hsd import Dat

FLOOR, CEILING, RIGHT_WALL, LEFT_WALL = 1, 2, 4, 8
LO_PLATFORM, LO_LEDGE = 0x100, 0x200
# map_head general point slots (Ground_801C2D24 ids)
SLOT_SPAWN, SLOT_RESPAWN, SLOT_ITEM = 0, 4, 127
SLOT_CAM_CENTER, SLOT_CAM_TL, SLOT_CAM_BR, SLOT_BLAST_TL, SLOT_BLAST_BR = 148, 149, 150, 151, 152


# ---- collision ---------------------------------------------------------------

def signed_area(poly):
    return 0.5 * sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
                     for i in range(len(poly)))


def classify(p0, p1):
    """Line type of a solid's edge walked clockwise (solid on the right)."""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    nx, ny = -dy, dx  # outward normal (left of the direction)
    if abs(ny) >= abs(nx):
        return FLOOR if ny > 0 else CEILING
    return RIGHT_WALL if nx > 0 else LEFT_WALL


def build_collision(level):
    verts, lines = [], []  # line: dict(v0, v1, kind, lo, prev, next)

    def vid(p):
        verts.append((float(p[0]), float(p[1])))
        return len(verts) - 1

    for poly in level.get('solids', []):
        poly = [tuple(p) for p in poly]
        if signed_area(poly) > 0:     # make it clockwise (y up)
            poly.reverse()
        ids = [vid(p) for p in poly]
        first = len(lines)
        n = len(ids)
        for i in range(n):
            lines.append(dict(v0=ids[i], v1=ids[(i + 1) % n], kind=classify(poly[i], poly[(i + 1) % n]),
                              lo=0, prev=first + (i - 1) % n, next=first + (i + 1) % n))
        for i in range(first, first + n):
            ln = lines[i]
            if ln['kind'] != FLOOR:
                continue
            if lines[ln['prev']]['kind'] != FLOOR or lines[ln['next']]['kind'] != FLOOR:
                ln['lo'] |= LO_LEDGE
    for plat in level.get('platforms', []):
        ids = [vid(p) for p in plat]
        first = len(lines)
        for i in range(len(ids) - 1):
            lines.append(dict(v0=ids[i], v1=ids[i + 1], kind=FLOOR, lo=LO_PLATFORM,
                              prev=first + i - 1 if i > 0 else -1,
                              next=first + i + 1 if i < len(ids) - 2 else -1))
    # the game keeps lines grouped: floors, ceilings, right walls, left walls
    order = sorted(range(len(lines)), key=lambda i: [FLOOR, CEILING, RIGHT_WALL, LEFT_WALL].index(lines[i]['kind']))
    new_index = {old: new for new, old in enumerate(order)}
    sorted_lines = []
    for old in order:
        ln = dict(lines[old])
        ln['prev'] = new_index[ln['prev']] if ln['prev'] >= 0 else -1
        ln['next'] = new_index[ln['next']] if ln['next'] >= 0 else -1
        sorted_lines.append(ln)
    ranges = []
    start = 0
    for kind in (FLOOR, CEILING, RIGHT_WALL, LEFT_WALL):
        count = sum(1 for ln in sorted_lines if ln['kind'] == kind)
        ranges.append((start, count))
        start += count
    ranges.append((0, 0))  # dynamic
    if len(verts) > 0xFFFF or len(sorted_lines) > 0x7FFF:
        raise SystemExit('too much collision: %d vertices (max 65535), %d lines (max 32767)'
                         % (len(verts), len(sorted_lines)))
    return verts, sorted_lines, ranges


def write_collision(d, verts, lines, ranges):
    cd = d.root('coll_data')
    vp = d.alloc(len(verts) * 8, 4)
    for i, (x, y) in enumerate(verts):
        struct.pack_into('>2f', d.data, vp + i * 8, x, y)
    lp = d.alloc(len(lines) * 16, 4)
    for i, ln in enumerate(lines):
        struct.pack_into('>2H4h2H', d.data, lp + i * 16, ln['v0'], ln['v1'], ln['prev'], ln['next'], -1, -1,
                         ln['kind'], ln['lo'])
    jp = d.alloc(0x28, 4)
    v = np.array(verts)
    lo, hi = v.min(0) - 10, v.max(0) + 10
    struct.pack_into('>10h4f2h', d.data, jp, *[x for r in ranges for x in r], lo[0], lo[1], hi[0], hi[1], 0,
                     len(verts))
    d.set_ptr(cd, vp)
    d.set_u32(cd + 4, len(verts))
    d.set_ptr(cd + 8, lp)
    d.set_u32(cd + 0xC, len(lines))
    for k, (s, c) in enumerate(ranges):
        struct.pack_into('>2h', d.data, cd + 0x10 + k * 4, s, c)
    d.set_ptr(cd + 0x24, jp)
    d.set_u32(cd + 0x28, 1)


# ---- general points ------------------------------------------------------------

def write_points(d, level):
    want = {}
    for k, p in enumerate(level.get('spawns', [])[:4]):
        want[SLOT_SPAWN + k] = p
    for k, p in enumerate(level.get('respawns', level.get('spawns', []))[:4]):
        want[SLOT_RESPAWN + k] = p
    for k, p in enumerate(level.get('items', [])[:8]):
        want[SLOT_ITEM + k] = p
    if 'camera' in level:
        l, t, r, b = level['camera']
        want[SLOT_CAM_TL], want[SLOT_CAM_BR] = (l, t), (r, b)
        want[SLOT_CAM_CENTER] = ((l + r) / 2, (t + b) / 2)
    if 'blastzone' in level:
        l, t, r, b = level['blastzone']
        want[SLOT_BLAST_TL], want[SLOT_BLAST_BR] = (l, t), (r, b)
    mh = d.root('map_head')
    entries, n = d.ptr(mh), d.u32(mh + 4)
    done = set()
    for e in range(n):
        root = d.ptr(entries + e * 12)
        pairs, count = d.ptr(entries + e * 12 + 4), d.u32(entries + e * 12 + 8)
        joints, parent, world = d.joint_worlds(root)
        for k in range(count):
            ji, slot = struct.unpack_from('>2h', d.data, pairs + k * 4)
            if slot not in want:
                continue
            j = joints[ji]
            pw = world[parent[j]] if parent[j] is not None else np.eye(4)
            target = np.array([want[slot][0], want[slot][1], 0.0, 1.0])
            local = np.linalg.inv(pw) @ target
            struct.pack_into('>3f', d.data, j + 0x2C, *local[:3])
            done.add(slot)
    missing = sorted(set(want) - done)
    if missing:
        print('warning: the base stage has no general point for slots', missing)


# ---- textures and meshes -----------------------------------------------------------

def font(size):
    for f in ('arialbd.ttf', 'arial.ttf', 'DejaVuSans-Bold.ttf'):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def checker(size, cells, c0, c1, grid=None, label=False):
    img = Image.new('RGBA', (size, size), c0)
    dr = ImageDraw.Draw(img)
    cs = size // cells
    for y in range(cells):
        for x in range(cells):
            if (x + y) % 2:
                dr.rectangle((x * cs, y * cs, x * cs + cs - 1, y * cs + cs - 1), fill=c1)
            if label:
                dr.text((x * cs + 6, y * cs + 4), '%s%d' % (chr(65 + x), y + 1), fill=(0, 0, 0, 255),
                        font=font(size // 10))
    if grid:
        for i in range(0, size, cs):
            dr.line((i, 0, i, size), fill=grid, width=2)
            dr.line((0, i, size, i), fill=grid, width=2)
    return img


def backdrop(size):
    t = np.linspace(0, 1, size)[:, None]
    a = np.zeros((size, size, 4), np.uint8)
    a[..., 0] = np.broadcast_to(40 + 60 * t, (size, size))
    a[..., 1] = np.broadcast_to(60 + 90 * t, (size, size))
    a[..., 2] = np.broadcast_to(120 + 120 * t, (size, size))
    a[..., 3] = 255
    img = Image.fromarray(a, 'RGBA')
    dr = ImageDraw.Draw(img)
    for i in range(0, size, size // 8):
        dr.line((i, 0, i, size), fill=(255, 255, 255, 90))
        dr.line((0, i, size, i), fill=(255, 255, 255, 90))
    return img


class Mesh:
    def __init__(self):
        self.p, self.n, self.uv, self.t = [], [], [], []

    def poly(self, pts, n, uvs):
        base = len(self.p)
        self.p += [tuple(p) for p in pts]
        self.n += [tuple(n)] * len(pts)
        self.uv += [tuple(u) for u in uvs]
        for k in range(1, len(pts) - 1):
            self.t.append((base, base + k, base + k + 1))

    def empty(self):
        return not self.t

    def arrays(self):
        return (np.array(self.p, float), np.array(self.n, float), np.array(self.uv, np.float32),
                np.array(self.t, np.int64))


def ear_clip(poly):
    pts = [np.array(p, float) for p in poly]
    idx = list(range(len(pts)))
    ccw = signed_area(poly) > 0

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    tris = []
    while len(idx) > 3:
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = pts[i0], pts[i1], pts[i2]
            cr = cross(a, b, c)
            if abs(cr) < 1e-9 or (cr > 0) != ccw:
                continue
            if any(j not in (i0, i1, i2) and
                   all((cross(*e, pts[j]) > 0) == ccw for e in ((a, b), (b, c), (c, a))) for j in idx):
                continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            break
        else:
            break  # degenerate: give up on the rest
    if len(idx) == 3:
        tris.append(tuple(idx))
    return tris


def build_meshes(level, depth=18.0, s=24.0):
    top, side, face, plats, back = Mesh(), Mesh(), Mesh(), Mesh(), Mesh()
    for poly in level.get('solids', []):
        poly = [tuple(map(float, p)) for p in poly]
        if signed_area(poly) > 0:
            poly.reverse()
        acc = 0.0
        for i in range(len(poly)):
            p0, p1 = poly[i], poly[(i + 1) % len(poly)]
            L = float(np.hypot(p1[0] - p0[0], p1[1] - p0[1]))
            if L < 1e-6:
                continue
            nrm = (-(p1[1] - p0[1]) / L, (p1[0] - p0[0]) / L, 0.0)
            mesh = top if classify(p0, p1) == FLOOR else side
            u0, u1 = acc / s, (acc + L) / s
            acc += L
            mesh.poly([(p0[0], p0[1], depth), (p1[0], p1[1], depth), (p1[0], p1[1], -depth), (p0[0], p0[1], -depth)],
                      nrm, [(u0, depth / s), (u1, depth / s), (u1, -depth / s), (u0, -depth / s)])
        for tri in ear_clip(poly):
            pts = [poly[k] for k in tri]
            uvs = [(x / s, -y / s) for x, y in pts]
            face.poly([(x, y, depth) for x, y in reversed(pts)], (0, 0, 1), list(reversed(uvs)))
            face.poly([(x, y, -depth) for x, y in pts], (0, 0, -1), uvs)
    for plat in level.get('platforms', []):
        for (x0, y0), (x1, y1) in zip(plat, plat[1:]):
            th, z = 2.5, 10.0
            u = lambda x: x / 16.0
            plats.poly([(x0, y0, z), (x1, y1, z), (x1, y1, -z), (x0, y0, -z)], (0, 1, 0),
                       [(u(x0), 0), (u(x1), 0), (u(x1), 1.25), (u(x0), 1.25)])
            plats.poly([(x0, y0 - th, -z), (x1, y1 - th, -z), (x1, y1 - th, z), (x0, y0 - th, z)], (0, -1, 0),
                       [(u(x0), 0), (u(x1), 0), (u(x1), 1.25), (u(x0), 1.25)])
            plats.poly([(x0, y0, z), (x0, y0 - th, z), (x1, y1 - th, z), (x1, y1, z)], (0, 0, 1),
                       [(u(x0), 0), (u(x0), 0.16), (u(x1), 0.16), (u(x1), 0)])
            plats.poly([(x1, y1, -z), (x1, y1 - th, -z), (x0, y0 - th, -z), (x0, y0, -z)], (0, 0, -1),
                       [(u(x1), 0), (u(x1), 0.16), (u(x0), 0.16), (u(x0), 0)])
        for (x, y), sgn in ((plat[0], -1), (plat[-1], 1)):
            plats.poly([(x, y, -10), (x, y - 2.5, -10), (x, y - 2.5, 10), (x, y, 10)][::sgn], (sgn, 0, 0),
                       [(0, 0), (0, 0.16), (1.25, 0.16), (1.25, 0)])
    l, t, r, b = level.get('blastzone', (-280, 250, 280, -140))
    back.poly([(l * 1.5, t * 1.3, -160), (r * 1.5, t * 1.3, -160), (r * 1.5, b * 1.6, -160), (l * 1.5, b * 1.6, -160)],
              (0, 0, 1), [(0, 0), (1, 0), (1, 1), (0, 1)])
    return [(top, checker(256, 4, (235, 140, 40, 255), (250, 200, 120, 255), (60, 30, 0, 255), True), 1),
            (side, checker(256, 4, (90, 90, 100, 255), (140, 140, 150, 255), (30, 30, 30, 255)), 1),
            (face, checker(256, 4, (90, 90, 100, 255), (140, 140, 150, 255), (30, 30, 30, 255)), 1),
            (plats, checker(128, 2, (40, 120, 230, 255), (220, 235, 255, 255), (10, 30, 80, 255)), 1),
            (back, backdrop(256), 0)]


def write_visuals(d, level, model_gobj=None):
    mh = d.root('map_head')
    gobjs, n = d.ptr(mh + 8), d.u32(mh + 0xC)
    best, best_count = None, -1
    for gi in range(n):
        root = d.ptr(gobjs + gi * 0x34)
        if root is None:
            continue
        joints, _ = d.joint_tree(root)
        count = sum(len(d.pobjs(x)) for j in joints for x in d.dobjs(j))
        if count > best_count:
            best, best_count = gi, count
        hsd.blank_pobjs(d, joints)
    target = d.ptr(gobjs + (model_gobj if model_gobj is not None else best) * 0x34)
    for mesh, img, wrap in build_meshes(level):
        if mesh.empty():
            continue
        p, nn, uv, t = mesh.arrays()
        tobj = hsd.add_texture(d, img, wrap=wrap)
        mobj = hsd.add_material(d, 0x14, tobj, ambient=(180, 180, 180, 255))
        hsd.append_dobj(d, target, hsd.add_dobj(d, mobj, hsd.add_mesh_pobjs(d, p, nn, uv, t)))
    d.set_u32(target + 4, d.u32(target + 4) | 0x80 | (1 << 18))


# ---- stage select art -----------------------------------------------------------------

def write_art(level, out_dir):
    solids = level.get('solids', [])
    pts = np.array([p for poly in solids for p in poly] + [p for pl in level.get('platforms', []) for p in pl])
    lo, hi = pts.min(0), pts.max(0)

    def draw(img, margin):
        w, h = img.size
        sc = min((w - 2 * margin) / max(hi[0] - lo[0], 1), (h - 2 * margin) / max(hi[1] - lo[1], 1))
        cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
        f = lambda p: (w / 2 + (p[0] - cx) * sc, h / 2 - (p[1] - cy) * sc)
        dr = ImageDraw.Draw(img)
        for poly in solids:
            dr.polygon([f(p) for p in poly], fill=(235, 140, 40, 255), outline=(60, 30, 0, 255))
        for pl in level.get('platforms', []):
            dr.line([f(p) for p in pl], fill=(220, 235, 255, 255), width=max(2, int(sc * 3)))
        return dr
    icon = Image.new('RGBA', (64, 56), (30, 50, 110, 255))
    draw(icon, 4)
    icon.save(os.path.join(out_dir, 'sss_icon.png'))
    prev = backdrop(256).resize((256, 192))
    dr = draw(prev, 16)
    if 'seed' in level:
        dr.text((8, 6), 'seed %s' % level['seed'], fill=(255, 255, 255, 255), font=font(14))
    prev.save(os.path.join(out_dir, 'sss_preview.png'))
    name = Image.new('RGBA', (224, 56), (0, 0, 0, 0))
    ImageDraw.Draw(name).text((6, 8), level.get('name', 'NEW STAGE').upper(), fill=(255, 255, 255, 255),
                              font=font(30))
    name.save(os.path.join(out_dir, 'sss_name.png'))


def build(level, disc_path, out_dir, out_name):
    disc = Disc(disc_path)
    d = Dat(disc.file(level.get('base_file', 'GrNBa.dat')))
    verts, lines, ranges = build_collision(level)
    write_collision(d, verts, lines, ranges)
    write_points(d, level)
    write_visuals(d, level, level.get('model_gobj'))
    os.makedirs(out_dir, exist_ok=True)
    d.save(os.path.join(out_dir, out_name))
    write_art(level, out_dir)
    kinds = {k: sum(1 for ln in lines if ln['kind'] == k) for k in (FLOOR, CEILING, RIGHT_WALL, LEFT_WALL)}
    print('%s: %d vertices, %d lines (floors %d, ceilings %d, walls %d/%d), %d ledges' % (
        out_name, len(verts), len(lines), kinds[FLOOR], kinds[CEILING], kinds[RIGHT_WALL], kinds[LEFT_WALL],
        sum(1 for ln in lines if ln['lo'] & LO_LEDGE)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('level')
    ap.add_argument('--disc', required=True)
    ap.add_argument('--out', default='.')
    ap.add_argument('--file', default='GrNw.dat', help='output stage file name')
    a = ap.parse_args()
    build(json.load(open(a.level)), a.disc, a.out, a.file)


if __name__ == '__main__':
    main()
