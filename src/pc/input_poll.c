/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/input_poll.h"
#include "pc/pc.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <dolphin/pad.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Thread* s_poll_thread = NULL;
static atomic_bool s_poll_running = false;
static atomic_uint_fast64_t s_poll_count = 0;
/* When the state the sim will read next was sampled; 0 until the first poll. */
static _Atomic uint64_t s_sample_ns;

/* Sample age per frame, game thread only. 600 frames = 10 s at 60 Hz. */
#define AGE_WINDOW 600
static float s_age_ms[AGE_WINDOW];
static int s_age_n, s_age_i;

static int SDLCALL input_poll_worker(void* data) {
    (void)data;
    /* Request high thread priority so input sampling is immune to general CPU loads. */
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    const uint64_t interval_ns = 1000000ull; /* 1.0 ms = 1000 Hz */
    uint64_t next_tick = SDL_GetTicksNS();

    while (atomic_load_explicit(&s_poll_running, memory_order_relaxed)) {
        /* Stamp before the update so the age includes the update itself. */
        atomic_store_explicit(&s_sample_ns, SDL_GetTicksNS(), memory_order_relaxed);
        /* SDL only refreshes gamepad state from SDL_UpdateJoysticks, which
         * the event pump runs once per frame; calling it here makes every SDL
         * gamepad (and its rumble timers) current to 1 ms. Joystick calls
         * are thread-safe (SDL_joystick.h). Returns early before SDL_Init. */
        SDL_UpdateGamepads();
        pc_gcadapter_poll();
        /* Apply keyboard and touch virtual controller states every 1 ms */
        pc_keyboard_apply();
        atomic_fetch_add_explicit(&s_poll_count, 1, memory_order_relaxed);

        next_tick += interval_ns;
        const uint64_t now = SDL_GetTicksNS();
        if (now < next_tick) {
            SDL_DelayPrecise(next_tick - now);
        } else {
            /* If delayed or falling behind, realign to current time */
            next_tick = now;
        }
    }
    return 0;
}

void pc_input_poll_init(void) {
    if (s_poll_thread != NULL) {
        return;
    }
    atomic_store_explicit(&s_poll_running, true, memory_order_release);
    atomic_store_explicit(&s_poll_count, 0, memory_order_relaxed);
    s_poll_thread = SDL_CreateThread(input_poll_worker, "Melee-Input-1000Hz", NULL);
    if (s_poll_thread == NULL) {
        pc_log_line("Warning: Failed to create 1000 Hz input polling thread: %s", SDL_GetError());
    } else {
        pc_log_line("1000 Hz input polling thread started (1 ms polling interval)");
    }
}

void pc_input_poll_shutdown(void) {
    if (s_poll_thread == NULL) {
        return;
    }
    atomic_store_explicit(&s_poll_running, false, memory_order_release);
    SDL_WaitThread(s_poll_thread, NULL);
    s_poll_thread = NULL;
    pc_log_line("1000 Hz input polling thread stopped (total polls: %llu)",
        (unsigned long long)atomic_load_explicit(&s_poll_count, memory_order_relaxed));
}

bool pc_input_poll_is_running(void) {
    return atomic_load_explicit(&s_poll_running, memory_order_relaxed);
}

/* Smoothed 1000 Hz thread rate for the HUD; updated ~4x/s. */
float pc_input_poll_hz(void) {
    static uint64_t last_ns, last_count;
    static float hz;
    const uint64_t now = SDL_GetTicksNS();
    const uint64_t count = pc_input_poll_get_count();
    if (last_ns == 0) {
        last_ns = now;
        last_count = count;
    } else if (now > last_ns) {
        const float dt = (float)(now - last_ns) / 1e9f;
        if (dt >= 0.25f) {
            hz = (float)(count - last_count) / dt;
            last_ns = now;
            last_count = count;
        }
    }
    return hz;
}

uint64_t pc_input_poll_get_count(void) {
    return atomic_load_explicit(&s_poll_count, memory_order_relaxed);
}

bool pc_is_input_hud_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("MELEE_INPUT_HUD");
        on = e != NULL && e[0] != '\0' && e[0] != '0';
    }
    return on;
}

static int cmp_float(const void* a, const void* b) {
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

bool pc_input_latency(float* mean_ms, float* max_ms, float* p99_ms) {
    if (s_age_n == 0) {
        return false;
    }
    float sum = 0.0f, max = 0.0f;
    for (int i = 0; i < s_age_n; i++) {
        sum += s_age_ms[i];
        if (s_age_ms[i] > max) {
            max = s_age_ms[i];
        }
    }
    if (mean_ms != NULL) {
        *mean_ms = sum / (float)s_age_n;
    }
    if (max_ms != NULL) {
        *max_ms = max;
    }
    if (p99_ms != NULL) {
        /* ponytail: sort a copy on demand; callers ask once per frame at most. */
        float sorted[AGE_WINDOW];
        memcpy(sorted, s_age_ms, sizeof(float) * (size_t)s_age_n);
        qsort(sorted, (size_t)s_age_n, sizeof(float), cmp_float);
        *p99_ms = sorted[(s_age_n - 1) * 99 / 100];
    }
    return true;
}

void pc_input_latency_record(void) {
    const uint64_t sampled = atomic_load_explicit(&s_sample_ns, memory_order_relaxed);
    if (sampled == 0) {
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    s_age_ms[s_age_i] = now > sampled ? (float)(now - sampled) / 1e6f : 0.0f;
    s_age_i = (s_age_i + 1) % AGE_WINDOW;
    if (s_age_n < AGE_WINDOW) {
        s_age_n++;
    }

    /* MELEE_INPUT_HUD=1: a log summary every 600 frames, so a headless run
     * (or one that never reaches a match) still reports the numbers. */
    if (!pc_is_input_hud_enabled()) {
        return;
    }
    static int frames;
    static uint64_t t0_ns, polls0, reports0;
    if (t0_ns == 0) {
        t0_ns = now;
        polls0 = pc_input_poll_get_count();
        reports0 = pc_gcadapter_report_count();
    }
    if (++frames < AGE_WINDOW) {
        return;
    }
    frames = 0;
    const double secs = (double)(now - t0_ns) / 1e9;
    const uint64_t polls = pc_input_poll_get_count();
    const uint64_t reports = pc_gcadapter_report_count();
    float mean, max, p99;
    pc_input_latency(&mean, &max, &p99);
    uint8_t raw[6];
    PADStatus st;
    bool wireless = false;
    char pad[96] = "port1 no GC adapter controller";
    if (pc_gcadapter_raw(0, raw, &wireless) && pc_gcadapter_status(0, &st)) {
        snprintf(pad, sizeof(pad),
            "port1 %s raw %u,%u c %u,%u L %u R %u -> %d,%d c %d,%d L %u R %u btn %04x",
            wireless ? "wireless" : "wired", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
            st.stickX, st.stickY, st.substickX, st.substickY, st.triggerLeft, st.triggerRight,
            st.button);
    }
    pc_log_line("input: poll %.0f Hz, adapter reports %.0f/s, sample age mean %.2f max %.2f "
                "p99 %.2f ms over %d frames, %s",
        secs > 0 ? (double)(polls - polls0) / secs : 0.0,
        secs > 0 ? (double)(reports - reports0) / secs : 0.0, mean, max, p99, AGE_WINDOW, pad);
    t0_ns = now;
    polls0 = polls;
    reports0 = reports;
}
