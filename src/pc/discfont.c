/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Load the two HSD font atlases out of the user's own disc at boot.
 *
 * The decomp keeps HSD_DebugFontAtlas and HSD_SisLib_FontAtlas out of the
 * repository: they are pixel data from the retail DOL, not code. Upstream
 * recovers them with decomp-toolkit at build time, which would mean either
 * committing game data or shipping a build with no sislib glyphs -- and
 * sislib draws real menu text (gmtitle, gmresult, the character select).
 *
 * Since the port already has the disc open, lift them at runtime instead.
 * This is the same signature scan decomp-toolkit does, against main.dol read
 * through nod. No game data is stored anywhere; it lives only in the user's
 * own image.
 */
#include <nod.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/region.h"
#include <sysdolphin/baselib/hsd_3915.h>
#include <sysdolphin/baselib/sislib_font.h>

#include "pc/discfont.h"
#include "disc_open.h"

#define DOL_OFFSET_FIELD 0x420
#define FST_OFFSET_FIELD 0x424
#define DOL_MAX_SIZE (8u << 20)

/* HSD_DebugFontAtlas is 128 glyphs of 56 bytes; the blob is contiguous. */
#define DEBUG_FONT_BYTES (int) (sizeof(DebugFontGlyph) * 128)
#define SIS_GLYPH_BYTES (int) sizeof(TextGlyphTexture)
#define SIS_GLYPH_COUNT 287

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* Naive forward search; the DOL is a few MB and this runs once at boot. */
static long find_bytes(const u8* hay, long hay_len, const u8* needle,
                       long needle_len, long from)
{
    for (long i = from; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, (size_t) needle_len) == 0) {
            return i;
        }
    }
    return -1;
}

/* The debug font follows a 4-entry { s32, fn } glyph-callback table whose
 * first words are these four constants. */
static long find_debug_font(const u8* dol, long len)
{
    static const u32 marks[4] = { 0x10808000, 0x46808000, 0x7C808000,
                                  0xB3808000 };
    for (long i = 0; i + 0x20 + DEBUG_FONT_BYTES <= len; i += 4) {
        if (be32(dol + i) != marks[0]) {
            continue;
        }
        if (be32(dol + i + 8) == marks[1] && be32(dol + i + 16) == marks[2] &&
            be32(dol + i + 24) == marks[3])
        {
            return i + 0x20;
        }
    }
    return -1;
}

static bool load_from_dol(const u8* dol, long len, const char* game_id)
{
    long debug_off = find_debug_font(dol, len);
    if (debug_off < 0) {
        fprintf(stderr, "discfont: debug font glyph table not found\n");
        return false;
    }
    memcpy(HSD_DebugFontAtlas, dol + debug_off, (size_t) DEBUG_FONT_BYTES);

    /* The sislib atlas sits between the u8 kerning table and the 13 s32 xor
     * keys at lbl_80430BD0, minus two 0x8C-byte locals in between. The
     * kerning table's length is the only region-dependent part. */
    long kerning_len;
    if (memcmp(game_id, "GALE01", 6) == 0) {
        kerning_len = 0x240;
    } else if (memcmp(game_id, "GALP01", 6) == 0) {
        kerning_len = 0x140;
    } else {
        fprintf(stderr, "discfont: unsupported game id %.6s\n", game_id);
        return false;
    }

    static const u8 kern_sig[] = { 0x09, 0x08, 0x09, 0x0C,
                                   0x09, 0x08, 0x08, 0x08 };
    static const u8 keys_sig[] = {
        0, 0, 0, 0x26, 0, 0, 0, 0xFF, 0, 0, 0, 0xE8, 0, 0, 0, 0xEF,
        0, 0, 0, 0x42, 0, 0, 0, 0xD6, 0, 0, 0, 0x01, 0, 0, 0, 0x54,
        0, 0, 0, 0x14, 0, 0, 0, 0xA3, 0, 0, 0, 0x80, 0, 0, 0, 0xFD,
        0, 0, 0, 0x6E,
    };
    long kern = find_bytes(dol, len, kern_sig, (long) sizeof kern_sig, 0);
    long keys = find_bytes(dol, len, keys_sig, (long) sizeof keys_sig, 0);
    if (kern < 0 || keys < 0) {
        fprintf(stderr, "discfont: sislib font boundaries not found\n");
        return false;
    }

    long start = (kern + kerning_len + 31) & ~31L;
    long end = (keys - 0x8C) & ~31L;
    long glyphs = (end - start) / SIS_GLYPH_BYTES;
    if (glyphs <= 0 || glyphs > SIS_GLYPH_COUNT ||
        start + glyphs * SIS_GLYPH_BYTES > len)
    {
        fprintf(stderr, "discfont: implausible sislib glyph count %ld\n",
                glyphs);
        return false;
    }
    /* Fewer glyphs than the array holds (PAL) leaves the tail zeroed. */
    memcpy(HSD_SisLib_FontAtlas, dol + start,
           (size_t) (glyphs * SIS_GLYPH_BYTES));
    return true;
}

/* nod returns short reads on compressed images, so loop until satisfied. */
static bool read_exact(NodHandle* disc, u8* buf, long len)
{
    for (long done = 0; done < len;) {
        int64_t n = nod_read(disc, buf + done, (size_t) (len - done));
        if (n <= 0) {
            return false;
        }
        done += (long) n;
    }
    return true;
}

bool pc_load_disc_fonts(const char* disc_path)
{
    NodHandle* disc = NULL;
    if (pc_open_nod_disc(disc_path, &disc) != NOD_RESULT_OK || disc == NULL) {
        fprintf(stderr, "discfont: cannot open %s\n", disc_path);
        return false;
    }

    bool ok = false;
    u8* dol = NULL;
    u8 header[0x440];
    if (nod_seek(disc, 0, SEEK_SET) < 0 ||
        !read_exact(disc, header, (long) sizeof header))
    {
        fprintf(stderr, "discfont: cannot read disc header\n");
        goto done;
    }

    u32 dol_off = be32(header + DOL_OFFSET_FIELD);
    u32 fst_off = be32(header + FST_OFFSET_FIELD);
    /* main.dol is not in the FST; it runs from its header offset up to the
     * FST, which on every retail GC disc follows it. */
    if (fst_off <= dol_off || fst_off - dol_off > DOL_MAX_SIZE) {
        fprintf(stderr, "discfont: implausible DOL extent %u..%u\n", dol_off,
                fst_off);
        goto done;
    }
    long dol_size = (long) (fst_off - dol_off);

    dol = malloc((size_t) dol_size);
    if (dol == NULL) {
        goto done;
    }
    if (nod_seek(disc, dol_off, SEEK_SET) < 0 ||
        !read_exact(disc, dol, dol_size))
    {
        fprintf(stderr, "discfont: cannot read main.dol\n");
        goto done;
    }

    ok = load_from_dol(dol, dol_size, (const char*) header);

    pc_region_set((const char*) header);

done:
    free(dol);
    nod_free(disc);
    return ok;
}