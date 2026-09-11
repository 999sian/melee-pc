#ifndef SYSDOLPHIN_BASELIB_SPLINE_H
#define SYSDOLPHIN_BASELIB_SPLINE_H

#include <Runtime/platform.h>

#include <dolphin/mtx.h>

/* On-disc (reached from HSD_Joint.u.spline / stage data). */
typedef struct DISC_STRUCT HSD_Spline {
    /*  +0 */ u8 type;
    /*  +2 */ s16 numcv;
    /*  +4 */ f32 tension;
    /*  +8 */ DISC_PTR(DiscVec3) cv;
    /*  +C */ f32 totalLength;
    /* +10 */ DISC_PTR(DiscF32) segLength;
    /* +14 */ DISC_PTR(DiscF32) segPoly; /* f32[numcv-1][5] */
} HSD_Spline;
DISC_ASSERT_SIZE(HSD_Spline, 0x18);

f32 splGetHelmite(f32, f32, f32, f32, f32, f32);
void splGetSplinePoint(Vec3*, HSD_Spline*, f32);
f32 splArcLengthGetParameter(HSD_Spline*, f32);
void splArcLengthPoint(Vec3*, HSD_Spline*, f32);

#endif
