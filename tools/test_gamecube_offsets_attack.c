#define __assert libc_assert
#include <assert.h>
#undef __assert
#include "../src/melee/ft/ft_0892.c"
static Fighter fighter;
u16 plAttack_80037B08(void) {
    return 7;
}
bool it_8026B6C8(Item_GObj* gobj) {
    assert(0);
    return false;
}
int main(void) {
    /* x2073 is the move ID, the low byte of the motion state's flag word. */
    fighter.kind = Ft_Kind_Mario;
    ft_800895E0(&fighter, 0x00240014);
    assert(fighter.x2070.x2070_int == 0x00240014);
    assert(fighter.x2070.x0.x2073 == 0x14);
    assert(fighter.x2070.x0.x2070 == 0);
    /* Luigi's misfire (0x71) is rewritten to 0x240063. */
    fighter.kind = Ft_Kind_Luigi;
    ft_800895E0(&fighter, 0x71);
    assert(fighter.x2070.x2070_int == 0x240063);
    assert(fighter.x2070.x0.x2073 == 0x63);
}
