/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Vertex array sizes for aurora's GXSetArray.
 *
 * GameCube GX only needs an array base + stride; aurora uploads the whole
 * array, so it needs its byte size. Nothing on disc records that, so at
 * HSD_PObjLoadDesc time we walk each PObj's display list (a big-endian GX
 * command stream) and record, per indexed attribute array pointer,
 * (max index + 1) * stride in a pointer-keyed hash table.
 */
#include "pc/compat.h"
#include "pc/pc.h"

#include <dolphin/gx.h>
#include <sysdolphin/baselib/pobj.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    const void* key;
    u32 size;
} VtxArrayEntry;

static VtxArrayEntry* s_table;
static u32 s_cap; /* power of two */
static u32 s_count;

static u32 hash_ptr(const void* p) {
    uintptr_t h = (uintptr_t)p;
    h ^= h >> 17;
    h *= 0x9E3779B1u;
    h ^= h >> 15;
    return (u32)h;
}

static VtxArrayEntry* find_slot(VtxArrayEntry* table, u32 cap, const void* key) {
    u32 i = hash_ptr(key) & (cap - 1);
    while (table[i].key != NULL && table[i].key != key) {
        i = (i + 1) & (cap - 1);
    }
    return &table[i];
}

static void grow(void) {
    u32 new_cap = s_cap ? s_cap * 2 : 1024;
    VtxArrayEntry* new_table = calloc(new_cap, sizeof(VtxArrayEntry));
    u32 i;
    for (i = 0; i < s_cap; i++) {
        if (s_table[i].key != NULL) {
            *find_slot(new_table, new_cap, s_table[i].key) = s_table[i];
        }
    }
    free(s_table);
    s_table = new_table;
    s_cap = new_cap;
}

static void record(const void* data, u32 size) {
    VtxArrayEntry* e;
    if (s_count * 2 >= s_cap) {
        grow();
    }
    e = find_slot(s_table, s_cap, data);
    if (e->key == NULL) {
        e->key = data;
        s_count++;
    }
    if (size > e->size) {
        e->size = size;
    }
}

u32 pc_vtx_array_size(const void* data) {
    if (s_cap == 0) {
        return 0;
    }
    return find_slot(s_table, s_cap, data)->size;
}

/* Byte size of one GX_DIRECT attribute in the display list. */
static u32 direct_attr_size(const HSD_VtxDescList* d) {
    u32 n, elem;
    switch (d->attr) {
    case GX_VA_PNMTXIDX:
    case GX_VA_TEX0MTXIDX:
    case GX_VA_TEX1MTXIDX:
    case GX_VA_TEX2MTXIDX:
    case GX_VA_TEX3MTXIDX:
    case GX_VA_TEX4MTXIDX:
    case GX_VA_TEX5MTXIDX:
    case GX_VA_TEX6MTXIDX:
    case GX_VA_TEX7MTXIDX:
        return 1;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        switch (d->comp_type) {
        case GX_RGB565:
        case GX_RGBA4:
            return 2;
        case GX_RGB8:
        case GX_RGBA6:
            return 3;
        default: /* GX_RGBX8, GX_RGBA8 */
            return 4;
        }
    case GX_VA_POS:
        n = d->comp_cnt == GX_POS_XY ? 2 : 3;
        break;
    case GX_VA_NRM:
        /* GX_NRM_NBT/NBT3 carry normal+binormal+tangent: nine components, not
         * three. aurora's comp_cnt_count() sizes them that way, so a
         * three-component guess here would shift every following attribute
         * in the display list and mis-size all the indexed arrays. */
        n = d->comp_cnt == GX_NRM_XYZ ? 3 : 9;
        break;
    case GX_VA_NBT:
        n = 9;
        break;
    default: /* texcoords */
        n = d->comp_cnt == GX_TEX_S ? 1 : 2;
        break;
    }
    switch (d->comp_type) {
    case GX_U8:
    case GX_S8:
        elem = 1;
        break;
    case GX_U16:
    case GX_S16:
        elem = 2;
        break;
    default:
        elem = 4;
        break;
    }
    return n * elem;
}

static u32 attr_index_count(const HSD_VtxDescList* d) {
    return (d->attr == GX_VA_NRM || d->attr == GX_VA_NBT) && d->comp_cnt == GX_NRM_NBT3 ? 3 : 1;
}

static void scan_display_list(const HSD_VtxDescList* verts, const u8* dl, u32 length) {
    u32 max_idx[GX_VA_MAX_ATTR];
    u32 n_attr = 0, vtx_size = 0, l = 0, i;
    const HSD_VtxDescList* d;

    for (d = verts; d->attr != GX_VA_NULL && n_attr < GX_VA_MAX_ATTR; d++, n_attr++) {
        max_idx[n_attr] = 0;
        switch (d->attr_type) {
        case GX_NONE:
            break; /* attribute disabled: contributes no vertex bytes */
        case GX_DIRECT:
            vtx_size += direct_attr_size(d);
            break;
        case GX_INDEX8:
            vtx_size += attr_index_count(d);
            break;
        default: /* GX_INDEX16 */
            vtx_size += 2 * attr_index_count(d);
            break;
        }
    }

    while (l + 3 <= length) {
        u8 op = dl[l] & GX_OPCODE_MASK;
        u32 n, v;
        /* HSD emits nothing but draw primitives, followed by NOP padding to
         * the 32-byte chunk. Stop at anything that is not a primitive rather
         * than mis-reading its payload as a vertex batch. */
        if (op < GX_DRAW_QUADS || op > GX_DRAW_POINTS) {
            break;
        }
        n = (u32)dl[l + 1] << 8 | dl[l + 2];
        l += 3;
        if (l + n * vtx_size > length) {
            break;
        }
        for (v = 0; v < n; v++) {
            for (d = verts, i = 0; i < n_attr; d++, i++) {
                if (d->attr_type == GX_NONE) {
                    continue;
                }
                if (d->attr_type == GX_DIRECT) {
                    l += direct_attr_size(d);
                    continue;
                }
                /* NBT3 stores separate normal/binormal/tangent indices.
                 * Consume all three before the next attribute and retain
                 * the largest so the GPU upload contains every vector. */
                for (u32 j = 0; j < attr_index_count(d); j++) {
                    u32 idx;
                    if (d->attr_type == GX_INDEX8) {
                        idx = dl[l++];
                    } else {
                        idx = (u32)dl[l] << 8 | dl[l + 1];
                        l += 2;
                    }
                    if (idx > max_idx[i]) {
                        max_idx[i] = idx;
                    }
                }
            }
        }
    }

    for (d = verts, i = 0; i < n_attr; d++, i++) {
        if ((d->attr_type == GX_INDEX8 || d->attr_type == GX_INDEX16) && d->vertex != 0) {
            record(DP(void, d->vertex), (max_idx[i] + 1) * d->stride);
        }
    }
}

void pc_vtx_array_scan(const HSD_PObjDesc* desc) {
    const HSD_VtxDescList* verts = DP(HSD_VtxDescList, desc->verts);
    const u8* dl = DP(u8, desc->display);
    if (verts == NULL) {
        return;
    }
    /* A missing display list must NOT skip the recording pass. An array the
     * table has never seen reports size 0, and 0 is not a truncating
     * under-size like the 1 * stride floor is: aurora pushes a zero-length
     * storage range, so cachedRange.size stays 0 and the array is re-pushed
     * on every draw with a degenerate offset. Scanning with length 0 skips
     * the walk (no index is read, no byte of `dl` is touched) but still
     * records every indexed array at the floor. */
    scan_display_list(verts, dl, dl != NULL ? (u32)desc->n_display << 5 : 0);
}
