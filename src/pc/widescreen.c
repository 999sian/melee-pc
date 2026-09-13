#include "compat.h"
#include "widescreen.h"
#include <math.h>
#include <dolphin/gx/GXAurora.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/video.h>

static int s_mode;
static bool s_supported;

typedef struct { float x, y, width, height; } PcWideRect;

static PcWideRect pc_widescreen_fit(unsigned width, unsigned height, float aspect)
{
    PcWideRect r = {0};
    if (!width || !height || !isfinite(aspect) || aspect <= 0) return r;
    r.height = fminf(height, width / aspect);
    r.width = r.height * aspect;
    r.x = (width - r.width) * 0.5f;
    r.y = (height - r.height) * 0.5f;
    return r;
}

static float pc_widescreen_aspect(int mode, bool supported, float window, float original)
{
    if (!supported) return original;
    if (mode == 1) return fmaxf(original, 16.0f / 9.0f);
    if (mode == 2 && isfinite(window)) return fmaxf(original, window);
    return original;
}

void pc_widescreen_set_mode(int mode)
{
    s_mode = mode >= 0 && mode <= 2 ? mode : 0;
    AuroraSetViewportPolicy(s_mode ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
}

void pc_widescreen_set_scene(bool supported) { s_supported = supported; }

/* Horizontal widening factor shared by every on-screen camera of an eligible
 * scene. Widening projection and viewport by the same factor leaves each drawn
 * pixel where it was relative to the frame centre, so the HUD stays classic and
 * only the field of view grows sideways. */
float pc_widescreen_scale(void)
{
    u32 width, height;
    if (!s_mode || !s_supported || HSD_GetCurrentRenderPass() != HSD_RP_SCREEN) return 1;
    AuroraGetRenderSize(&width, &height);
    if (!width || !height) return 1;
    return pc_widescreen_aspect(s_mode, true, (float) width / height, 4.0f / 3.0f) / (4.0f / 3.0f);
}

/* Map a logical 640x480-space viewport and scissor into the presented frame. */
static void pc_widescreen_render(float scale, float vx, float vy, float vw, float vh,
    float sl, float st, float sr, float sb)
{
    u32 width, height;
    AuroraGetRenderSize(&width, &height);
    const GXRenderModeObj* mode = HSD_VIGetRenderMode();
    if (!width || !height || !mode->viWidth || !mode->viHeight) return;
    PcWideRect r = pc_widescreen_fit(width, height,
        pc_widescreen_aspect(s_mode, s_supported, (float) width / height, 4.0f / 3.0f));
    if (scale == 1) {
        /* Not widened: undo the stretch policy and keep the original aspect. */
        PcWideRect fit = pc_widescreen_fit((unsigned) lroundf(r.width),
            (unsigned) lroundf(r.height), 4.0f / 3.0f);
        fit.x += r.x;
        fit.y += r.y;
        r = fit;
    }
    float sx = r.width / mode->viWidth, sy = r.height / mode->viHeight;
    GXSetViewportRender(r.x + vx * sx, r.y + vy * sy, vw * sx, vh * sy, 0, 1);
    float left = fmaxf(0, r.x + sl * sx);
    float top = fmaxf(0, r.y + st * sy);
    float right = fminf(width, r.x + sr * sx);
    float bottom = fminf(height, r.y + sb * sy);
    GXSetScissorRender((u32) left, (u32) top,
        (u32) fmaxf(0, right - left), (u32) fmaxf(0, bottom - top));
}

void pc_widescreen_apply_camera(HSD_CObj* camera, float projection[4][4])
{
    /* Offscreen passes render into their own targets and keep GX's own mapping. */
    if (!s_mode || HSD_GetCurrentRenderPass() != HSD_RP_SCREEN) return;
    float scale = pc_widescreen_scale();
    pc_widescreen_render(scale, camera->viewport.xmin, camera->viewport.ymin,
        camera->viewport.xmax - camera->viewport.xmin,
        camera->viewport.ymax - camera->viewport.ymin,
        camera->scissor.left, camera->scissor.top,
        camera->scissor.right, camera->scissor.bottom);
    /* Only the submitted matrix changes. Camera queries keep original semantics. */
    projection[0][0] /= scale;
}

/* Cameraless HUD text resets the logical viewport to the whole framebuffer;
 * its ortho projection is built inline, so keep it in the original aspect. */
void pc_widescreen_apply_screen(void)
{
    if (!s_mode || HSD_GetCurrentRenderPass() != HSD_RP_SCREEN) return;
    const GXRenderModeObj* mode = HSD_VIGetRenderMode();
    pc_widescreen_render(1, 0, 0, mode->viWidth, mode->viHeight,
        0, 0, mode->viWidth, mode->viHeight);
}
