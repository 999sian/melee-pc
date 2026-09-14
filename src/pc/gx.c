/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GX entry points aurora declares but does not implement. */
#include <dolphin/gx.h>

#include <string.h>

void GXSetTevClampMode(GXTevStageID stage, GXTevClampMode mode)
{
    (void) stage;
    (void) mode;
}

void GXSetMisc(GXMiscToken token, u32 val)
{
    (void) token;
    (void) val;
}

void GXSetVerifyLevel(int level)
{
    (void) level;
}

void GXAbortFrame(void) {}

static u16 s_draw_sync_token;

void GXSetDrawSync(u16 token)
{
    s_draw_sync_token = token;
}

u16 GXReadDrawSync(void)
{
    return s_draw_sync_token;
}

/* Fog range adjustment for non-square projections. Not implemented: fill the
 * table with the identity instead of zeroes. GXSetFogRangeAdj keeps the low
 * 12 bits of each entry, so 0xFFFF becomes 0xFFF and aurora's
 * build_fog_range_lut derives k = 63.98, giving sqrt(offset^2 + k^2)/k ~= 1
 * across the whole screen -- i.e. no adjustment, which is what this stub
 * means to express. Zeroes instead hit the k = max(k, 1e-6) clamp there and
 * scaled fog by up to 2e6, pinning every pixel to full fog colour. */
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, f32 projmtx[4][4])
{
    (void) width;
    (void) projmtx;
    memset(table, 0xFF, sizeof(*table));
}