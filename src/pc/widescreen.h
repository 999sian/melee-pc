/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Opt-out flag for cameras whose projection box IS the screen rectangle and
 * whose job is to cover the whole frame (full-screen overlays). A HUD camera
 * is geometrically identical to one of these, so only intent separates them:
 * the HUD deliberately keeps its 4:3 layout and does NOT set this, while an
 * overlay that must reach both edges does.
 *
 * Bit 29 is free in HSD_CObj::flags -- 0, 1, 30 and 31 are taken, and CObjLoad
 * only ever reseeds the low bits from the disc desc, so a bit set after
 * HSD_CObjLoadDesc survives. */
#define PC_COBJ_FILL_FRAME (1u << 29)

struct HSD_CObj;
struct HSD_ImageDesc;

void pc_widescreen_set_mode(int mode);
void pc_widescreen_set_scene(bool supported);
void pc_widescreen_update(void);
float pc_widescreen_scale(void);

/* The horizontal scale setupNormalCamera actually divided this camera's
 * projection by. Anything that has to agree with the submitted matrix --
 * erase rects, projected-texture matrices, EFB copy rects -- must ask for it
 * here rather than calling pc_widescreen_scale() directly, so the fill-frame
 * opt-out stays in one place. */
float pc_widescreen_cobj_scale(struct HSD_CObj* cobj);

/* Scale one projection edge pair about `center` -- the widening is a scale
 * about the camera's horizontal centre, not about the world origin. */
static inline void pc_widescreen_widen(float scale, float center, float* lo,
                                       float* hi)
{
    *lo = center + (*lo - center) * scale;
    *hi = center + (*hi - center) * scale;
}

/* EFB -> texture copy of content drawn through a widened SCREEN-pass camera.
 * That camera's image sits in the centred 1/s of its viewport, so the picture
 * the game declared (idesc->width x idesc->height) is the source rect
 * contracted about `center_x` by the same factor. `center_x` is in logical
 * framebuffer pixels: the horizontal centre of the viewport that drew the
 * content. Identical to HSD_ImageDescCopyFromEFB at scale 1. */
void pc_widescreen_copy_efb(struct HSD_ImageDesc* idesc, int origx, int origy,
                            float center_x, int clear);
#ifdef __cplusplus
}
#endif