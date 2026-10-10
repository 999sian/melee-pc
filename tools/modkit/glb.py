"""Minimal glTF 2.0 binary (.glb) reader: meshes, skins, nodes, images."""
import json
import struct

import numpy as np

COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


class Glb:
    def __init__(self, path):
        data = open(path, 'rb').read()
        magic, ver, length = struct.unpack('<4sII', data[:12])
        assert magic == b'glTF'
        off = 12
        self.bin = b''
        while off < length:
            clen, ctype = struct.unpack('<I4s', data[off:off + 8])
            chunk = data[off + 8:off + 8 + clen]
            if ctype == b'JSON':
                self.j = json.loads(chunk)
            elif ctype == b'BIN\0':
                self.bin = chunk
            off += 8 + clen

    def view(self, idx):
        bv = self.j['bufferViews'][idx]
        o = bv.get('byteOffset', 0)
        return self.bin[o:o + bv['byteLength']], bv.get('byteStride')

    def accessor(self, idx):
        a = self.j['accessors'][idx]
        dt = COMP[a['componentType']]
        n = NCOMP[a['type']]
        raw, stride = self.view(a['bufferView'])
        off = a.get('byteOffset', 0)
        isz = np.dtype(dt).itemsize
        if stride and stride != n * isz:
            out = np.empty((a['count'], n), dt)
            for i in range(a['count']):
                out[i] = np.frombuffer(raw, dt, n, off + i * stride)
        else:
            out = np.frombuffer(raw, dt, a['count'] * n, off).reshape(a['count'], n)
        if a.get('normalized'):
            out = out.astype(np.float32) / np.iinfo(dt).max
        return out

    def image_bytes(self, idx):
        img = self.j['images'][idx]
        raw, _ = self.view(img['bufferView'])
        return raw, img.get('mimeType')

    def node_local(self, n):
        node = self.j['nodes'][n]
        if 'matrix' in node:
            return np.array(node['matrix'], np.float64).reshape(4, 4).T
        t = np.array(node.get('translation', [0, 0, 0]), np.float64)
        r = node.get('rotation', [0, 0, 0, 1])
        s = np.array(node.get('scale', [1, 1, 1]), np.float64)
        x, y, z, w = r
        R = np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        M = np.eye(4)
        M[:3, :3] = R * s
        M[:3, 3] = t
        return M

    def world_matrices(self):
        nodes = self.j['nodes']
        parent = {}
        for i, n in enumerate(nodes):
            for c in n.get('children', []):
                parent[c] = i
        cache = {}

        def w(i):
            if i in cache:
                return cache[i]
            m = self.node_local(i)
            if i in parent:
                m = w(parent[i]) @ m
            cache[i] = m
            return m
        return [w(i) for i in range(len(nodes))], parent
