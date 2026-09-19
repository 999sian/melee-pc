/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A netplay session dies silently when one side's game thread stops ticking:
 * the socket thread keeps answering, so the peer sees "still sending but
 * stuck at frame N" and has nothing else to go on, while the stuck side logs
 * nothing at all because the thread that would log is the one that is stuck.
 *
 * The transmit timer runs on its own thread and is therefore still alive, so
 * it is the one that notices, says so, and asks the game thread for a stack:
 * SIGPROF interrupts it and the handler walks the frames. Addresses, not
 * symbols -- a release build has no symbol table worth reading in a signal
 * handler -- so resolve them with
 *
 *     addr2line -f -C -e build/melee <addr>                     (Linux)
 *     llvm-addr2line -f -C -e build/android-arm64/libmelee.so   (Android)
 *
 * subtracting the library's load address on Android (the line prints it). */
#include "compat.h"
#include "pc/net_internal.h"

#include <SDL3/SDL_timer.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>

#ifdef __linux__
#include <pthread.h>
#include <unwind.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#endif

#define WATCHDOG_MS 5000
#define WATCHDOG_DEPTH 24

static pthread_t s_game_thread;
static bool s_game_thread_known;
static bool s_reported;
static int32_t s_last_frame = -1;
static uint64_t s_last_move_ns;

typedef struct Walk {
    uintptr_t pc[WATCHDOG_DEPTH];
    int n;
} Walk;

static _Unwind_Reason_Code walk_frame(struct _Unwind_Context* ctx, void* arg) {
    Walk* w = arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (ip == 0 || w->n >= WATCHDOG_DEPTH) {
        return _URC_END_OF_STACK;
    }
    w->pc[w->n++] = ip;
    return _URC_NO_REASON;
}

/* Runs on the stuck thread, from the signal. Only the write() inside
 * pc_log_line is not strictly async-signal-safe, and the process is wedged
 * anyway: a stack is worth more than the purity. */
static void watchdog_signal(int sig) {
    (void)sig;
    Walk w = {{0}, 0};
    _Unwind_Backtrace(walk_frame, &w);
    for (int i = 0; i < w.n; i++) {
        uintptr_t base = 0;
#ifdef __ANDROID__
        Dl_info info;
        if (dladdr((void*)w.pc[i], &info) != 0) {
            base = (uintptr_t)info.dli_fbase;
        }
#endif
        pc_log_line("net: wedged #%d pc %" PRIxPTR " (base %" PRIxPTR ", offset %" PRIxPTR ")", i,
            w.pc[i], base, w.pc[i] - base);
    }
}

/* Game thread, once per tick: this thread is the one that must keep moving. */
void net_watchdog_arm(void) {
    if (!s_game_thread_known) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = watchdog_signal;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        s_game_thread = pthread_self();
        s_game_thread_known = true;
    }
}

/* Transmit-timer thread: still running when the game thread is not. */
void net_watchdog_tick(int32_t frame) {
    uint64_t now = SDL_GetTicksNS();
    if (frame != s_last_frame) {
        s_last_frame = frame;
        s_last_move_ns = now;
        s_reported = false;
        return;
    }
    if (s_reported || !s_game_thread_known || s_last_move_ns == 0 ||
        now - s_last_move_ns < (uint64_t)WATCHDOG_MS * 1000000ull)
    {
        return;
    }
    s_reported = true;
    pc_log_line("net: the game thread has not ticked for %d ms at frame %d; stack follows",
        WATCHDOG_MS, frame);
    pthread_kill(s_game_thread, SIGPROF);
}
#else
void net_watchdog_arm(void) {}

void net_watchdog_tick(int32_t frame) {
    (void)frame;
}
#endif
