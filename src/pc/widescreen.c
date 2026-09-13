#include "compat.h"
#include "widescreen.h"
#include <math.h>
#include <dolphin/gx/GXAurora.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/initialize.h>

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
