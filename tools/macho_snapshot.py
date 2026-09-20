"""Size-preserving Mach-O section relabeling for rollback game objects.

Only section names change. Contents, relocations, symbol/section ordinals,
alignment and zero-fill flags remain byte-for-byte intact. This avoids relying
on objcopy's unsupported Mach-O section-renaming option. Fat objects are handled
slice by slice; unknown architectures/layouts fail closed.
"""
import struct
from pathlib import Path


class MachO:
    def __init__(self, data, base=0, length=None):
        self.data, self.base = data, base
        self.length = len(data)-base if length is None else length
        self.check(0, 32)
        header = self.unpack('<8I', 0)
        magic, cpu, _, self.filetype, count, size, _, _ = header
        if magic != 0xfeedfacf or cpu not in (0x01000007, 0x0100000c):
            raise ValueError('snapshots require 64-bit x86-64/arm64 Mach-O')
        self.check(32, size)
        self.sections, self.symbols = [], {}
        position = 32
        symtab = None
        for _ in range(count):
            command, length = self.unpack('<II', position)
            if length < 8 or position+length > 32+size:
                raise ValueError('invalid Mach-O load command')
            if command == 0x19:  # LC_SEGMENT_64
                if length < 72:
                    raise ValueError('truncated Mach-O segment')
                section_count = self.unpack('<I', position+64)[0]
                if 72+section_count*80 != length:
                    raise ValueError('invalid Mach-O section table')
                for index in range(section_count):
                    offset = position+72+index*80
                    name, segment, address, span = self.unpack('<16s16sQQ', offset)
                    flags = self.unpack('<I', offset+64)[0]
                    self.sections.append((offset, self.name(segment), self.name(name), address, span, flags))
            elif command == 2:  # LC_SYMTAB
                if length != 24:
                    raise ValueError('invalid Mach-O symbol table command')
                symtab = self.unpack('<4I', position+8)
            position += length
        if position != 32+size:
            raise ValueError('inconsistent Mach-O load command size')
        if symtab is None:
            raise ValueError('missing Mach-O symbol table (verify before stripping)')
        symbol_offset, symbol_count, strings, string_size = symtab
        self.check(symbol_offset, symbol_count*16)
        self.check(strings, string_size)
        self.commons = []
        for i in range(symbol_count):
            string, kind, section, _, value = self.unpack('<IBBHQ', symbol_offset+i*16)
            if string >= string_size:
                raise ValueError('invalid Mach-O symbol name offset')
            begin = self.base+strings+string
            end = data.find(b'\0', begin, self.base+strings+string_size)
            if end < 0:
                raise ValueError('unterminated Mach-O symbol name')
            name = bytes(data[begin:end]).decode('utf-8')
            if kind & 0xe0:  # STAB debugging entry
                continue
            if kind & 0x0e == 0 and value:
                self.commons.append(name)
            if kind & 0x0e in (0x0e, 0x02):
                self.symbols[name] = value

    @staticmethod
    def name(raw):
        return raw.rstrip(b'\0').decode('ascii')

    def check(self, offset, size):
        if offset < 0 or size < 0 or offset+size > self.length:
            raise ValueError('truncated Mach-O structure')

    def unpack(self, fmt, offset):
        self.check(offset, struct.calcsize(fmt))
        return struct.unpack_from(fmt, self.data, self.base+offset)


def images(data):
    if len(data) < 4:
        raise ValueError('truncated Mach-O header')
    magic = bytes(data[:4])
    if magic == b'\xcf\xfa\xed\xfe':
        return [MachO(data)]
    formats = {b'\xca\xfe\xba\xbe': ('>', False), b'\xbe\xba\xfe\xca': ('<', False),
               b'\xca\xfe\xba\xbf': ('>', True), b'\xbf\xba\xfe\xca': ('<', True)}
    if magic not in formats or len(data) < 8:
        raise ValueError('expected Mach-O object/image, not bitcode or another format')
    endian, wide = formats[magic]
    count = struct.unpack_from(endian+'I', data, 4)[0]
    stride = 32 if wide else 20
    table_end = 8+count*stride
    if count == 0 or table_end > len(data):
        raise ValueError('invalid universal Mach-O table')
    result, spans = [], []
    for i in range(count):
        entry = 8+i*stride
        start, size = struct.unpack_from(endian+('QQ' if wide else 'II'), data, entry+8)
        if start < table_end or start+size > len(data):
            raise ValueError('invalid universal Mach-O slice')
        if any(start < hi and lo < start+size for lo, hi in spans):
            raise ValueError('overlapping universal Mach-O slices')
        spans.append((start, start+size))
        result.append(MachO(data, start, size))
    return result


def rewrite(path):
    data = bytearray(Path(path).read_bytes())
    for image in images(data):
        if image.filetype != 1:
            raise ValueError('snapshot relabeling requires MH_OBJECT')
        if image.commons:
            raise ValueError('COMMON allocation escaped snapshot sections; compile with -fno-common')
        for offset, segment, name, _, _, flags in image.sections:
            if name.startswith('__melee_'):
                raise ValueError('object already uses reserved snapshot sections')
            if segment not in ('__DATA', '__DATA_CONST'):
                if segment not in ('__TEXT', '__DWARF', '__LD', '__LLVM'):
                    raise ValueError(f'uncovered Mach-O segment {segment},{name}')
                continue
            if name in ('__data', '__const') and flags & 0xff == 0:
                renamed = '__melee_data'
            elif name in ('__bss', '__common') and flags & 0xff == 1:
                renamed = '__melee_bss'
            else:
                raise ValueError(f'uncovered writable section {segment},{name} (flags {flags:#x})')
            # Names have fixed width, so no relocations/offsets need adjustment.
            start = image.base+offset
            data[start:start+32] = renamed.encode().ljust(16, b'\0') + b'__DATA'.ljust(16, b'\0')
    Path(path).write_bytes(data)


def verify(path, tracked, excluded):
    for image in images(Path(path).read_bytes()):
        ranges = []
        for name in ('__melee_data', '__melee_bss'):
            matches = [s for s in image.sections if s[1:3] == ('__DATA', name)]
            if len(matches) != 1 or matches[0][4] <= 0:
                raise ValueError(f'missing/empty rollback section {name}')
            _, _, _, start, size, _ = matches[0]
            ranges.append((start, start+size))
        if max(ranges[0][0], ranges[1][0]) < min(ranges[0][1], ranges[1][1]):
            raise ValueError('rollback sections overlap')
        for names, expected in ((tracked, True), (excluded, False)):
            for name in names:
                if '_'+name not in image.symbols:
                    raise ValueError(f'rollback verification is missing symbol {name}')
                address = image.symbols['_'+name]
                actual = any(lo <= address < hi for lo, hi in ranges)
                if actual != expected:
                    raise ValueError(f'rollback misplaced {name}: tracked={actual}, expected={expected}')
