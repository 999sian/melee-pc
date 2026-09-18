#include "gmonlinemode.h"

#include <stdio.h>
#include <string.h>
#include <melee/lb/forward.h>

#include "forward.h"
#include "gm_1A36.h"
#include "gm_1A3F.h"
#include "gm_unsplit.h"
#include "gmscene.h"
#include "gmvsmelee.h"
#include "types.h"
#include <dolphin/pad.h>
#include <melee/if/if_2FD9.h>
#include <melee/lb/types.h>
#include <melee/mn/inlines.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/sislib.h>
#ifdef TARGET_PC
#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/pc.h"
#endif

/* GM_ONLINE: lobby -> VS -> results -> lobby. The lobby is the Double Dash
 * LAN counter screen (docs/netcode-plan.md §8): pc_lan_* announces us, counts
 * peers, and the first Start elects the host; once pc_lan_state() reports the
 * match (2) both peers leave the lobby on the same synced frame. */

enum {
    state_lobby = 0,
    state_vs = 1,
    state_results = 2,
};

static void onEnterLobby(GameModeState*);
static void onEnterOnlineVs(GameModeState*);
static void onEnterResults(GameModeState*);

GameModeState gm_Mode_Online_States[] = {
    {
        state_lobby,
        lbDvdPreload_2,
        0,
        onEnterLobby,
        NULL,
        {
            GS_ONLINE_LOBBY,
            NULL,
            NULL,
        },
    },
    {
        state_vs,
        lbDvdPreload_2,
        0,
        onEnterOnlineVs,
        NULL,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        state_results,
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

static HSD_Text* lobby_text;
static int lobby_entry;
static char lobby_status[96];

void onEnterLobby(UNUSED GameModeState* state)
{
#ifdef TARGET_PC
    if (gm_GetPreviousSceneIndex() == state_results) {
        /* Back from a match: the session belongs to the match, not the
         * lobby (net_lan.c), so tear it down here before re-announcing. */
        pc_net_disconnect();
        pc_lan_stop();
    }
#endif
}

/* Copy of gmvsmode.c onEnterDebugVs (the netplay fixture) as a stock match:
 * Link vs Mario, both human, 4 stocks, 8 minutes, Final Destination. */
void onEnterOnlineVs(GameModeState* state)
{
    StartMeleeData* start = gm_GetGameModeStateEnterData(state);
    ssize_t i;

    gm_SetupRulesDefaults(&start->rules);
    start->rules.stkind = St_Kind_Last;
    start->rules.item_freq = -1;
    start->rules.sd_penalty = -1;
    start->rules.match_kind = MatchKind_Stock;
    start->rules.is_stock = true;
    start->rules.is_vs = true;
    start->rules.timer_enabled = true;
    start->rules.time_limit = 8 * 60;

    for (i = 0; i < Gm_Player_NumMax; i++) {
        gm_SetupPlayerDefaults(&start->players[i]);
        start->players[i].stocks = 4;
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

    start->players[0].rumble_enabled = false;
    start->players[1].rumble_enabled = false;
    start->players[2].rumble_enabled = false;
    start->players[3].rumble_enabled = false;

    gm_LoadAnnouncer();
}

void onEnterResults(GameModeState* state)
{
    gmVsMelee_EnterResults(state);
}

static void lobbySetStatus(const char* status)
{
    if (strcmp(status, lobby_status) == 0) {
        return;
    }
    snprintf(lobby_status, sizeof(lobby_status), "%s", status);
    HSD_SisLib_803A70A0(lobby_text, lobby_entry, "%s", lobby_status);
#ifdef TARGET_PC
    pc_log_line("lobby: %s", lobby_status);
#endif
}

/* ponytail: black screen + two SIS lines, same canvas as the title screen's
 * build stamp; the Names panel backdrop can replace it later. */
void gm_Scene_OnlineLobby_OnEnter(UNUSED void* unused)
{
    int entry;

    HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
    lobby_text = HSD_SisLib_803A6754(0, 0);
    lobby_text->default_kerning = 1;
    entry = HSD_SisLib_803A6B98(lobby_text, 40.0F, 60.0F, "ONLINE - LAN");
    HSD_SisLib_803A7548(lobby_text, entry, 0.9f, 0.9f);
    lobby_entry = HSD_SisLib_803A6B98(lobby_text, 40.0F, 220.0F, "%s",
                                      "LAN: searching...");
    HSD_SisLib_803A7548(lobby_text, lobby_entry, 0.6f, 0.6f);
    snprintf(lobby_status, sizeof(lobby_status), "LAN: searching...");
    entry = HSD_SisLib_803A6B98(lobby_text, 40.0F, 400.0F,
                                "START: play with the first peer    B: back");
    HSD_SisLib_803A7548(lobby_text, entry, 0.45f, 0.45f);
#ifdef TARGET_PC
    pc_lan_start();
#endif
}

void gm_Scene_OnlineLobby_OnFrame(void)
{
#ifdef TARGET_PC
    PcLanPeer peers[PC_LAN_MAX_PEERS];
    const char* why = NULL;
    char buf[96];
    int state;
    int n;
    u64 input = gm_GetButtonsTriggered(PAD_MAX_CONTROLLERS);

    pc_lan_poll();
    state = pc_lan_state(&why);
    n = pc_lan_peers(peers, PC_LAN_MAX_PEERS);
    switch (state) {
    case 0:
        if (n == 0) {
            snprintf(buf, sizeof(buf), "LAN: searching...");
        } else {
            snprintf(buf, sizeof(buf), "%d players found - press START", n);
        }
        break;
    case 1:
        snprintf(buf, sizeof(buf), "Connecting...");
        break;
    case 2:
        snprintf(buf, sizeof(buf), "Starting...");
        break;
    default:
        snprintf(buf, sizeof(buf), "Failed: %s (B to go back)",
                 why != NULL ? why : "unknown");
        break;
    }
    lobbySetStatus(buf);

    if (state == 2) {
        /* Both peers tick in lockstep once connected, so leaving on the
         * agreed frame puts GS_VS on the same synced frame everywhere. */
        if (pc_net_frame() >= pc_lan_start_frame()) {
            *HSD_RandSeedPtr = pc_lan_seed();
            pc_log_line("lobby: entering VS at frame %d, seed %u",
                        pc_net_frame(), pc_lan_seed());
            gm_801A4B60();
        }
        return;
    }
    if (input & HSD_PAD_B) {
        sfxBack();
        pc_lan_stop();
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
    } else if ((input & HSD_PAD_START) && state == 0 && n > 0) {
        sfxForward();
        pc_lan_start_match();
    }
#endif
}
