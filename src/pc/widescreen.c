/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "widescreen.h"
#include <math.h>
#include <dolphin/gx/GXAurora.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/tobj.h>

/* Widescreen is a framebuffer-shape change, not a viewport trick: Aurora sizes
 * the content framebuffer to the presented aspect and letterboxes it inside the
 * window, so logical 640x480 always maps onto the whole framebuffer with no
 * offset. Every mapping Aurora derives from that -- viewports, scissors and EFB
 * copy regions -- therefore stays consistent, and the only thing the game has to
 * do is divide the submitted horizontal projection term by the same factor. */

#define ORIGINAL_ASPECT (4.0f / 3.0f)

static int s_mode;
static bool s_supported;

static float pc_widescreen_target(void)
{
    u32 width, height;
    if (!s_mode || !s_supported) return ORIGINAL_ASPECT;
    if (s_mode == 1) return 16.0f / 9.0f;
    AuroraGetWindowSize(&width, &height);
    if (!width || !height) return ORIGINAL_ASPECT;
    return fmaxf(ORIGINAL_ASPECT, (float) width / height);
}

void pc_widescreen_set_mode(int mode)
{
    s_mode = mode >= 0 && mode <= 2 ? mode : 0;
    AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    pc_widescreen_update();
}

void pc_widescreen_set_scene(bool supported)
{
    s_supported = supported;
    pc_widescreen_update();
}

void pc_widescreen_update(void)
{
    AuroraSetPresentationAspect(pc_widescreen_target());
}

/* Derived from the framebuffer actually in use, so geometry stays consistent
 * while a requested aspect change is still working its way through. */
float pc_widescreen_scale(void)
{
    u32 width, height;
    if (HSD_GetCurrentRenderPass() != HSD_RP_SCREEN) return 1;
    AuroraGetRenderSize(&width, &height);
    if (!width || !height) return 1;
    return fmaxf(1.0f, ((float) width / height) / ORIGINAL_ASPECT);
}

float pc_widescreen_cobj_scale(struct HSD_CObj* cobj)
{
    if (cobj != NULL && (HSD_CObjGetFlags(cobj) & PC_COBJ_FILL_FRAME)) {
        return 1;
    }
    return pc_widescreen_scale();
}

void pc_widescreen_copy_efb(struct HSD_ImageDesc* idesc, int origx, int origy,
                            float center_x, int clear)
{
    float scale, left, right;
    u16 width;

    if (idesc == NULL) {
        return;
    }

    scale = pc_widescreen_scale();
    if (scale <= 1.0f) {
        HSD_ImageDescCopyFromEFB(idesc, origx, origy, clear, true);
        return;
    }

    left = origx;
    right = origx + idesc->width;
    pc_widescreen_widen(1.0f / scale, center_x, &left, &right);

    /* Borrowing idesc->width for the call keeps src == dst, which is what
     * makes Aurora resolve the mapped rect 1:1 instead of resampling it: the
     * copy comes back at the region's true physical size with the 4:3 aspect
     * the model's UVs expect. The declared width is restored immediately;
     * nothing else reads it between these two lines. */
    width = idesc->width;
    idesc->width = (u16) lroundf(right - left);
    HSD_ImageDescCopyFromEFB(idesc, (u16) lroundf(left), (u16) origy, clear,
                             true);
    idesc->width = width;
}