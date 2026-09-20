/* Exercise the shipping random-stage selector, including sentinel inputs. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/mn/mnstagesel.c"
static uint32_t seed;
static int enabled = -1;
uint32_t pc_net_seed(void) {
    return seed;
}
bool gm_80164330(s32 stage) {
    return enabled == -1 || stage == enabled;
}
int main(void) {
    for (seed = 0; seed < 10000; ++seed) {
        for (int cell = 29; cell <= 30; ++cell) {
            mnStageSel_804D6CAE = cell;
            int pick = netStageSel_Random();
            assert(pick >= 0 && pick < NUM_STAGES);
            assert(mnStageSel_803F06D0[pick].stkind != 0);
            assert(netStageSel_Random() == pick);
        }
    }
    enabled = mnStageSel_803F06D0[7].xA;
    assert(netStageSel_Random() == 7);
    enabled = 1000;
    assert(netStageSel_Random() == 0);
    puts("PASS: synchronized random selection excludes sentinel cells and obeys stage switches");
}
