/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/input_poll.h"
#include "pc/pc.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Thread* s_poll_thread = NULL;
static atomic_bool s_poll_running = false;
static atomic_uint_fast64_t s_poll_count = 0;

static int SDLCALL input_poll_worker(void* data) {
    (void)data;
    /* Request high thread priority so input sampling is immune to general CPU loads. */
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    const uint64_t interval_ns = 1000000ull; /* 1.0 ms = 1000 Hz */
    uint64_t next_tick = SDL_GetTicksNS();

    while (atomic_load_explicit(&s_poll_running, memory_order_relaxed)) {
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

uint64_t pc_input_poll_get_count(void) {
    return atomic_load_explicit(&s_poll_count, memory_order_relaxed);
}
