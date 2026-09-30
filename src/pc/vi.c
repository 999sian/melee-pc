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

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern void browser_yield(void);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/pc.h"
#include "pc/launcher.h"
#include "pc/touch.h"
#include "pc/widescreen.h"
#include "pc/interp.h"
#include "pc/net.h"
#include "pc/net_chat.h"
#include <melee/if/ifnetchat.h>
#include <dolphin/pad.h>
#include "pc/net_lan.h"
#include "pc/net_match.h"
#include "pc/net_rank_session.h"

bool pc_exit_requested;

static u32 s_retrace_count;
/* When the next tick's frame boundary is due; the tick now running started
 * one period before it. */
static u64 next_sim_ns;
static u32 s_subframes; /* in-between frames presented, for MELEE_FPS */
/* In-between frames (src/pc/interp.c) are presented on a schedule, not as soon
 * as they are drawn: frame k of n goes out at tick start + margin + (k-1)/n of
 * the tick, the tick's exact frame last. Only the first frame waits on the
 * tick's logic, so presenting on completion alternates short and long gaps.
 * The margin follows how long logic plus that first draw really takes. */
static u64 s_present_margin_ns = 2000000ull;
static int s_tick_frames; /* frames this tick is split into; 0 = no schedule */

static VIRetraceCallback s_pre_cb;
static VIRetraceCallback s_post_cb;
static void* s_next_fb;
static void* s_current_fb;
static BOOL s_black;
static bool s_in_frame;

void pc_os_run_alarms(void);

/* MELEE_INTERP_TRACE=<first tick>: log a timeline of ten ticks, relative
 * to the start each tick was scheduled for. */
void pc_vi_trace(const char* what) {
    static long first = -2;
    if (first == -2) {
        const char* v = getenv("MELEE_INTERP_TRACE");
        first = v != NULL ? atol(v) : -1;
    }
    if (first < 0 || s_retrace_count < (u32)first || s_retrace_count >= (u32)first + 10)
        return;
    long long rel = (long long)SDL_GetTicksNS() - (long long)(next_sim_ns - pc_sim_period_ns());
    pc_log_line("trace tick %u %-14s %+.2f ms", s_retrace_count, what, rel / 1e6);
}

/* MELEE_FPS: spacing of consecutive presents, tick and in-between frames
 * alike. Even spacing is what makes a high refresh rate look smooth; the
 * frame count alone cannot show a 4/12 ms alternation. */
static void present_spacing_note(void) {
    static int enabled = -1;
    static u64 prev, t0;
    static u32 hist[9]; /* <2 <3 <4 <5 <6 <8 <10 <14 >=14 ms */
    if (enabled < 0)
        enabled = getenv("MELEE_FPS") != NULL;
    if (!enabled)
        return;
    u64 now = SDL_GetTicksNS();
    if (prev != 0) {
        static const u64 edge[8] = { 2, 3, 4, 5, 6, 8, 10, 14 };
        u64 ms_x = (now - prev) / 1000000ull;
        int bucket = 0;
        while (bucket < 8 && ms_x >= edge[bucket])
            bucket++;
        hist[bucket]++;
    }
    prev = now;
    if (t0 == 0)
        t0 = now;
    if (now - t0 >= 1000000000ull) {
        pc_log_line("present spacing ms: <2:%u 2-3:%u 3-4:%u 4-5:%u 5-6:%u 6-8:%u 8-10:%u "
                    "10-14:%u 14+:%u",
            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8]);
        memset(hist, 0, sizeof hist);
        t0 = now;
    }
}
void aurora_heap_check(void);

uint32_t pc_gfx_prewarm(uint32_t max_wait_ms) {
    return aurora_wait_pipelines(max_wait_ms);
}

uint64_t pc_monotonic_ns(void) {
    return SDL_GetTicksNS();
}

#ifdef __EMSCRIPTEN__
/* A precise native sleep would block the browser event loop; the frame
 * boundary is also the only place the page gets control back. */
#define PC_PACE_ALWAYS 1
static bool s_pace_waited;
static void pc_pace_wait(u64 ns) {
    s_pace_waited = true;
    if (ns >= 1000000ull)
        emscripten_sleep((unsigned)(ns / 1000000ull));
    else
        browser_yield();
}
#else
#define PC_PACE_ALWAYS 0
#define pc_pace_wait SDL_DelayPrecise
#endif

static void pace_until(u64 due) {
    u64 now = SDL_GetTicksNS();
    if (now < due)
        pc_pace_wait(due - now);
}

void pc_frame_boundary(void) {
    static int timing_debug = -1;
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

    if (timing_debug < 0)
        timing_debug = getenv("MELEE_NET_DEBUG") != NULL;
    if (s_in_frame && s_tick_frames > 1 && next_sim_ns != 0) {
        const u64 period = pc_sim_period_ns();
        pace_until(next_sim_ns - period + s_present_margin_ns +
                   period * (u64)(s_tick_frames - 1) / (u64)s_tick_frames);
    }
    s_tick_frames = 0;
    if (s_in_frame) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        aurora_end_frame();
        present_spacing_note();
        pc_vi_trace("present exact");
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 20000000ull)
                pc_log_line("net timing: aurora_end_frame %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
        s_in_frame = false;
    }
    /* Session messages are host-side work, never rollback simulation. The
     * matcher owns them until both peers cross its READY barrier. */
    if (pc_net_match_state(NULL) == PC_MATCH_READY)
        pc_rank_session_poll();
#ifdef __EMSCRIPTEN__
    extern void pc_audio_pump(void);
    pc_audio_pump();
#endif
    aurora_heap_check();    /* no-op unless MELEE_HEAP_CHECK is set */
    pc_widescreen_update(); /* Auto mode follows window resizes. */
    /* MELEE_LAN_TEST=1|host: the LAN lobby without the menu; "host" starts
     * a match with the first peer found. MELEE_LAN_DIRECT=ip:port: the same
     * with a known peer, no discovery (src/pc/net_lan.c). */
    static int lan_test = -1;
    static u32 lan_frames;
    if (lan_test < 0) {
        const char* t = getenv("MELEE_LAN_TEST");
        lan_test = getenv("MELEE_LAN_DIRECT") != NULL ? 3 :
                   t == NULL                          ? 0 :
                   strcmp(t, "host") == 0             ? 2 :
                                                        1;
    }
    if (lan_test) {
        pc_lan_poll();
        /* Both fixtures start only once the game has its rules loaded (the
         * title screen), not at frame 0: RULES would carry zeros. */
        lan_frames++;
        if (lan_test == 2 && lan_frames >= 300 && pc_lan_state(NULL) == 0) {
            pc_lan_start_match();
        }
        if (lan_test == 3 && lan_frames == 300) {
            const char* d = getenv("MELEE_LAN_DIRECT");
            char host[64];
            const char* colon = strrchr(d, ':');
            if (colon != NULL && (size_t)(colon - d) < sizeof host) {
                memcpy(host, d, (size_t)(colon - d));
                host[colon - d] = '\0';
                pc_lan_connect_direct(host, (uint16_t)atoi(colon + 1));
            } else {
                pc_log_line("lan: MELEE_LAN_DIRECT must be ip:port");
            }
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
            /* Through pc_log_line, not stderr: on Android stderr goes
             * nowhere, so MELEE_FPS printed nothing there. pc_log_line also
             * reaches logcat and MELEE_LOG_FILE. */
            pc_log_line("fps %.1f presented %.1f worst %.1fms late>20ms %u late>33ms %u "
                        "sleep_overshoot %.1fms",
                fps_n * 1000.0 / (double)(now - fps_t0),
                (fps_n + s_subframes) * 1000.0 / (double)(now - fps_t0), frame_worst_ns / 1e6,
                frame_late_20, frame_late_33, sleep_worst_over_ns / 1e6);
            s_subframes = 0;
            fps_t0 = now;
            fps_n = 0;
            frame_worst_ns = 0;
            frame_late_20 = 0;
            frame_late_33 = 0;
            sleep_worst_over_ns = 0;
        }
    }

    u64 update_started = timing_debug ? SDL_GetTicksNS() : 0;
    const AuroraEvent* event = aurora_update();
    if (timing_debug) {
        u64 elapsed = SDL_GetTicksNS() - update_started;
        if (elapsed > 50000000ull)
            pc_log_line("net timing: aurora_update %.1f ms at retrace %u frame %d", elapsed / 1e6,
                s_retrace_count, pc_net_frame());
    }
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
    /* MELEE_EXIT_AFTER_FRAMES=<n>: bound a scripted run without needing
     * synthetic input, which is unreliable under Xwayland. Setting
     * pc_exit_requested instead of exiting here on purpose: the window-close
     * path is the one that runs atexit(pc_shutdown_once), and skipping it is
     * what makes Dawn's static destructors race the live device. */
    static int exit_after = -1;
    if (exit_after < 0) {
        const char* n = getenv("MELEE_EXIT_AFTER_FRAMES");
        exit_after = n != NULL ? atoi(n) : 0;
    }
    if (exit_after > 0 && s_retrace_count >= (u32)exit_after && !pc_exit_requested) {
        pc_log_line("MELEE_EXIT_AFTER_FRAMES: reached frame %u, exiting", s_retrace_count);
        pc_exit_requested = true;
    }
    if (pc_exit_requested) {
        pc_net_match_stop();
        pc_net_disconnect();
        pc_lan_stop();
        exit(0);
    }

#ifdef __EMSCRIPTEN__
    s_pace_waited = false;
#endif
    /* Enforce deterministic 60 Hz simulation pacing regardless of display refresh rate
     * (e.g. 120 Hz, 144 Hz, 240 Hz high-refresh monitors). When VSync is enabled on high-refresh
     * displays, aurora_begin_frame() unblocks at monitor refresh rate. Without this check,
     * the simulation would run at 2x-4x speed. Pacing strictly to 60.000 Hz ensures physics,
     * hitboxes, and timers remain bit-identical. */
    const u64 sim_period = pc_sim_period_ns();
    u64 now = SDL_GetTicksNS();
    /* How far behind its schedule this boundary is, and how much of that to
     * run off by skipping the sleep below. Offline a large hitch is dropped:
     * there is nothing to stay in step with. In netplay dropping it left the
     * peer that froze behind the other one by the whole freeze, which time
     * sync then took seconds to close (pc_net_catch_up_ns). */
    u64 late = next_sim_ns != 0 && now > next_sim_ns ? now - next_sim_ns : 0;
    late = pc_net_active() ? pc_net_catch_up_ns(late) : late > sim_period * 2 ? 0 : late;
    if (next_sim_ns == 0 || now > next_sim_ns) {
        next_sim_ns = now - late;
    } else if (now < next_sim_ns) {
        const u64 want = next_sim_ns - now;
        /* On standard 60 Hz VSync, aurora_begin_frame already waited for VBlank. On high-refresh
         * (120/144/240 Hz), VSync-off or MAILBOX presentation (interpolated frames, see
         * src/pc/interp.c), this throttles simulation to exact 60 Hz. */
        if (PC_PACE_ALWAYS || !aurora_present_blocks() || want > 2000000ull) {
            u64 started = (fps_log || timing_debug) ? SDL_GetTicksNS() : 0;
            pc_pace_wait(want);
            if (fps_log || timing_debug) {
                u64 slept = SDL_GetTicksNS() - started;
                if (fps_log && slept > want && slept - want > sleep_worst_over_ns) {
                    sleep_worst_over_ns = slept - want;
                }
                if (timing_debug && slept > 20000000ull)
                    pc_log_line(
                        "net timing: pacing sleep %.1f ms (asked %.1f) at retrace %u frame %d",
                        slept / 1e6, want / 1e6, s_retrace_count, pc_net_frame());
            }
        }
    }
    next_sim_ns += sim_period;
#ifdef __EMSCRIPTEN__
    if (!s_pace_waited)
        browser_yield(); /* every frame returns to the event loop at least once */
#endif

    /* aurora_begin_frame returns false while minimized/paused; keep pumping.
     * Sleep a frame between attempts: without it a minimized window spins a
     * core at 100% polling SDL. */
    for (;;) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        bool begun = aurora_begin_frame();
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 20000000ull)
                pc_log_line("net timing: aurora_begin_frame %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
        if (begun)
            break;
        update_started = timing_debug ? SDL_GetTicksNS() : 0;
        event = aurora_update();
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - update_started;
            if (elapsed > 50000000ull)
                pc_log_line("net timing: aurora_update %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
        while (event != NULL && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) {
                pc_net_match_stop();
                pc_net_disconnect();
                pc_lan_stop();
                exit(0);
            }
            ++event;
        }
#ifdef __EMSCRIPTEN__
        emscripten_sleep(16);
#else
        SDL_Delay(16);
#endif
    }
    s_in_frame = true;
    pc_vi_trace("tick begun");

    /* Presentation-only chat reads the physical local controller, never a
     * rewound/synchronized pad. It cannot change the game simulation. */
    bool chat_eligible = pc_net_chat_available();
    PADStatus chat_pads[4] = {0};
    if (chat_eligible)
        PADRead(chat_pads);
    pc_net_chat_poll(chat_pads[0].button, chat_eligible, SDL_GetTicks());
    ifNetChat_Update(chat_eligible);

    s_retrace_count++;
#ifdef __EMSCRIPTEN__
    // clang-format off
    EM_ASM({if(Module.onFrame)Module.onFrame($0);},s_retrace_count);
    // clang-format on
#endif
    /* Age of the 1000 Hz sample the sim is about to consume, before the pad
     * alarms (fn_800195FC -> PADRead) fire from pc_os_run_alarms. */
    pc_input_latency_record();
    u64 alarms_started = timing_debug ? SDL_GetTicksNS() : 0;
    pc_os_run_alarms();
    if (timing_debug) {
        u64 elapsed = SDL_GetTicksNS() - alarms_started;
        if (elapsed > 50000000ull)
            pc_log_line("net timing: pc_os_run_alarms %.1f ms at retrace %u frame %d",
                elapsed / 1e6, s_retrace_count, pc_net_frame());
    }
    /* Time sync spreads its correction here: a per-frame lengthening of the
     * next wait, plus a whole frame when a gap is too big to nudge away. */
    next_sim_ns += pc_net_pace_adjust_ns();
    if (s_pre_cb) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        s_pre_cb(s_retrace_count);
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 50000000ull)
                pc_log_line("net timing: pre-retrace callback %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
    }
    s_current_fb = s_next_fb;
    if (s_post_cb) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        s_post_cb(s_retrace_count);
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 50000000ull)
                pc_log_line("net timing: post-retrace callback %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
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

u64 pc_sim_period_ns(void) {
    /* MELEE_DEBUG_SIM_HZ=<hz>: slow motion for inspecting the in-between
     * frames of src/pc/interp.c. Never set in normal play. */
    static u64 period;
    if (period == 0) {
        const char* hz = getenv("MELEE_DEBUG_SIM_HZ");
        int v = hz != NULL ? atoi(hz) : 0;
        period = 1000000000ull / (u64)(v > 0 && v <= 60 ? v : 60);
    }
    return period;
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

/* An in-between frame (src/pc/interp.c) is finished: present it and open the
 * next one. Only the presentation half of a frame boundary runs here -- no
 * alarms, no pads, no retrace callbacks -- so the game sees one retrace per
 * tick however many frames reach the screen. */
void pc_vi_present_subframe(int k, int n) {
    const u64 period = pc_sim_period_ns();
    const u64 tick_start = next_sim_ns - period;
    s_subframes++;
    s_tick_frames = n;
    if (k == 1 && next_sim_ns != 0) {
        /* Grow at once when the first frame is late, shrink slowly. */
        const u64 slot = period / (u64)n;
        u64 ready = SDL_GetTicksNS() - tick_start + 500000ull;
        if (ready > s_present_margin_ns)
            s_present_margin_ns = ready;
        else if (s_present_margin_ns > 20000ull)
            s_present_margin_ns -= 20000ull;
        if (s_present_margin_ns > slot)
            s_present_margin_ns = slot;
    }
    if (next_sim_ns != 0)
        pace_until(tick_start + s_present_margin_ns + period * (u64)(k - 1) / (u64)n);
    if (s_in_frame) {
        aurora_end_frame();
        present_spacing_note();
        pc_vi_trace("present sub");
        s_in_frame = false;
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
    for (;;) {
        if (aurora_begin_frame())
            break;
        if (pc_exit_requested)
            return;
        SDL_Delay(16);
        event = aurora_update();
        while (event != NULL && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT)
                pc_exit_requested = true;
            ++event;
        }
    }
    s_in_frame = true;
}
