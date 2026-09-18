/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
/*
 * OS services aurora does not provide: interrupt masking, alarms, reset,
 * progressive-mode flags, and misc queries.
 *
 * Melee is single-threaded, but aurora delivers GX draw-done and DVD
 * completion callbacks on worker threads. The game brackets its shared-state
 * updates with OSDisableInterrupts/OSRestoreInterrupts, so those are mapped
 * onto a recursive mutex to keep that atomicity.
 */
#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <dolphin/os/OSReset.h>
#include <dolphin/os/OSError.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "pc/pc.h"
#include "pc/disc.h"

/* ---- interrupts ------------------------------------------------------- */

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif
#endif

static pthread_mutex_t s_intr_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static __thread int s_intr_depth;
static __thread int s_is_game_thread;

void pc_os_run_alarms(void);

BOOL OSDisableInterrupts(void) {
    pthread_mutex_lock(&s_intr_mutex);
    return s_intr_depth++ == 0;
}

/* Interrupts were the GC's way of delivering alarms; on PC, due alarms are
 * delivered on the game thread whenever it re-enables interrupts (and once
 * per frame from pc_frame_boundary), which is where the original code
 * expected them to be able to run. */
static void deliver_pending(void) {
    static __thread int in_delivery;
    if (s_is_game_thread && !in_delivery) {
        in_delivery = 1;
        pc_os_run_alarms();
        in_delivery = 0;
    }
}

BOOL OSEnableInterrupts(void) {
    BOOL was_enabled = s_intr_depth == 0;
    while (s_intr_depth > 0) {
        s_intr_depth--;
        pthread_mutex_unlock(&s_intr_mutex);
    }
    deliver_pending();
    return was_enabled;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL was_enabled = s_intr_depth == 0;
    if (level) {
        OSEnableInterrupts();
    } else if (s_intr_depth > 0) {
        s_intr_depth--;
        pthread_mutex_unlock(&s_intr_mutex);
    }
    return was_enabled;
}

/* ---- alarms ----------------------------------------------------------- */
/* Handlers always run on the game thread (see deliver_pending), like
 * interrupt handlers did. */

static OSAlarm* s_alarms;

void OSInitAlarm(void) {}

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = NULL;
    alarm->prev = alarm->next = NULL;
    alarm->period = 0;
}

static void insert_alarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler) {
    BOOL intr = OSDisableInterrupts();
    if (alarm->handler) {
        OSCancelAlarm(alarm);
    }
    alarm->handler = handler;
    alarm->fire = fire;
    alarm->prev = NULL;
    alarm->next = s_alarms;
    if (s_alarms) {
        s_alarms->prev = alarm;
    }
    s_alarms = alarm;
    OSRestoreInterrupts(intr);
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    alarm->period = 0;
    insert_alarm(alarm, OSGetTime() + tick, handler);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) {
    alarm->period = 0;
    insert_alarm(alarm, time, handler);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    alarm->period = period;
    alarm->start = start;
    insert_alarm(alarm, OSGetTime() + start, handler);
}

void OSCancelAlarm(OSAlarm* alarm) {
    BOOL intr = OSDisableInterrupts();
    if (alarm->handler) {
        if (alarm->prev) {
            alarm->prev->next = alarm->next;
        } else if (s_alarms == alarm) {
            s_alarms = alarm->next;
        }
        if (alarm->next) {
            alarm->next->prev = alarm->prev;
        }
        alarm->handler = NULL;
        alarm->prev = alarm->next = NULL;
    }
    OSRestoreInterrupts(intr);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) {
    alarm->tag = tag;
}

void OSCancelAlarms(u32 tag) {
    BOOL intr = OSDisableInterrupts();
    for (OSAlarm* a = s_alarms; a;) {
        OSAlarm* next = a->next;
        if (a->tag == tag) {
            OSCancelAlarm(a);
        }
        a = next;
    }
    OSRestoreInterrupts(intr);
}

BOOL OSCheckAlarmQueue(void) {
    return s_alarms != NULL;
}

static void card_deliver(void);

void pc_os_run_alarms(void) {
    OSTime now = OSGetTime();
    BOOL intr = OSDisableInterrupts();
    for (OSAlarm* a = s_alarms; a;) {
        OSAlarm* next = a->next;
        if (a->fire <= now) {
            OSAlarmHandler handler = a->handler;
            if (a->period > 0) {
                if (a->period <= OSMillisecondsToTicks(10)) {
                    /* ponytail: periodic alarms like the 3ms pad-poll alarm only
                     * need "has fired since last frame" semantics to avoid redundant polling. */
                    a->fire += a->period;
                    if (a->fire <= now) {
                        a->fire = now + a->period;
                    }
                    handler(a, NULL);
                } else {
                    /* Timekeeping periodic alarms (e.g. 60 Hz movie player):
                     * Catch up all due periods so ticks match real elapsed time and video stays in
                     * sync. */
                    int max_catchup = 10;
                    while (a->fire <= now && max_catchup-- > 0 && a->handler == handler) {
                        a->fire += a->period;
                        handler(a, NULL);
                    }
                    if (a->fire <= now) {
                        a->fire = now + a->period;
                    }
                }
            } else {
                OSCancelAlarm(a);
                handler(a, NULL);
            }
        }
        a = next;
    }
    OSRestoreInterrupts(intr);
    card_deliver();
}

/* ---- memory card completions ------------------------------------------ */
/* aurora finishes CARD*Async calls inline; the game's drivers arm their
 * "pending" state after the call returns, so completions are queued here and
 * delivered with the alarms (game thread, interrupts enabled), as the CARD
 * interrupt handler would have on GameCube. */

#include <dolphin/card.h>

#define PC_CARD_QUEUE 16
static struct {
    CARDCallback callback;
    s32 chan;
    s32 result;
} s_card_queue[PC_CARD_QUEUE];
static int s_card_head, s_card_count;

static void card_dispatch(CARDCallback callback, s32 chan, s32 result) {
    BOOL intr = OSDisableInterrupts();
    if (s_card_count == PC_CARD_QUEUE) {
        OSPanic(__FILE__, __LINE__, "card completion queue overflow");
    }
    int slot = (s_card_head + s_card_count++) % PC_CARD_QUEUE;
    s_card_queue[slot].callback = callback;
    s_card_queue[slot].chan = chan;
    s_card_queue[slot].result = result;
    OSRestoreInterrupts(intr);
}

static void card_deliver(void) {
    while (s_card_count > 0) {
        BOOL intr = OSDisableInterrupts();
        CARDCallback callback = s_card_queue[s_card_head].callback;
        s32 chan = s_card_queue[s_card_head].chan;
        s32 result = s_card_queue[s_card_head].result;
        s_card_head = (s_card_head + 1) % PC_CARD_QUEUE;
        s_card_count--;
        OSRestoreInterrupts(intr);
        callback(chan, result);
    }
}

/* ---- reset / mode flags ------------------------------------------------ */

static BOOL s_progressive;
static BOOL s_eurgb60;

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)reset;
    (void)resetCode;
    (void)forceMenu;
    fprintf(stderr, "OSResetSystem: exiting\n");
    exit(0);
}

u32 OSGetResetCode(void) {
    return 0;
}

u32 OSGetProgressiveMode(void) {
    return s_progressive;
}

void OSSetProgressiveMode(u32 on) {
    s_progressive = on;
}

u32 OSGetEuRgb60Mode(void) {
    return s_eurgb60;
}

void OSSetEuRgb60Mode(u32 on) {
    s_eurgb60 = on;
}

u32 OSGetConsoleSimulatedMemSize(void) {
    return PC_MEM1_SIZE;
}

BOOL OSCheckActiveThreads(void) {
    return 1;
}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error;
    (void)handler;
    return NULL;
}

BOOL DBIsDebuggerPresent(void) {
    return 0;
}

#define PC_MAX_EXT_PTRS 65536
static void* s_ext_ptrs[PC_MAX_EXT_PTRS];
static uint32_t s_ext_ptr_count = 0;
static pthread_mutex_t s_ext_ptr_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t pc_register_ext_ptr(const void* p) {
    if (!p)
        return 0;
    pthread_mutex_lock(&s_ext_ptr_mutex);
    for (uint32_t i = 0; i < s_ext_ptr_count; i++) {
        if (s_ext_ptrs[i] == p) {
            pthread_mutex_unlock(&s_ext_ptr_mutex);
            return i + 1;
        }
    }
    if (s_ext_ptr_count < PC_MAX_EXT_PTRS) {
        uint32_t id = s_ext_ptr_count++;
        s_ext_ptrs[id] = (void*)p;
        pthread_mutex_unlock(&s_ext_ptr_mutex);
        return id + 1;
    }
    pthread_mutex_unlock(&s_ext_ptr_mutex);
    fprintf(stderr, "pc_register_ext_ptr: table overflow (max %d)\n", PC_MAX_EXT_PTRS);
    abort();
}

void* pc_resolve_ext_ptr(uint32_t id) {
    if (id == 0 || id > s_ext_ptr_count)
        return NULL;
    return s_ext_ptrs[id - 1];
}

void pc_disc_ptr_overflow(const void* p, const char* file, int line) {
    fprintf(stderr, "%s:%d: pointer %p does not fit a 32-bit disc slot\n", file, line, p);
    abort();
}

#include "pc/input_poll.h"
#include "pc/net.h"
#include "pc/net_lan.h"

void pc_platform_init(void) {
    s_is_game_thread = 1;
    aurora_card_set_callback_dispatch(card_dispatch);
    pc_textures_init();
    pc_input_poll_init();
    pc_net_init();
    if (getenv("MELEE_LAN_TEST") || getenv("MELEE_LAN_DIRECT")) {
        pc_lan_start(); /* LAN lobby fixture without the menu, see vi.c */
    }
}

/* ---- reporting -------------------------------------------------------- */
/* aurora declares these weak and leaves them to the game. */

#include <stdarg.h>
#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <os/log.h>
#endif

void OSVReport(const char* msg, va_list list) {
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_INFO, "OSReport", msg, list);
#elif defined(__APPLE__)
    char buf[1024];
    va_list copy;
    va_copy(copy, list);
    vsnprintf(buf, sizeof(buf), msg, copy);
    va_end(copy);
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "[Melee] %{public}s", buf);
    vfprintf(stdout, msg, list);
    fflush(stdout);
#else
    vfprintf(stdout, msg, list);
    fflush(stdout);
#endif
}

void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    OSVReport(msg, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    va_list args;
    va_start(args, msg);
#if defined(__ANDROID__)
    char buf[1024];
    vsnprintf(buf, sizeof(buf), msg, args);
    __android_log_print(ANDROID_LOG_FATAL, "OSPanic", "PANIC %s:%d: %s", file, line, buf);
#elif defined(__APPLE__)
    char buf[1024];
    vsnprintf(buf, sizeof(buf), msg, args);
    os_log_with_type(
        OS_LOG_DEFAULT, OS_LOG_TYPE_FAULT, "[Melee PANIC] %s:%d: %{public}s", file, line, buf);
    fprintf(stderr, "PANIC %s:%d: %s\n", file, line, buf);
    fflush(stderr);
#else
    fprintf(stderr, "PANIC %s:%d: ", file, line);
    vfprintf(stderr, msg, args);
    fputc('\n', stderr);
    fflush(stderr);
#endif
    va_end(args);
    abort();
}
