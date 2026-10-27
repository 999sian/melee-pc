/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * SDL times everything (SDL_GetTicksNS: frame pacing in src/pc/vi.c, the
 * MELEE_FPS counters, every timeout) with clock_gettime(CLOCK_MONOTONIC_RAW).
 * Emscripten's libc has no such clock and returns EINVAL, so SDL fell back to
 * gettimeofday: Date.now(), which moves in whole milliseconds and jumps with
 * the system clock. CLOCK_MONOTONIC is performance.now(), microseconds and
 * monotonic, and is what RAW means here. Linked with
 * -Wl,--wrap=clock_gettime (CMakeLists.txt).
 */
#include <time.h>

int __real_clock_gettime(clockid_t id, struct timespec* ts);

int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    return __real_clock_gettime(id == CLOCK_MONOTONIC_RAW ? CLOCK_MONOTONIC : id, ts);
}
