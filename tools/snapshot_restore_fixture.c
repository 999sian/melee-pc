/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Freestanding restore fixture: can execute ARM64 ELF with qemu without an
 * Android runtime, and link natively on macOS or Windows with a tiny main. */
#include "melee_state.h"
#include <stddef.h>
#include <stdint.h>
extern int game_value, game_zero[1024], *game_pointer, *game_zero_pointer;
extern int (*game_function)(void);
extern void mutate(void);
extern int audio_value, audio_zero[1024], engine_value, engine_zero[32];
static unsigned char saved_data[32768], saved_bss[32768];
static int contains(void* p, char* a, char* z) {
    return (uintptr_t)p >= (uintptr_t)a && (uintptr_t)p < (uintptr_t)z;
}
static int tracked(void* p) {
    return contains(p, __melee_data_start, __melee_data_end) ||
           contains(p, __melee_bss_start, __melee_bss_end);
}
static void copy(unsigned char* to, const unsigned char* from, size_t length) {
    for (size_t i = 0; i < length; i++) {
        to[i] = from[i];
    }
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            return __LINE__;                                                                       \
        }                                                                                          \
    } while (0)
int snapshot_fixture(void) {
    size_t d = (uintptr_t)__melee_data_end - (uintptr_t)__melee_data_start;
    size_t b = (uintptr_t)__melee_bss_end - (uintptr_t)__melee_bss_start;
    CHECK(d > 0 && b > 0 && d <= sizeof saved_data && b <= sizeof saved_bss);
    CHECK(tracked(&game_value) && tracked(&game_pointer) && tracked(&game_function));
    CHECK(tracked(game_zero) && tracked(&game_zero[1023]) && tracked(&game_zero_pointer));
    CHECK(!tracked(&audio_value) && !tracked(audio_zero) && !tracked(&audio_zero[1023]));
    CHECK(!tracked(&engine_value) && !tracked(engine_zero) && !tracked(saved_data));
    CHECK(game_function() == 84 && game_pointer == &game_value && game_zero_pointer == game_zero);
    copy(saved_data, (unsigned char*)__melee_data_start, d);
    copy(saved_bss, (unsigned char*)__melee_bss_start, b);
    mutate(); /* includes file-local initialized and zero-filled globals */
    game_value = 19;
    game_zero[0] = 17;
    game_zero[1023] = 29;
    game_pointer = &audio_value;
    game_zero_pointer = audio_zero;
    game_function = NULL;
    audio_value = 81;
    audio_zero[0] = 95;
    engine_value = 42;
    engine_zero[0] = 16;
    copy((unsigned char*)__melee_data_start, saved_data, d);
    copy((unsigned char*)__melee_bss_start, saved_bss, b);
    CHECK(game_function() == 84 && game_pointer == &game_value && game_zero_pointer == game_zero);
    CHECK(game_zero[0] == 0 && game_zero[1023] == 0);
    CHECK(audio_value == 81 && audio_zero[0] == 95 && engine_value == 42 && engine_zero[0] == 16);
    return 0;
}
#ifdef SNAPSHOT_ELF_ENTRY
/* Linux/Android use the same exit syscall ABI. No libc, GPU or device needed. */
void _start(void) {
    int result = snapshot_fixture();
#if defined(__aarch64__)
    register int status __asm__("x0") = result;
    register int call __asm__("x8") = 93;
    __asm__ volatile("svc #0" : : "r"(status), "r"(call) : "memory");
#elif defined(__x86_64__)
    __asm__ volatile("syscall" : : "a"(60), "D"(result) : "rcx", "r11", "memory");
#else
#error "Add the target exit syscall ABI before enabling this fixture"
#endif
    __builtin_unreachable();
}
#else
int main(void) {
    return snapshot_fixture();
}
#endif
