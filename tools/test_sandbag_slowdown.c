#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/ft/ftcommon.c"
static Fighter fighter;
int main(void) {
    unsigned char attrs[8] = {0x3e, 0, 0, 0, 0x3e, 0x80, 0, 0};
    fighter.kind = Ft_Kind_Sandbag;
    fighter.dat_attrs = attrs;
    assert(ftCommon_SandbagGetKnockbackDeaccelX(&fighter) == 0.125f);
    assert(ftCommon_SandbagGetKnockbackDeaccelY(&fighter) == 0.25f);
    for (int sign = -1; sign <= 1; sign += 2) {
        float vx = sign * 1.0f, vy = sign * 1.0f;
        for (int frame = 0; frame < 8; ++frame) {
            vx = ftCommon_SandbagKnockbackDeaccel(
                vx, ftCommon_SandbagGetKnockbackDeaccelX(&fighter));
            vy = ftCommon_SandbagKnockbackDeaccel(
                vy, ftCommon_SandbagGetKnockbackDeaccelY(&fighter));
        }
        assert(vx == 0 && vy == 0);
    }
    puts("PASS: Sandbag reads disc slowdown values and knockback decays to zero");
}
