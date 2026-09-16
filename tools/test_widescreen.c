#include "../src/pc/widescreen.c"
#include <stdlib.h>
#define assert(x)                                                                                  \
    do {                                                                                           \
        if (!(x))                                                                                  \
            abort();                                                                               \
    } while (0)
#include <stdio.h>
static u32 window_w = 1920, window_h = 1080;
static u32 render_w = 1920, render_h = 1080;
static float requested = -1;
static HSD_RenderPass test_pass = HSD_RP_SCREEN;
HSD_RenderPass HSD_GetCurrentRenderPass(void) {
    return test_pass;
}
void AuroraSetViewportPolicy(AuroraViewportPolicy policy) {
    (void)policy;
}
void AuroraGetRenderSize(u32* w, u32* h) {
    *w = render_w;
    *h = render_h;
}
void AuroraGetWindowSize(u32* w, u32* h) {
    *w = window_w;
    *h = window_h;
}
void AuroraSetPresentationAspect(float aspect) {
    requested = aspect;
}
static int test_hud_mode = 0;
int pc_get_hud_mode(void) {
    return test_hud_mode;
}
static void near(float a, float b) {
    assert(fabsf(a - b) < 0.001f);
}

/* The framebuffer follows the requested aspect, letterboxed in the window. */
static void present(void) {
    if (!window_w || !window_h || requested <= 0) {
        render_w = window_w;
        render_h = window_h;
        return;
    }
    if ((float)window_w / window_h > requested) {
        render_h = window_h;
        render_w = (u32)lroundf(window_h * requested);
    } else {
        render_w = window_w;
        render_h = (u32)lroundf(window_w / requested);
    }
}

static float scene(int mode, bool supported, u32 w, u32 h) {
    window_w = w;
    window_h = h;
    pc_widescreen_set_mode(mode);
    pc_widescreen_set_scene(supported);
    present();
    return pc_widescreen_scale();
}

int main(void) {
    /* Original mode and unsupported scenes present the classic aspect. */
    near(scene(0, true, 1920, 1080), 1);
    near(requested, 73.0f / 60.0f);
    near(render_w, 1314);
    near(render_h, 1080);
    near(scene(1, false, 1920, 1080), 1);
    near(requested, 73.0f / 60.0f);

    /* 16:9 widens by exactly the aspect ratio of the aspect ratios (320/219). */
    near(scene(1, true, 1920, 1080), (16.0f / 9) / (73.0f / 60.0f));
    near(requested, 16.0f / 9);
    near(render_w, 1920);
    near(render_h, 1080);

    /* 16:9 in a wider window letterboxes sideways, not vertically. */
    near(scene(1, true, 2560, 1080), (16.0f / 9) / (73.0f / 60.0f));
    near(render_w, 1920);
    near(render_h, 1080);

    /* 16:9 in a narrower window keeps 16:9 and letterboxes top/bottom. */
    near(scene(1, true, 1280, 800), (16.0f / 9) / (73.0f / 60.0f));
    near(render_w, 1280);
    near(render_h, 720);

    /* Auto follows the window and never narrows below the original aspect. */
    near(scene(2, true, 2560, 1080), (2560.0f / 1080) / (73.0f / 60.0f));
    near(requested, 2560.0f / 1080);
    near(render_w, 2560);
    near(render_h, 1080);
    near(scene(2, true, 800, 1000), 1);
    near(requested, 73.0f / 60.0f);
    near(render_w, 800);
    near(render_h, 658);

    /* Invalid modes and degenerate windows fall back to the original aspect. */
    near(scene(99, true, 1920, 1080), 1);
    near(requested, 73.0f / 60.0f);
    near(scene(2, true, 0, 0), 1);
    near(requested, 73.0f / 60.0f);

    /* Offscreen passes are never widened. */
    window_w = 2560;
    window_h = 1080;
    pc_widescreen_set_mode(2);
    pc_widescreen_set_scene(true);
    present();
    test_pass = HSD_RP_OFFSCREEN;
    near(pc_widescreen_scale(), 1);
    test_pass = HSD_RP_SCREEN;
    near(pc_widescreen_scale(), (2560.0f / 1080) / (73.0f / 60.0f));

    /* HUD horizontal offset: 0 in Classic (4:3) mode, (scale - 1) * 320 in Wide (16:9). */
    test_hud_mode = 0;
    scene(1, true, 1920, 1080);
    near(pc_widescreen_hud_offset(), 0.0f);

    test_hud_mode = 1;
    scene(0, true, 1920, 1080);
    near(pc_widescreen_hud_offset(), 0.0f);

    scene(1, true, 1920, 1080);
    near(pc_widescreen_hud_offset(), ((16.0f / 9.0f) / (73.0f / 60.0f) - 1.0f) * 320.0f);

    test_pass = HSD_RP_OFFSCREEN;
    near(pc_widescreen_hud_offset(), 0.0f);
    test_pass = HSD_RP_SCREEN;

    puts("PASS: widescreen aspect selection, framebuffer fit and scale");
}
