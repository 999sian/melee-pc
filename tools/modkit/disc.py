"""Read files from a GameCube disc image (.iso/.gcm, or .ciso)."""
import struct


class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(8)
        self.ciso = head[:4] == b'CISO'
        if self.ciso:
            self.block = struct.unpack('<I', head[4:8])[0]
            present = self.f.read(0x8000 - 8)
            self.map = {}
            n = 0
            for i, b in enumerate(present):
                if b:
                    self.map[i] = n
                    n += 1
        fst_off, fst_size = struct.unpack('>2I', self.read(0x424, 8))
        fst = self.read(fst_off, fst_size)
        count = struct.unpack_from('>I', fst, 8)[0]
        names = fst[count * 12:]
        self.files = {}

        def name(o):
            return names[o:names.index(0, o)].decode('latin-1')

        def walk(start, end, prefix):
            i = start
            while i < end:
                w0, w1, w2 = struct.unpack_from('>3I', fst, i * 12)
                nm = prefix + name(w0 & 0xFFFFFF)
                if w0 >> 24:
                    walk(i + 1, w2, nm + '/')
                    i = w2
                else:
                    self.files[nm.lower()] = (w1, w2)
                    i += 1
        walk(1, count, '')

    def read(self, off, size):
        if not self.ciso:
            self.f.seek(off)
            return self.f.read(size)
        out = bytearray()
        while size > 0:
            blk, inner = divmod(off, self.block)
            take = min(size, self.block - inner)
            if blk in self.map:
                self.f.seek(0x8000 + self.map[blk] * self.block + inner)
                out += self.f.read(take)
            else:
                out += b'\0' * take
            off += take
            size -= take
        return bytes(out)

    def file(self, name):
        off, size = self.files[name.lower().lstrip('/')]
        return self.read(off, size)
