/*
 * VI layer. aurora provides the window/framebuffer (VIInit/VIConfigure/VIFlush);
 * retrace timing and the XFB flip are emulated here on top of aurora's frame
 * loop. Melee waits in HSD_VIWaitXFBFlush -> VIWaitForRetrace once per frame,
 * so VIWaitForRetrace is the frame boundary.
 */
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <stdlib.h>

#include "pc/pc.h"
#include "pc/launcher.h"
#include "pc/widescreen.h"

bool pc_exit_requested;

static u32 s_retrace_count;
static VIRetraceCallback s_pre_cb;
static VIRetraceCallback s_post_cb;
static void* s_next_fb;
static void* s_current_fb;
static BOOL s_black;
static bool s_in_frame;

void pc_os_run_alarms(void);
void aurora_heap_check(void);

void pc_frame_boundary(void)
{
    static int fps_log = -1;
    static u64 fps_t0;
    static u32 fps_n;

    if (s_in_frame) {
        aurora_end_frame();
        s_in_frame = false;
    }
    aurora_heap_check(); /* no-op unless MELEE_HEAP_CHECK is set */
    pc_widescreen_update(); /* Auto mode follows window resizes. */
    if (fps_log < 0) {
        fps_log = getenv("MELEE_FPS") != NULL;
        fps_t0 = SDL_GetTicks();
    }
    if (fps_log) {
        u64 now = SDL_GetTicks();
        fps_n++;
        if (now - fps_t0 >= 1000) {
            fprintf(stderr, "fps %.1f\n", fps_n * 1000.0 / (double) (now - fps_t0));
            fps_t0 = now;
            fps_n = 0;
        }
    }

    const AuroraEvent* event = aurora_update();
    while (event != NULL && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) {
            pc_exit_requested = true;
        } else if (event->type == AURORA_SDL_EVENT) {
            if (event->sdl.type == SDL_EVENT_KEY_DOWN &&
                event->sdl.key.scancode == SDL_SCANCODE_F1 && !event->sdl.key.repeat)
                pc_menu_toggle();
            pc_menu_event(&event->sdl);
            pc_keyboard_event(&event->sdl);
        }
        ++event;
    }
    pc_menu_update();
    /* Nothing draws while the overlay pauses the game, so hold the last
     * frame instead of clearing the EFB to black underneath the menu. */
    aurora_preserve_frame_buffer(pc_menu_is_open());
    pc_keyboard_apply();
    if (pc_exit_requested) {
        exit(0);
    }

    /* The game is a fixed 60 Hz simulation. Vsync normally paces it, but
     * when the window is not presentable (occluded, minimized) aurora skips
     * the present and the loop would free-run; pace it ourselves. */
    {
        static u64 next_ns;
        const u64 period = 1000000000ull / 60;
        u64 now = SDL_GetTicksNS();
        if (next_ns == 0 || now > next_ns + period) {
            next_ns = now; /* first frame, or we fell behind: resync */
        } else if (now < next_ns) {
            SDL_DelayPrecise(next_ns - now);
        }
        next_ns += period;
    }

    /* aurora_begin_frame returns false while minimized/paused; keep pumping.
     * Sleep a frame between attempts: without it a minimized window spins a
     * core at 100% polling SDL. */
    while (!aurora_begin_frame()) {
        event = aurora_update();
        while (event != NULL && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) {
                exit(0);
            }
            ++event;
        }
        SDL_Delay(16);
    }
    s_in_frame = true;

    s_retrace_count++;
    pc_os_run_alarms();
    if (s_pre_cb) {
        s_pre_cb(s_retrace_count);
    }
    s_current_fb = s_next_fb;
    if (s_post_cb) {
        s_post_cb(s_retrace_count);
    }
}

void VIWaitForRetrace(void)
{
    pc_frame_boundary();
    /* The overlay pauses the game. Melee's whole simulation hangs off this
     * call returning, so keep presenting frames and pumping input here and
     * simply do not hand one back until the menu closes. */
    while (pc_menu_is_open() && !pc_exit_requested) {
        pc_frame_boundary();
    }
}

u32 VIGetRetraceCount(void)
{
    return s_retrace_count;
}

u32 VIGetNextField(void)
{
    return s_retrace_count & 1;
}

u32 VIGetDTVStatus(void)
{
    return 0;
}

void* VIGetCurrentFrameBuffer(void)
{
    return s_current_fb;
}

void* VIGetNextFrameBuffer(void)
{
    return s_next_fb;
}

void VISetNextFrameBuffer(void* fb)
{
    s_next_fb = fb;
}

void VISetBlack(BOOL black)
{
    s_black = black;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = s_pre_cb;
    s_pre_cb = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = s_post_cb;
    s_post_cb = cb;
    return old;
}

u16 VIPadFrameBufferWidth(u16 width)
{
    return (u16) ((width + 15) & ~15);
}
