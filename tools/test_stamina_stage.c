/* Exercise the real Stamina SSS exit handler and shared VS handler. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/gm/gmstamina.c"
#include "../src/melee/gm/gmvsmelee.c"

static struct gmm_x0 save_data;
struct gmm_x0* gmMainLib_804D3EE0 = &save_data;
static int audio_stage, next_state, loads;
void* gm_GetGameModeStateExitData(GameModeState* state) { return state->info.exit_data; }
void gm_SetNextGameModeStateId(u8 state) { next_state = state; }
u64 lbAudioAx_80026EBC(StKind stage) { audio_stage = stage; return 0x40; }
void lbAudioAx_80026F2C(u32 flags) { assert(flags == 24); }
void lbAudioAx_8002702C(u32 flags, u64 mask) { assert(flags == 8 && mask == 0x40); }
void lbAudioAx_80027168(void) { ++loads; }

int main(void)
{
    SSSData sss = {0};
    GameModeState state = { .info.exit_data = &sss };
    sss.start_game = 1;
    sss.force_stage_id = -1; /* normal manual stage selection */
    sss.vs.start.rules.stkind = 4;
    audio_stage = -99;
    gm_801B927C(&state);
    assert(audio_stage == 4);
    assert(save_data.modes.vs_stamina.start.rules.stkind == 4 && loads == 1);
    sss.force_stage_id = 31; /* automatic selection resolves into rules too */
    sss.vs.start.rules.stkind = 31;
    gm_801B927C(&state);
    assert(audio_stage == 31 && loads == 2);
    sss.start_game = 0;
    next_state = -99;
    gm_801B927C(&state);
    assert(next_state == 0 && loads == 2);
    puts("PASS: Stamina uses selected stage audio; automatic selection and cancel preserved");
}
