#define __assert libc_assert
#include <assert.h>
#undef __assert
#include "../src/melee/gm/gm_181A.c"
#include <string.h>
int main(void) {
    unsigned char before[sizeof(lbl_80472ED8)];
    memset(&lbl_80472ED8, 0xA5, sizeof(lbl_80472ED8));
    memcpy(before, &lbl_80472ED8, sizeof(before));
    for (int mode = 0x21; mode <= 0x26; ++mode) {
        for (int fighter = 0; fighter < 27; ++fighter) {
            memset(&lbl_80473594, 0x5A, sizeof(lbl_80473594));
            gm_80182554(fighter, mode);
            assert(lbl_80473594.xC == fighter);
            assert(lbl_80473594.x8 == mode);
            assert(lbl_80473594.x0 == 0);
            assert(lbl_80473594.x2 == 0);
            assert(lbl_80473594.x4 == 0);
            assert(lbl_80473594.x10 == 0x5A);
            assert(lbl_80473594.x11 == 0x5A);
            assert(memcmp(before, &lbl_80472ED8, sizeof(before)) == 0);
        }
    }
}
