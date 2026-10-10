#include "gmvsmode.h"

#include <stdlib.h>
#include <string.h>
#include <melee/lb/forward.h>

#include "forward.h"
#include "gm_1A3F.h"
#include "gm_unsplit.h"
#include "gmmovieend.h"
#include "gmresult.h"
#include "gmvsmelee.h"
#include "types.h"
#include <melee/if/if_2FD9.h>
#include <melee/lb/types.h>
#include <melee/mn/types.h>
#ifdef TARGET_PC
#include <stdio.h>
#include <dolphin/os.h>
#include <melee/ft/ftdata.h>
#include <melee/pl/player.h>
#include "pc/mods/stages.h"
#include "pc/mods/items.h"
#endif

/* 1B13B8 */ static void onEnterDebugVs(GameModeState*);
/* 1B14A0 */ static void onEnterCss(GameModeState*);
/* 1B14DC */ static void onExitCss(GameModeState*);
/* 1B1514 */ static void onEnterSss(GameModeState*);
/* 1B154C */ static void onExitSss(GameModeState*);
/* 1B1588 */ static void onEnterVs(GameModeState*);
/* 1B15C8 */ static void onExitVs(GameModeState*);
/* 1B1648 */ static void onEnterSuddenDeath(GameModeState*);
/* 1B1688 */ static void onExitSuddenDeath(GameModeState*);
/* 1B16A8 */ static void onEnterResults(GameModeState*);
/* 1B16C8 */ static void onExitResults(GameModeState*);

GameModeState gm_Mode_Vs_States[] = {
    {
        gmVsMode_State_Css,
        lbDvdPreload_3,
        0,
        onEnterCss,
        onExitCss,
        {
            GS_CSS,
            &gmVsMelee_CssData,
            &gmVsMelee_CssData,
        },
    },
    {
        gmVsMode_State_Sss,
        lbDvdPreload_3,
        0,
        onEnterSss,
        onExitSss,
        {
            GS_SSS,
            &gmVsMelee_SssData,
            &gmVsMelee_SssData,
        },
    },
    {
        gmVsMode_State_Vs,
        lbDvdPreload_3,
        0,
        onEnterVs,
        onExitVs,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        gmVsMode_State_SuddenDeath,
        lbDvdPreload_3,
        0,
        onEnterSuddenDeath,
        onExitSuddenDeath,
        {
            GS_SUDDEN_DEATH,
            &gmVsMelee_StartData,
            &gmVsMelee_SuddenDeathExitInfo,
        },
    },
    {
        gmVsMode_State_Results,
        lbDvdPreload_3,
        0,
        onEnterResults,
        onExitResults,
        {
            GS_RESULTS,
            &gmVsMelee_ResultsEnterData,
            NULL,
        },
    },
    {
        gmVsMode_State_Approach,
        lbDvdPreload_2,
        0,
        gm_ModeState_Approach_OnEnter,
        NULL,
        {
            GS_APPROACH,
            &gmVsMelee_ApproachData,
            &gmVsMelee_ApproachData,
        },
    },
    {
        gmVsMode_State_ApproachVs,
        lbDvdPreload_2,
        0,
        gm_ModeState_ApproachVs_OnEnter,
        gm_ModeState_ApproachVs_OnExit,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        gmVsMode_State_Prize,
        lbDvdPreload_2,
        0,
        gm_ModeState_Prize_OnEnter,
        gm_ModeState_Prize_OnExit,
        {
            GS_PRIZE_INTERFACE,
            &if_Scene_Prize_EnterData,
            NULL,
        },
    },
    { GM_GAMEMODESTATE_TERMINATE },
};

enum {
    state_debug_vs = 1,
    state_debug_results = 3,
};

GameModeState gm_Mode_DebugVs_States[] = {
    {
        state_debug_vs,
        lbDvdPreload_2,
        0,
        onEnterDebugVs,
        NULL,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        state_debug_results,
        lbDvdPreload_2,
        0,
        onEnterResults,
        NULL,
        {
            GS_RESULTS,
            &gmVsMelee_ResultsEnterData,
            NULL,
        },
    },
    { GM_GAMEMODESTATE_TERMINATE },
};

void onEnterDebugVs(GameModeState* state)
{
    StartMeleeData* start = gm_GetGameModeStateEnterData(state);
    ssize_t i;

    gm_SetupRulesDefaults(&start->rules);
    start->rules.stkind = St_Kind_Last;
    start->rules.item_freq = -1;
    start->rules.sd_penalty = -1;
    start->rules.match_kind = MatchKind_Time;

    for (i = 0; i < Gm_Player_NumMax; i++) {
        gm_SetupPlayerDefaults(&start->players[i]);
        start->players[i].stocks = 0;
        start->players[i].cpu_kind = 4;
    }

    start->players[0].ckind = CKind_Link;
    start->players[1].ckind = CKind_Mario;
    start->players[2].ckind = CKind_Link;
    start->players[3].ckind = CKind_Link;

    start->players[0].slot_type = Gm_PKind_Human;
    start->players[1].slot_type = Gm_PKind_Human;
    start->players[2].slot_type = Gm_PKind_NA;
    start->players[3].slot_type = Gm_PKind_NA;
#ifdef TARGET_PC
    /* MELEE_DEBUG_VS_STAGE=<StKind>: the debug match on one stage instead of
     * the last-used one, so the harness can reach a stage's mid-match loads
     * (3 is Pokemon Stadium, whose transformations load from disc). */
    if (getenv("MELEE_DEBUG_VS_STAGE") != NULL) {
        int st = atoi(getenv("MELEE_DEBUG_VS_STAGE"));
        if (st > St_Kind_Test && st < St_Kind_Last) {
            start->rules.stkind = (StKind) st;
        }
    }
    if (getenv("MELEE_DEBUG_VS") != NULL && strcmp(getenv("MELEE_DEBUG_VS"), "cpu") == 0) {
        start->players[1].slot_type = Gm_PKind_Cpu;
    } else if (getenv("MELEE_DEBUG_VS") != NULL && strcmp(getenv("MELEE_DEBUG_VS"), "cpu4") == 0) {
        /* Four CPUs fighting each other: the worst-case scene for a
         * frame-time gate, with no input needed to keep it busy. */
        static const CharacterKind kinds[4] = {
            CKind_Link, CKind_Mario, CKind_Fox, CKind_Donkey,
        };
        for (i = 0; i < 4; i++) {
            start->players[i].ckind = kinds[i];
            start->players[i].slot_type = Gm_PKind_Cpu;
        }
    }
    /* MELEE_DEBUG_VS_PACK=<port>:<pack>[,<port>:<pack>...]: put mod character
     * pack <pack> (1-based, the "-> pack N" number in the log) on <port>
     * (1-4), as its base character. For testing packs without the CSS. */
    if (getenv("MELEE_DEBUG_VS_PACK") != NULL) {
        const char* spec = getenv("MELEE_DEBUG_VS_PACK");
        while (*spec != '\0') {
            int port = 0, pack = 0, used = 0;
            if (sscanf(spec, "%d:%d%n", &port, &pack, &used) != 2 || used == 0) {
                break;
            }
            spec += used;
            if (*spec == ',') {
                spec++;
            }
            if (port >= 1 && port <= 4 && pack >= 1 && pack <= Ft_Kind_PackMax) {
                FighterKind base = ftData_BaseKind(Ft_Kind_PackFirst + pack - 1);
                CharacterKind ck = Player_CharacterForFighter(base);
                if (base != Ft_Kind_PackFirst + pack - 1 && ck != ChKind_None) {
                    start->players[port - 1].ckind = ck;
                    start->players[port - 1].pc_pack = (u8) pack;
                    OSReport("debug vs: port %d plays pack %d (base fighter %d)\n",
                             port, pack, base);
                }
            }
        }
    }
    /* MELEE_DEBUG_VS_STAGE_PACK=<n>: play mod map pack <n> (the "-> map pack
     * N" number in the log) on its base stage. */
    if (getenv("MELEE_DEBUG_VS_STAGE_PACK") != NULL) {
        int pack = atoi(getenv("MELEE_DEBUG_VS_STAGE_PACK"));
        if (pack >= 1 && pack <= pc_stages_count()) {
            start->rules.stkind = (StKind) pc_stages_base(pack - 1);
            start->rules.pc_stage_pack = (u8) pack;
            OSReport("debug vs: map pack %d on stage %d\n", pack,
                     start->rules.stkind);
        }
    }
    /* MELEE_DEBUG_VS_ITEMS=<freq>[:<item>]: items at frequency <freq> (0 very
     * low .. 4 very high), optionally only <item> (a common item name, as
     * mod.json "base" takes it) -- for watching an item pack spawn. */
    if (getenv("MELEE_DEBUG_VS_ITEMS") != NULL) {
        const char* spec = getenv("MELEE_DEBUG_VS_ITEMS");
        const char* colon = strchr(spec, ':');
        int freq = atoi(spec);
        if (freq >= 0 && freq <= 4) {
            start->rules.item_freq = (s8) freq;
        }
        if (colon != NULL) {
            /* a name, or a raw ItemKind number (0x22 is the Poke Ball) */
            char* end = NULL;
            long kind = strtol(colon + 1, &end, 0);
            if (end == colon + 1 || *end != '\0') {
                kind = pc_item_from_name(colon + 1);
            }
            if (kind >= 0 && kind < 0x23) {
                start->rules.x20 = 1ULL << kind;
            }
        }
        OSReport("debug vs: items at frequency %d, mask %llx\n",
                 start->rules.item_freq,
                 (unsigned long long) start->rules.x20);
    }
    /* MELEE_DEBUG_VS_STOCKS=<n>: a stock match instead of an untimed time
     * one, so a run can end on GAME! with stocks the replay (src/pc/slp.c)
     * must carry. */
    if (getenv("MELEE_DEBUG_VS_STOCKS") != NULL) {
        int stocks = atoi(getenv("MELEE_DEBUG_VS_STOCKS"));
        if (stocks > 0 && stocks < 100) {
            start->rules.match_kind = MatchKind_Stock;
            start->rules.is_stock = true;
            for (i = 0; i < Gm_Player_NumMax; i++) {
                start->players[i].stocks = stocks;
            }
        }
    }
#endif

    start->players[0].rumble_enabled = false;
    start->players[1].rumble_enabled = false;
    start->players[2].rumble_enabled = false;
    start->players[3].rumble_enabled = false;

    gm_LoadAnnouncer();
}

void onEnterCss(GameModeState* state)
{
    gmVsMelee_EnterCss(state, gmVsMelee_GetVsData(), VS_MELEE);
}

void onExitCss(GameModeState* state)
{
    gmVsMelee_ExitCss(state, gmVsMelee_GetVsData());
}

void onEnterSss(GameModeState* state)
{
    gmVsMelee_EnterSss(state, gmVsMelee_GetVsData());
}

void onExitSss(GameModeState* state)
{
    gmVsMelee_ExitSss(state, gmVsMelee_GetVsData(), gmVsMode_State_Css);
}

void onEnterVs(GameModeState* state)
{
    gmVsMelee_EnterVs(state, gmVsMelee_GetVsData(), NULL, NULL);
}

void onExitVs(GameModeState* state)
{
    MatchExitInfo* mei;
    ssize_t i;

    gmVsMelee_ExitVs(state, gmVsMode_State_Results,
                     gmVsMode_State_SuddenDeath);
    mei = gm_GetGameModeStateExitData(state);
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        if (mei->match_end.player_standings[i].pkind != Gm_PKind_NA) {
            gm_80162A98(mei->match_end.player_standings[i].x20);
            gm_RecordSelfDestructs(
                mei->match_end.player_standings[i].self_destructs);
            gm_80162A4C(mei->match_end.player_standings[i].x44);
        }
    }
}

void onEnterSuddenDeath(GameModeState* state)
{
    gmVsMelee_EnterSuddenDeath(state, gmVsMelee_GetVsData(), NULL, NULL);
}

void onExitSuddenDeath(GameModeState* state)
{
    gmVsMelee_ExitSuddenDeath(state);
}

void onEnterResults(GameModeState* state)
{
    gmVsMelee_EnterResults(state);
}

void onExitResults(GameModeState* state)
{
    gmVsMelee_ExitResults(state, gmVsMelee_GetVsData(), gmVsMode_State_Css);
    if (!gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome)) {
        gm_801623A4(&gmVsMelee_ResultsEnterData.match_end);
    }
}
