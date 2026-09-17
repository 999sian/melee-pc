/* Native SIS slots contain big-endian, 32-bit disc pointer tables. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/gm/gmcamera.c"
SIS* HSD_SisLib_804D1124[5];
static DiscU32 table[3];
static u8 digits[20];
int main(void) {
    unsigned values[] = {0, 1, 10, 100, 2043, 9999, 10000, 0xffffffff};
    for (int i = 0; i < 10; ++i) {
        digits[i * 2] = 0x20;
        digits[i * 2 + 1] = '0' + i;
    }
    table[2].v = (uintptr_t)digits;
    HSD_SisLib_804D1124[3] = (SIS*)table;
    for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        char expected[16];
        u8 result[16];
        int n = snprintf(expected, sizeof(expected), "%u", values[i] > 9999 ? 9999 : values[i]);
        assert(gmCamera_801A2224(result, values[i]) == result + 2 * n);
        for (int j = 0; j < n; ++j) {
            assert(result[2 * j] == 0x20);
            assert(result[2 * j + 1] == expected[j]);
        }
        assert(result[2 * n] == 0);
    }
    puts("PASS: Camera digits use native SIS slot and disc pointers, including clamp");
}
