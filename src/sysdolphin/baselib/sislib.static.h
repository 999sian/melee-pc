#ifndef SYSDOLPHIN_BASELIB_SISLIB_STATIC_H
#define SYSDOLPHIN_BASELIB_SISLIB_STATIC_H

#include <Runtime/platform.h>

/// Shared by the sislib translation units.
static inline u8* HSD_SisLib_BytePtr(void* ptr)
{
    return ptr;
}

/* Glyph encoding. NTSC-U discs use two bytes per glyph (0x20xx atlas, 0x40xx
 * a font's own textures). PAL discs use one: 0x20 is a space (opcode 26
 * here) and any other byte b >= 0x21 is atlas glyph b - 0x21. */
#ifdef TARGET_PC
#include "pc/region.h"
static inline bool sis_pal(void) { return pc_region_is_pal(); }
#else
static inline bool sis_pal(void) { return false; }
#endif
static inline u8 sis_opcode(const u8* p) { return (sis_pal() && *p == 0x20) ? 26 : *p; }
static inline int sis_glyph_len(void) { return sis_pal() ? 1 : 2; }
static inline u16 sis_glyph(const u8* p)
{
    return sis_pal() ? (u16) (0x2000 + *p - 0x21) : (u16) ((p[0] << 8) | p[1]);
}

#endif
