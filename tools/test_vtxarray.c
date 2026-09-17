/* Real display-list scanner regression; no GPU required. */
/* The game also declares __assert, with the retail SDK signature. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/pc/vtxarray.c"

static u8 normals[36 * 301], texcoords[8 * 10];

static void indexed_nbt3(GXAttr attr, GXAttrType type) {
    HSD_VtxDescList desc[] = {{.attr = attr,
                                  .attr_type = type,
                                  .comp_cnt = GX_NRM_NBT3,
                                  .comp_type = GX_F32,
                                  .stride = 36},
        {.attr = GX_VA_TEX0,
            .attr_type = GX_INDEX8,
            .comp_cnt = GX_TEX_ST,
            .comp_type = GX_F32,
            .stride = 8},
        {.attr = GX_VA_NULL}};
    /* Two vertices; the largest normal index is the SECOND index of the
     * first vertex. A texcoord follows all three normal indices. */
    const u8 dl8[] = {GX_DRAW_POINTS, 0, 2, 1, 30, 2, 9, 3, 4, 5, 7};
    const u8 dl16[] = {GX_DRAW_POINTS, 0, 2, 0, 1, 1, 44, 0, 2, 9, 0, 3, 0, 4, 0, 5, 7};
    desc[0].vertex = (u32)(uintptr_t)normals;
    desc[1].vertex = (u32)(uintptr_t)texcoords;
    free(s_table);
    s_table = NULL;
    s_count = s_cap = 0;
    scan_display_list(
        desc, type == GX_INDEX8 ? dl8 : dl16, type == GX_INDEX8 ? sizeof(dl8) : sizeof(dl16));
    assert(pc_vtx_array_size(normals) == (type == GX_INDEX8 ? 1116 : 10836));
    assert(pc_vtx_array_size(texcoords) == 80);

    /* A truncated vertex batch must not read a partial NBT3 tuple. */
    free(s_table);
    s_table = NULL;
    s_count = s_cap = 0;
    scan_display_list(desc, dl8, 4);
    assert(pc_vtx_array_size(normals) == 36);
    assert(pc_vtx_array_size(texcoords) == 8);

    /* Ordinary one-index normals still leave the following UV aligned. */
    desc[0].comp_cnt = GX_NRM_XYZ;
    desc[0].attr_type = GX_INDEX8;
    const u8 single[] = {GX_DRAW_POINTS, 0, 2, 1, 9, 3, 7};
    scan_display_list(desc, single, sizeof(single));
    assert(pc_vtx_array_size(normals) == 144);
    assert(pc_vtx_array_size(texcoords) == 80);
}

int main(void) {
    indexed_nbt3(GX_VA_NRM, GX_INDEX8);
    indexed_nbt3(GX_VA_NRM, GX_INDEX16);
    indexed_nbt3(GX_VA_NBT, GX_INDEX8);
    indexed_nbt3(GX_VA_NBT, GX_INDEX16);
    free(s_table);
    puts("PASS: NBT3 index8/index16 array extents and following texture indices");
}
