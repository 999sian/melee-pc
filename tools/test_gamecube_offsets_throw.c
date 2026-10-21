#define __assert libc_assert
#include <assert.h>
#undef __assert
#include "../src/melee/ft/kinds/ftCommon/ftCo_Throw.c"
static Fighter fighter;
static int grounded_phys;
void ft_80085134(Fighter_GObj* gobj) {
    assert(0);
}
void ft_80085004(Fighter_GObj* gobj) {
    ++grounded_phys;
}
int main(void) {
    static void (*const phys[])(Fighter_GObj*) = {
        ftCo_ThrowF_Phys, ftCo_ThrowB_Phys, ftCo_ThrowHi_Phys, ftCo_ThrowLw_Phys,
    };
    HSD_GObj gobj = {0};
    unsigned i;
    gobj.user_data = &fighter;
    for (i = 0; i < sizeof(phys) / sizeof(phys[0]); ++i) {
        fighter.ground_or_air = GA_Air;
        fighter.facing_dir = -1.0f;
        fighter.self_vel = (Vec3){0, 0, 0};
        fighter.mv.co.fighterthrow.x4 = 1;
        /* What ftCo_800DD724 stores when the throw freezes mid-air. */
        fighter.mv.co.throw_.xC = (Vec3){7, -2, 3};
        phys[i](&gobj);
        assert(fighter.self_vel.x == -3);
        assert(fighter.self_vel.y == -2);
    }
    assert(grounded_phys == 0);
}
