#!/usr/bin/env python3
"""Extract HSD_DebugFontAtlas / HSD_SisLib_FontAtlas from a Melee disc's sys/ dir.

The decomp keeps these two blobs out of the repository (decomp-toolkit extracts
them from the original DOL). This script finds them in the NTSC-U (GALE01) or
PAL (GALP01) DOL by data signatures and writes the .inc files included by
src/sysdolphin/baselib/{hsd_3915.c,sislib_font.c}.

usage: extract_fonts.py <disc sys dir containing boot.bin and main.dol> <src/sysdolphin/baselib>
"""
import struct
import sys
from pathlib import Path

NTSC_SIS_GLYPHS = 287
# Length of HSD_SisLib_8040CB00 (u8 kerning pairs) per region; the atlas
# starts at the next 32-byte boundary after it.
KERNING_LEN = {"GALE01": 0x240, "GALP01": 0x140}


def main(sys_dir: str, out_dir: str) -> None:
    sys_path = Path(sys_dir)
    game_id = sys_path.joinpath("boot.bin").read_bytes()[:6].decode("ascii")
    if game_id not in KERNING_LEN:
        sys.exit(f"unsupported game id {game_id}")
    dol = sys_path.joinpath("main.dol").read_bytes()

    # Debug font: immediately follows the 4-entry GlyphEntry table
    # { 0x10808000, fn }, { 0x46808000, fn }, { 0x7C808000, fn }, { 0xB3808000, fn }.
    i = 0
    table = -1
    while (i := dol.find(struct.pack(">I", 0x10808000), i)) >= 0:
        if (dol[i + 8:i + 12] == struct.pack(">I", 0x46808000)
                and dol[i + 16:i + 20] == struct.pack(">I", 0x7C808000)
                and dol[i + 24:i + 28] == struct.pack(">I", 0xB3808000)):
            table = i
            break
        i += 4
    if table < 0:
        sys.exit("debug font glyph table not found")
    debug_font = dol[table + 0x20:table + 0x20 + 0x1C00]

    # SisLib font atlas lies between the kerning table (starts 09 08 09 0C 09 08
    # 08 08) and two small local objects (0x8C bytes) that precede
    # lbl_80430BD0, a table of 13 s32 xor keys.
    kern = dol.find(bytes([0x09, 0x08, 0x09, 0x0C, 0x09, 0x08, 0x08, 0x08]))
    keys = dol.find(struct.pack(">13I", 0x26, 0xFF, 0xE8, 0xEF, 0x42, 0xD6, 0x01, 0x54, 0x14, 0xA3, 0x80, 0xFD, 0x6E))
    if kern < 0 or keys < 0:
        sys.exit("sislib font boundaries not found")
    atlas_start = (kern + KERNING_LEN[game_id] + 31) & ~31
    atlas_end = (keys - 0x8C) & ~31
    n = (atlas_end - atlas_start) // 512
    if n <= 0 or n > NTSC_SIS_GLYPHS:
        sys.exit(f"implausible sislib glyph count {n}")
    sis_font = dol[atlas_start:atlas_start + n * 512] + bytes(512 * (NTSC_SIS_GLYPHS - n))

    out = Path(out_dir)

    def write_inc(blob: bytes, name: str) -> None:
        with open(out / name, "w") as f:
            f.write(f"/* Extracted from {game_id} main.dol by tools/extract_fonts.py */\n")
            for j in range(0, len(blob), 16):
                f.write("    " + ", ".join(f"0x{b:02X}" for b in blob[j:j + 16]) + ",\n")

    write_inc(debug_font, "debug_font.inc")
    write_inc(sis_font, "sislib_font.inc")
    print(f"{game_id}: debug font @ {table + 0x20:#x}, sislib atlas @ {atlas_start:#x} ({n} glyphs)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
