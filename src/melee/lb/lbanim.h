#ifndef GALE01_01E560
#define GALE01_01E560

#include <Runtime/platform.h>

#include <melee/lb/forward.h>
#include <sysdolphin/baselib/forward.h>

/* Pl*AJ.dat animation headers, read in place. */
struct DISC_STRUCT FigaTrack {
    u16 length;
    u16 startframe;
    u8 obj_type;
    u8 frac_value;
    u8 frac_slope;
    DISC_PTR(u8) ad_head;
};
DISC_ASSERT_SIZE(struct FigaTrack, 0xC);

struct DISC_STRUCT FigaTree {
    int type;
    u32 flags;
    f32 frames;
    DISC_PTR(s8) nodes;
    DISC_PTR(FigaTrack) tracks;
};
DISC_ASSERT_SIZE(struct FigaTree, 0x14);

/* 01E60C */ HSD_FObj* fn_8001E60C(FigaTrack*, s8 frames);
void lbAnim_8001E6D8(HSD_JObj*, FigaTree*, FigaTrack*, s8 frames);
void lbAnim_8001E7E8(HSD_JObj*, FigaTree*, FigaTrack*, s8 frames);
float lbAnim_8001E8F8(FigaTree*);

#endif
