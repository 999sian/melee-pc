/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * VI layer. aurora provides the window/framebuffer (VIInit/VIConfigure/VIFlush);
 * retrace timing and the XFB flip are emulated here on top of aurora's frame
 * loop. Melee waits in HSD_VIWaitXFBFlush -> VIWaitForRetrace once per frame,
 * so VIWaitForRetrace is the frame boundary.
 */
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/pc.h"
#include "pc/launcher.h"
#include "pc/touch.h"
#include "pc/widescreen.h"
#include "pc/net_lan.h"

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

void pc_frame_boundary(void) {
    static int fps_log = -1;
    static u64 fps_t0;
    static u32 fps_n;
    /* An average hides stutter: one 60ms hitch a second barely moves it.
     * Track the outliers instead, plus how far the pacing sleep overshoots
     * its request -- on Windows a coarse timer resolution turns a 5ms wait
     * into ~16ms, which is stutter the frame average never shows. */
    static u64 frame_prev_ns;
    static u64 frame_worst_ns;
    static u32 frame_late_20;
    static u32 frame_late_33;
    static u64 sleep_worst_over_ns;

    if (s_in_frame) {
        aurora_end_frame();
        s_in_frame = false;
    }
    aurora_heap_check();    /* no-op unless MELEE_HEAP_CHECK is set */
    pc_widescreen_update(); /* Auto mode follows window resizes. */
    /* MELEE_LAN_TEST=1|host: the LAN lobby without the menu; "host" starts
     * a match with the first peer found (src/pc/net_lan.c). */
    static int lan_test = -1;
    if (lan_test < 0) {
        const char* t = getenv("MELEE_LAN_TEST");
        lan_test = t == NULL ? 0 : strcmp(t, "host") == 0 ? 2 : 1;
    }
    if (lan_test) {
        pc_lan_poll();
        if (lan_test == 2 && pc_lan_state(NULL) == 0) {
            pc_lan_start_match();
        }
    }
    if (fps_log < 0) {
        fps_log = getenv("MELEE_FPS") != NULL;
        fps_t0 = SDL_GetTicks();
    }
    if (fps_log) {
        u64 now = SDL_GetTicks();
        u64 now_ns = SDL_GetTicksNS();
        fps_n++;
        if (frame_prev_ns != 0) {
            u64 delta = now_ns - frame_prev_ns;
            if (delta > frame_worst_ns) {
                frame_worst_ns = delta;
            }
            if (delta > 20000000ull) {
                frame_late_20++;
            }
            if (delta > 33000000ull) {
                frame_late_33++;
            }
            /* Put the stall in the main log too, where it sits next to
             * whatever aurora reported loading at that moment. An average
             * cannot tell a shader compile from a disc read; a timestamped
             * marker beside the surrounding records can. */
            if (delta > 50000000ull) {
                pc_log_line("STALL %.1fms at frame %u", delta / 1e6, s_retrace_count);
            }
        }
        frame_prev_ns = now_ns;
        if (now - fps_t0 >= 1000) {
            fprintf(stderr,
                "fps %.1f worst %.1fms late>20ms %u late>33ms %u "
                "sleep_overshoot %.1fms\n",
                fps_n * 1000.0 / (double)(now - fps_t0), frame_worst_ns / 1e6, frame_late_20,
                frame_late_33, sleep_worst_over_ns / 1e6);
            fflush(stderr);
            fps_t0 = now;
            fps_n = 0;
            frame_worst_ns = 0;
            frame_late_20 = 0;
            frame_late_33 = 0;
            sleep_worst_over_ns = 0;
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
            pc_touch_event(&event->sdl);
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

    /* Enforce deterministic 60 Hz simulation pacing regardless of display refresh rate
     * (e.g. 120 Hz, 144 Hz, 240 Hz high-refresh monitors). When VSync is enabled on high-refresh
     * displays, aurora_begin_frame() unblocks at monitor refresh rate. Without this check,
     * the simulation would run at 2x-4x speed. Pacing strictly to 60.000 Hz ensures physics,
     * hitboxes, and timers remain bit-identical. */
    static u64 next_sim_ns;
    const u64 sim_period = 1000000000ull / 60;
    u64 now = SDL_GetTicksNS();
    if (next_sim_ns == 0 || now > next_sim_ns + sim_period * 2) {
        next_sim_ns = now; /* first frame, or large hitch: resync */
    } else if (now < next_sim_ns) {
        const u64 want = next_sim_ns - now;
        /* On standard 60 Hz VSync, aurora_begin_frame already waited for VBlank. On high-refresh
         * (120/144/240 Hz) or VSync-off, this throttles simulation to exact 60 Hz. */
        if (!aurora_vsync_enabled() || want > 2000000ull) {
            SDL_DelayPrecise(want);
            if (fps_log > 0) {
                const u64 slept = SDL_GetTicksNS() - now;
                if (slept > want && slept - want > sleep_worst_over_ns) {
                    sleep_worst_over_ns = slept - want;
                }
            }
        }
    }
    next_sim_ns += sim_period;

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

void VIWaitForRetrace(void) {
    pc_frame_boundary();
    /* The overlay pauses the game. Melee's whole simulation hangs off this
     * call returning, so keep presenting frames and pumping input here and
     * simply do not hand one back until the menu closes. */
    while (pc_menu_is_open() && !pc_exit_requested) {
        pc_frame_boundary();
    }
}

u32 VIGetRetraceCount(void) {
    return s_retrace_count;
}

u32 VIGetNextField(void) {
    return s_retrace_count & 1;
}

u32 VIGetDTVStatus(void) {
    return 0;
}

void* VIGetCurrentFrameBuffer(void) {
    return s_current_fb;
}

void* VIGetNextFrameBuffer(void) {
    return s_next_fb;
}

void VISetNextFrameBuffer(void* fb) {
    s_next_fb = fb;
}

void VISetBlack(BOOL black) {
    s_black = black;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = s_pre_cb;
    s_pre_cb = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = s_post_cb;
    s_post_cb = cb;
    return old;
}

u16 VIPadFrameBufferWidth(u16 width) {
    return (u16)((width + 15) & ~15);
}
