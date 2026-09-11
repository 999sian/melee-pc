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

/* ---- interrupts ------------------------------------------------------- */

static pthread_mutex_t s_intr_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static __thread int s_intr_depth;
static __thread int s_is_game_thread;

void pc_os_run_alarms(void);

BOOL OSDisableInterrupts(void)
{
    pthread_mutex_lock(&s_intr_mutex);
    return s_intr_depth++ == 0;
}

/* Interrupts were the GC's way of delivering alarms; on PC, due alarms are
 * delivered on the game thread whenever it re-enables interrupts (and once
 * per frame from pc_frame_boundary), which is where the original code
 * expected them to be able to run. */
static void deliver_pending(void)
{
    static __thread int in_delivery;
    if (s_is_game_thread && !in_delivery) {
        in_delivery = 1;
        pc_os_run_alarms();
        in_delivery = 0;
    }
}

BOOL OSEnableInterrupts(void)
{
    BOOL was_enabled = s_intr_depth == 0;
    while (s_intr_depth > 0) {
        s_intr_depth--;
        pthread_mutex_unlock(&s_intr_mutex);
    }
    deliver_pending();
    return was_enabled;
}

BOOL OSRestoreInterrupts(BOOL level)
{
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

void OSCreateAlarm(OSAlarm* alarm)
{
    alarm->handler = NULL;
    alarm->prev = alarm->next = NULL;
    alarm->period = 0;
}

static void insert_alarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler)
{
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

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    alarm->period = 0;
    insert_alarm(alarm, OSGetTime() + tick, handler);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler)
{
    alarm->period = 0;
    insert_alarm(alarm, time, handler);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler)
{
    alarm->period = period;
    alarm->start = start;
    insert_alarm(alarm, start, handler);
}

void OSCancelAlarm(OSAlarm* alarm)
{
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

void OSSetAlarmTag(OSAlarm* alarm, u32 tag)
{
    alarm->tag = tag;
}

void OSCancelAlarms(u32 tag)
{
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

BOOL OSCheckAlarmQueue(void)
{
    return s_alarms != NULL;
}

void pc_os_run_alarms(void)
{
    OSTime now = OSGetTime();
    BOOL intr = OSDisableInterrupts();
    for (OSAlarm* a = s_alarms; a;) {
        OSAlarm* next = a->next;
        if (a->fire <= now) {
            OSAlarmHandler handler = a->handler;
            if (a->period > 0) {
                /* ponytail: periodic alarms fire at most once per frame; the
                 * game's 3ms/pad-poll alarms only need "has fired since last
                 * frame" semantics. */
                a->fire += a->period;
                if (a->fire <= now) {
                    a->fire = now + a->period;
                }
            } else {
                OSCancelAlarm(a);
            }
            handler(a, NULL);
        }
        a = next;
    }
    OSRestoreInterrupts(intr);
}

/* ---- reset / mode flags ------------------------------------------------ */

static BOOL s_progressive;
static BOOL s_eurgb60;

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
    (void) reset;
    (void) resetCode;
    (void) forceMenu;
    fprintf(stderr, "OSResetSystem: exiting\n");
    exit(0);
}

u32 OSGetResetCode(void)
{
    return 0;
}

u32 OSGetProgressiveMode(void)
{
    return s_progressive;
}

void OSSetProgressiveMode(u32 on)
{
    s_progressive = on;
}

u32 OSGetEuRgb60Mode(void)
{
    return s_eurgb60;
}

void OSSetEuRgb60Mode(u32 on)
{
    s_eurgb60 = on;
}

u32 OSGetConsoleSimulatedMemSize(void)
{
    return PC_MEM1_SIZE;
}

BOOL OSCheckActiveThreads(void)
{
    return 1;
}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler)
{
    (void) error;
    (void) handler;
    return NULL;
}

BOOL DBIsDebuggerPresent(void)
{
    return 0;
}

void pc_disc_ptr_overflow(const void* p, const char* file, int line)
{
    fprintf(stderr, "%s:%d: pointer %p does not fit a 32-bit disc slot\n", file, line, p);
    abort();
}

void pc_platform_init(void)
{
    s_is_game_thread = 1;
}

/* ---- reporting -------------------------------------------------------- */
/* aurora declares these weak and leaves them to the game. */

#include <stdarg.h>

void OSVReport(const char* msg, va_list list)
{
    vfprintf(stdout, msg, list);
    fflush(stdout);
}

void OSReport(const char* msg, ...)
{
    va_list args;
    va_start(args, msg);
    OSVReport(msg, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    va_list args;
    va_start(args, msg);
    fprintf(stderr, "PANIC %s:%d: ", file, line);
    vfprintf(stderr, msg, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
    abort();
}
