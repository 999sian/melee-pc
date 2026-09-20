/* Exercise the shipping online scene's final match-setup callbacks. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/gm/gmonlinemode.c"
static bool tiebreak, set_complete;
static int async_status, next_state;
ResultsMatchInfo gmVsMelee_ResultsEnterData;
bool pc_rank_session_set_complete(void) {
    return set_complete;
}
int pc_rank_session_state(const char** reason) {
    (void)reason;
    return async_status;
}
void gm_SetNextGameModeStateId(u8 id) {
    next_state = id;
}
void gmVsMelee_ExitResults(GameModeState* state, VsModeData* vs, u8 id) {
    (void)state;
    (void)vs;
    next_state = id;
}
bool gm_WasMatchCanceled(u8 outcome) {
    return outcome == 99;
}
s32 gm_801623A4(MatchEnd* match) {
    (void)match;
    return 0;
}
bool pc_rank_session_active(void) {
    return true;
}
int pc_net_peer_status(void) {
    return 0;
}
void pc_net_peer_status_clear(void) {}
unsigned pc_rank_session_stocks(void) {
    return tiebreak ? 1 : 4;
}
unsigned pc_rank_session_seconds(void) {
    return tiebreak ? 180 : 480;
}
unsigned pc_rank_session_stage(void) {
    return 31;
}
int main(void) {
    StartMeleeData start = {0};
    online_kind = ONLINE_KIND_RANKED;
    for (unsigned tie = 0; tie < 2; tie++) {
        tiebreak = tie;
        start.rules.is_teams = true;
        start.rules.item_freq = 3;
        start.rules.x20 = ~0ULL;
        start.rules.x30 = 2;
        rankedRules(&start, NULL);
        assert(start.rules.match_kind == MatchKind_Stock && start.rules.is_stock);
        assert(start.rules.time_limit == (tie ? 180 : 480) && start.rules.timer_enabled);
        assert(start.rules.disable_pausing && !start.rules.is_teams);
        assert(start.rules.item_freq == -1 && start.rules.x20 == 0);
        assert(start.rules.x30 == 1 && start.rules.game_speed == 1 && start.rules.stkind == 31);
        for (int i = 0; i < 4; i++) {
            PlayerInitData player = {0};
            player.damage = 300;
            player.vs_metal = true;
            rankedPlayer(&player, &online_vs.start.players[i]);
            assert(player.slot_type == (i < 2 ? Gm_PKind_Human : Gm_PKind_NA));
            assert(player.stocks == (tie ? 1 : 4) && !player.damage && !player.vs_metal);
            assert(player.attack_ratio == 1 && player.defense_ratio == 1);
        }
    }
    /* A has only sent its signature; B has already appended. Their synced
     * results exit must choose the same scene in every completion ordering. */
    for (unsigned complete = 0; complete < 2; complete++) {
        set_complete = complete;
        for (int status = PC_RANK_SESSION_PLAY; status <= PC_RANK_SESSION_FAILED; status++) {
            async_status = status;
            onExitResults(NULL);
            assert(next_state == (complete ? state_lobby : state_css));
        }
    }
    set_complete = false;
    gmVsMelee_ResultsEnterData.match_end.outcome = 99;
    onExitResults(NULL);
    assert(next_state == state_lobby);
    gmVsMelee_ResultsEnterData.match_end.outcome = 0;
    online_kind = ONLINE_KIND_UNRANKED;
    StartMeleeData saved = start;
    rankedRules(&start, NULL);
    assert(!memcmp(&start, &saved, sizeof start));
    puts("PASS: ranked stock/time/item/pause/team/player rules, zero-damage tiebreak, "
         "async-independent results routing, unranked untouched");
}
