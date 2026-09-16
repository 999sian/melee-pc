/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * THP video frames on PC.
 *
 * Melee ships its own PPC-tuned THP decoder (src/thp/THPDec.c: 32-bit
 * big-endian bitstream loads, dcbz tiling). aurora's THPVideoDecode consumes
 * the same THP-JPEG stream and writes the Y/U/V planes straight into GX I8
 * tile layout, which is what the game uploads as textures, so the PC build
 * uses it and skips the game's decode work buffer.
 */
#include <dolphin/thp.h>

#include <stdio.h>

#include "pc/pc.h"

void pc_thp_decode_frame(const void* jpeg, void* tile_y, void* tile_u, void* tile_v) {
    s32 rc = THPVideoDecode(jpeg, tile_y, tile_u, tile_v, NULL);
    if (rc != 0) {
        fprintf(stderr, "THPVideoDecode failed: %d\n", rc);
    }
}
