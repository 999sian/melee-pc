"""HSD (.dat) archive reading and editing, enough to rebuild Melee models.

Layout: 0x20 header (file size, data size, reloc count, root count, ref
count), data block, relocation table (u32 offsets of pointer words inside the
data block), root and reference tables (u32 data offset, u32 string offset),
string table. Pointers are offsets into the data block."""
import math
import struct

import numpy as np

GX_VA_PNMTXIDX, GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0, GX_VA_NULL = 0, 9, 10, 11, 13, 0xFF
GX_DIRECT, GX_INDEX8, GX_INDEX16 = 1, 2, 3
GX_POS_XYZ, GX_NRM_XYZ, GX_TEX_ST, GX_CLR_RGBA = 1, 0, 1, 1
GX_F32, GX_RGBA8 = 4, 5
GX_TF_RGBA8 = 6
GX_DRAW_TRIANGLES = 0x90


class Dat:
    def __init__(self, raw):
        fsize, dsize, nreloc, nroot, nref = struct.unpack('>5I', raw[:20])
        self.data = bytearray(raw[0x20:0x20 + dsize])
        o = 0x20 + dsize
        self.relocs = set(struct.unpack('>%dI' % nreloc, raw[o:o + nreloc * 4]))
        o += nreloc * 4
        strtab = o + (nroot + nref) * 8

        def name(off):
            e = raw.index(0, strtab + off)
            return raw[strtab + off:e].decode()
        self.roots = []
        for i in range(nroot):
            d, s = struct.unpack('>2I', raw[o + i * 8:o + i * 8 + 8])
            self.roots.append([d, name(s)])
        o += nroot * 8
        self.refs = []
        for i in range(nref):
            d, s = struct.unpack('>2I', raw[o + i * 8:o + i * 8 + 8])
            self.refs.append([d, name(s)])

    @classmethod
    def load(cls, path):
        return cls(open(path, 'rb').read())

    def root(self, name):
        for d, n in self.roots:
            if n == name:
                return d
        raise KeyError(name)

    # ---- reading
    def u32(self, o):
        return struct.unpack_from('>I', self.data, o)[0]

    def u16(self, o):
        return struct.unpack_from('>H', self.data, o)[0]

    def f32(self, o):
        return struct.unpack_from('>f', self.data, o)[0]

    def ptr(self, o):
        """Pointer stored at o, or None for null."""
        return self.u32(o) if o in self.relocs else None

    # ---- writing
    def alloc(self, size, align=32):
        while len(self.data) % align:
            self.data.append(0)
        off = len(self.data)
        self.data.extend(b'\0' * size)
        return off

    def put(self, blob, align=32):
        off = self.alloc(len(blob), align)
        self.data[off:off + len(blob)] = blob
        return off

    def set_u32(self, o, v):
        struct.pack_into('>I', self.data, o, v)

    def set_u16(self, o, v):
        struct.pack_into('>H', self.data, o, v)

    def set_f32(self, o, v):
        struct.pack_into('>f', self.data, o, v)

    def set_ptr(self, o, target):
        if target is None:
            self.set_u32(o, 0)
            self.relocs.discard(o)
        else:
            self.set_u32(o, target)
            self.relocs.add(o)

    def add_root(self, off, name):
        self.roots.append([off, name])

    def save(self, path):
        while len(self.data) % 32:
            self.data.append(0)
        relocs = sorted(self.relocs)
        strings = b''
        table = b''
        for d, n in self.roots + self.refs:
            table += struct.pack('>2I', d, len(strings))
            strings += n.encode() + b'\0'
        body = bytes(self.data) + struct.pack('>%dI' % len(relocs), *relocs) + table + strings
        header = struct.pack('>5I', 0x20 + len(body), len(self.data), len(relocs), len(self.roots),
                             len(self.refs)) + b'\0' * 12
        open(path, 'wb').write(header + body)

    # ---- joints
    def joint_tree(self, root):
        """Depth-first pre-order joint offsets (the game's joint index order)."""
        out, parent = [], {}

        def walk(j, par):
            while j is not None:
                out.append(j)
                parent[j] = par
                walk(self.ptr(j + 8), j)
                j = self.ptr(j + 0xC)
        walk(root, None)
        return out, parent

    def joint_local(self, j):
        rx, ry, rz = struct.unpack_from('>3f', self.data, j + 0x14)
        sx, sy, sz = struct.unpack_from('>3f', self.data, j + 0x20)
        tx, ty, tz = struct.unpack_from('>3f', self.data, j + 0x2C)
        return srt_matrix((sx, sy, sz), (rx, ry, rz), (tx, ty, tz))

    def joint_worlds(self, root):
        joints, parent = self.joint_tree(root)
        world = {}
        for j in joints:
            m = self.joint_local(j)
            world[j] = world[parent[j]] @ m if parent[j] is not None else m
        return joints, parent, world

    def dobjs(self, j):
        if self.u32(j + 4) & ((1 << 14) | (1 << 5)):  # spline / particle joint
            return []
        out = []
        d = self.ptr(j + 0x10)
        while d is not None:
            out.append(d)
            d = self.ptr(d + 4)
        return out

    def pobjs(self, d):
        out = []
        p = self.ptr(d + 0xC)
        while p is not None:
            out.append(p)
            p = self.ptr(p + 4)
        return out


def srt_matrix(s, r, t):
    """HSD joint matrix: scale, then rotate X, Y, Z, then translate."""
    cx, sx_ = math.cos(r[0]), math.sin(r[0])
    cy, sy_ = math.cos(r[1]), math.sin(r[1])
    cz, sz_ = math.cos(r[2]), math.sin(r[2])
    Rx = np.array([[1, 0, 0], [0, cx, -sx_], [0, sx_, cx]])
    Ry = np.array([[cy, 0, sy_], [0, 1, 0], [-sy_, 0, cy]])
    Rz = np.array([[cz, -sz_, 0], [sz_, cz, 0], [0, 0, 1]])
    M = np.eye(4)
    M[:3, :3] = Rz @ Ry @ Rx @ np.diag(s)
    M[:3, 3] = t
    return M


# ---- building new model pieces ---------------------------------------------

def gx_rgba8(img):
    """PIL RGBA image (w, h multiples of 4) -> GX_TF_RGBA8 bytes."""
    a = np.asarray(img.convert('RGBA'), np.uint8)
    h, w = a.shape[:2]
    out = bytearray(w * h * 4)
    o = 0
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            tile = a[ty:ty + 4, tx:tx + 4].reshape(16, 4)
            ar = np.stack([tile[:, 3], tile[:, 0]], 1).reshape(-1)
            gb = np.stack([tile[:, 1], tile[:, 2]], 1).reshape(-1)
            out[o:o + 32] = ar.tobytes()
            out[o + 32:o + 64] = gb.tobytes()
            o += 64
    return bytes(out)


def add_texture(dat, img, template_tobj=None, wrap=1):
    """Image desc + TObj for a PIL image; returns the TObj offset."""
    w, h = img.size
    pix = dat.put(gx_rgba8(img))
    imgd = dat.alloc(0x18, 4)
    dat.set_ptr(imgd, pix)
    dat.set_u16(imgd + 4, w)
    dat.set_u16(imgd + 6, h)
    dat.set_u32(imgd + 8, GX_TF_RGBA8)
    tobj = dat.alloc(0x5C, 4)
    if template_tobj is not None:
        dat.data[tobj:tobj + 0x5C] = dat.data[template_tobj:template_tobj + 0x5C]
        for f in (0, 4, 0x4C, 0x50, 0x54, 0x58):
            dat.set_ptr(tobj + f, None)
    else:
        dat.set_u32(tobj + 8, 0)       # GX_TEXMAP0
        dat.set_u32(tobj + 0xC, 4)     # GX_TG_TEX0
        for i in range(3):
            dat.set_f32(tobj + 0x1C + i * 4, 1.0)
        dat.set_u32(tobj + 0x40, 0x10 | (5 << 16))  # UV coords, LIGHTMAP_DIFFUSE, COLORMAP_REPLACE
        dat.set_f32(tobj + 0x44, 1.0)
        dat.set_u32(tobj + 0x48, 1)    # GX_LINEAR
    dat.set_u32(tobj + 0x34, wrap)
    dat.set_u32(tobj + 0x38, wrap)
    dat.data[tobj + 0x3C] = 1
    dat.data[tobj + 0x3D] = 1
    dat.set_ptr(tobj + 0x4C, imgd)
    return tobj


def add_material(dat, rendermode, tobj, diffuse=(255, 255, 255, 255), ambient=(128, 128, 128, 255)):
    mat = dat.alloc(0x14, 4)
    dat.data[mat:mat + 4] = bytes(ambient)
    dat.data[mat + 4:mat + 8] = bytes(diffuse)
    dat.data[mat + 8:mat + 12] = bytes((255, 255, 255, 255))
    dat.set_f32(mat + 12, 1.0)
    dat.set_f32(mat + 16, 50.0)
    mobj = dat.alloc(0x18, 4)
    dat.set_u32(mobj + 4, rendermode)
    dat.set_ptr(mobj + 8, tobj)
    dat.set_ptr(mobj + 0xC, mat)
    return mobj


def add_mesh_pobjs(dat, pos, nrm, uv, tris, flags=0):
    """Rigid PObjs (vertices in the owning joint's space) for one mesh;
    returns the first PObj, chained. Splits so indices fit in 16 bits."""
    first = prev = None
    MAXV = 60000
    start = 0
    ntri = len(tris)
    while start < ntri:
        # take triangles until their vertex set nears the limit
        used = {}
        end = start
        while end < ntri:
            new = [v for v in tris[end] if v not in used]
            if len(used) + len(new) > MAXV:
                break
            for v in new:
                used[v] = len(used)
            end += 1
        order = np.array(sorted(used, key=used.get), np.int64)
        remap = np.vectorize(used.get)
        sub = remap(tris[start:end]) if end > start else np.zeros((0, 3), np.int64)
        p = _pobj(dat, pos[order], nrm[order], uv[order], sub, flags)
        if prev is None:
            first = p
        else:
            dat.set_ptr(prev + 4, p)
        prev = p
        start = end
    return first


def _vtx(dat, desc_off, attr, cnt, data, stride):
    arr = dat.put(np.ascontiguousarray(data, '>f4').tobytes())
    dat.set_u32(desc_off, attr)
    dat.set_u32(desc_off + 4, GX_INDEX16)
    dat.set_u32(desc_off + 8, cnt)
    dat.set_u32(desc_off + 0xC, GX_F32)
    dat.data[desc_off + 0x10] = 0
    dat.set_u16(desc_off + 0x12, stride)
    dat.set_ptr(desc_off + 0x14, arr)


def _pobj(dat, pos, nrm, uv, tris, flags):
    vdesc = dat.alloc(0x18 * 4, 4)
    _vtx(dat, vdesc, GX_VA_POS, GX_POS_XYZ, pos, 12)
    _vtx(dat, vdesc + 0x18, GX_VA_NRM, GX_NRM_XYZ, nrm, 12)
    _vtx(dat, vdesc + 0x30, GX_VA_TEX0, GX_TEX_ST, uv, 8)
    dat.set_u32(vdesc + 0x48, GX_VA_NULL)
    dl = bytearray()
    flat = np.asarray(tris, np.int64).reshape(-1)
    for i in range(0, len(flat), 0xFFFF // 3 * 3):
        chunk = flat[i:i + 0xFFFF // 3 * 3]
        dl += struct.pack('>BH', GX_DRAW_TRIANGLES, len(chunk))
        idx = np.repeat(chunk, 3).astype('>u2')  # pos, nrm, tex share one index
        dl += idx.tobytes()
    while len(dl) % 32:
        dl.append(0)
    disp = dat.put(bytes(dl))
    p = dat.alloc(0x18, 4)
    dat.set_ptr(p + 8, vdesc)
    dat.set_u16(p + 0xC, flags)
    dat.set_u16(p + 0xE, len(dl) // 32)
    dat.set_ptr(p + 0x10, disp)
    return p


def add_dobj(dat, mobj, pobj):
    d = dat.alloc(0x10, 4)
    dat.set_ptr(d + 8, mobj)
    dat.set_ptr(d + 0xC, pobj)
    return d


def append_dobj(dat, joint, dobj):
    """Adds a DObj at the end of a joint's list (existing indices keep)."""
    last = None
    d = dat.ptr(joint + 0x10)
    while d is not None:
        last = d
        d = dat.ptr(d + 4)
    if last is None:
        dat.set_ptr(joint + 0x10, dobj)
    else:
        dat.set_ptr(last + 4, dobj)


def blank_pobjs(dat, joints):
    """Turns every existing primitive into GX NOPs: the geometry stays (DObj
    indices other data refers to keep working) but draws nothing."""
    n = 0
    for j in joints:
        for d in dat.dobjs(j):
            for p in dat.pobjs(d):
                disp = dat.ptr(p + 0x10)
                size = dat.u16(p + 0xE) * 32
                if disp is not None:
                    dat.data[disp:disp + size] = b'\0' * size
                    n += 1
    return n
