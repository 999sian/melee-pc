#include "gmonlinemode.h"

#include <stdio.h>
#include <string.h>
#include <melee/lb/forward.h>

#include "forward.h"
#include "gm_1601.h"
#include "gm_1A36.h"
#include "gm_1A3F.h"
#include "gm_unsplit.h"
#include "gmresult.h"
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
#ifdef TARGET_PC
#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/pc.h"
#endif

/* GM_ONLINE: lobby -> CSS -> SSS -> VS -> (sudden death) -> results -> CSS,
 * the vanilla VS flow (gmvsmode.c) on a private VsModeData that both peers
 * reset identically on entering the lobby. The lobby is the Double Dash LAN
 * counter screen (docs/netcode-plan.md §8): pc_lan_* announces us, counts
 * peers, and the first Start elects the host; once pc_lan_state() reports
 * the match (2) both peers leave for the CSS on the same synced frame. From
 * there every scene runs on synced inputs: the local player is P1 when
 * hosting and P2 as guest, ports 3/4 report no controller, and the RULES
 * handshake (net.c) has made GameRules/GamePrefs/unlock/frozen-stadium
 * identical on both sides. B on the CSS goes back to the lobby. */

enum {
    state_lobby = 0,
    state_css = 1,
    state_sss = 2,
    state_vs = 3,
    state_sudden_death = 4,
    state_results = 5, /* last: gmVsMelee_ExitResults skips challengers */
};

static void onEnterLobby(GameModeState*);
static void onEnterCss(GameModeState*);
static void onExitCss(GameModeState*);
static void onEnterSss(GameModeState*);
static void onExitSss(GameModeState*);
static void onEnterVs(GameModeState*);
static void onExitVs(GameModeState*);
static void onEnterSuddenDeath(GameModeState*);
static void onExitSuddenDeath(GameModeState*);
static void onEnterResults(GameModeState*);
static void onExitResults(GameModeState*);

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
        state_css,
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
        state_sss,
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
        state_vs,
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
        state_sudden_death,
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
        state_results,
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
    { GM_GAMEMODESTATE_TERMINATE },
};

static OnlineKind online_kind;
static VsModeData online_vs;

void gmOnline_SetKind(OnlineKind kind)
{
    online_kind = kind;
}

OnlineKind gmOnline_GetKind(void)
{
    return online_kind;
}

void onEnterLobby(UNUSED GameModeState* state)
{
#ifdef TARGET_PC
    /* Fresh from the menu both are no-ops; back from a match or from B on
     * the CSS the session belongs to the match, not the lobby (net_lan.c),
     * so tear it down before re-announcing. */
    pc_net_disconnect();
    pc_lan_stop();
#endif
    /* Same CSS start state on both peers: two human doors, nothing picked. */
    gm_InitVsMode(&online_vs);
    online_vs.start.players[0].slot_type = Gm_PKind_Human;
    online_vs.start.players[1].slot_type = Gm_PKind_Human;
}

void onEnterCss(GameModeState* state)
{
    gmVsMelee_EnterCss(state, &online_vs, VS_MELEE);
}

void onExitCss(GameModeState* state)
{
    CSSData* css = gm_GetGameModeStateExitData(state);
    if (css->pending_scene_change == CSSPendingSceneChange_2) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
    gmVsMelee_ExitCss(state, &online_vs);
}

void onEnterSss(GameModeState* state)
{
    gmVsMelee_EnterSss(state, &online_vs);
}

void onExitSss(GameModeState* state)
{
    gmVsMelee_ExitSss(state, &online_vs, state_css);
}

void onEnterVs(GameModeState* state)
{
    gmVsMelee_EnterVs(state, &online_vs, NULL, NULL);
}

void onExitVs(GameModeState* state)
{
    MatchExitInfo* mei;
    ssize_t i;

    gmVsMelee_ExitVs(state, state_results, state_sudden_death);
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
    gmVsMelee_EnterSuddenDeath(state, &online_vs, NULL, NULL);
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
    gmVsMelee_ExitResults(state, &online_vs, state_css);
    if (!gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome)) {
        gm_801623A4(&gmVsMelee_ResultsEnterData.match_end);
    }
}

/* ---- lobby scene ------------------------------------------------------- */

void gm_Scene_OnlineLobby_OnEnter(UNUSED void* unused)
{
    mnOnlineLobby_Create();
#ifdef TARGET_PC
    pc_lan_start();
#endif
}

void gm_Scene_OnlineLobby_OnExit(UNUSED void* unused)
{
    mnOnlineLobby_Destroy();
}

#ifdef TARGET_PC
static void lobbyCopyName(char* dst, const char* src)
{
    snprintf(dst, ONLINE_LOBBY_NAME_LEN, "%s", src);
}

/* Fill the view from the LAN state; logs the status line when it changes. */
static void lobbyFillView(OnlineLobbyView* view, int state, const char* why,
                          const PcLanPeer* peers, int n)
{
    static char last_status[ONLINE_LOBBY_MSG_LEN];
    char status[ONLINE_LOBBY_MSG_LEN];
    bool connected = state == 1 || state == 2;
    bool host = connected && pc_lan_is_host();
    int ping = -1, delay;
    unsigned rollbacks;
    int i;

    memset(view, 0, sizeof *view);
    view->title =
        online_kind == ONLINE_KIND_DIRECT ? "DIRECT CONNECT" : "LAN PLAY";
    if (connected && !pc_net_stats(&ping, &delay, &rollbacks)) {
        ping = -1;
    }

    lobbyCopyName(view->players[0].name, pc_lan_local_name());
    view->players[0].ping_ms = -1;
    view->players[0].is_host = host;
    view->players[0].is_local = true;
    view->player_count = 1;
    for (i = 0; i < n && view->player_count < ONLINE_LOBBY_MAX_PLAYERS; i++) {
        OnlineLobbyPlayer* p = &view->players[view->player_count++];
        lobbyCopyName(p->name, peers[i].name);
        p->is_host = peers[i].host;
        /* The session peer: the host we joined, or our first peer as host. */
        p->ping_ms = connected && (peers[i].host || (host && i == 0)) ? ping : -1;
    }

    switch (state) {
    case 0:
        view->phase = n == 0 ? LOBBY_PHASE_SEARCHING : LOBBY_PHASE_FOUND;
        if (n == 0) {
            snprintf(status, sizeof status, "LAN: searching...");
        } else {
            snprintf(status, sizeof status, "%d players found - press START",
                     n + 1);
        }
        break;
    case 1:
        view->phase = LOBBY_PHASE_CONNECTING;
        snprintf(status, sizeof status, "Connecting...");
        break;
    case 2:
        view->phase = LOBBY_PHASE_STARTING;
        view->countdown_frames = pc_lan_start_frame() - pc_net_frame();
        if (view->countdown_frames < 0) {
            view->countdown_frames = 0;
        }
        snprintf(status, sizeof status, "Starting...");
        break;
    default:
        view->phase = LOBBY_PHASE_ERROR;
        snprintf(status, sizeof status, "Failed: %s",
                 why != NULL ? why : "unknown error");
        break;
    }
    memcpy(view->message, status, sizeof view->message);
    if (strcmp(status, last_status) != 0) {
        memcpy(last_status, status, sizeof last_status);
        pc_log_line("lobby: %s", status);
    }
}
#endif

void gm_Scene_OnlineLobby_OnFrame(void)
{
#ifdef TARGET_PC
    PcLanPeer peers[PC_LAN_MAX_PEERS];
    OnlineLobbyView view;
    const char* why = NULL;
    int state;
    int n;
    u64 input = gm_GetButtonsTriggered(PAD_MAX_CONTROLLERS);

    pc_lan_poll();
    state = pc_lan_state(&why);
    n = pc_lan_peers(peers, PC_LAN_MAX_PEERS);
    lobbyFillView(&view, state, why, peers, n);
    mnOnlineLobby_Update(&view);

    if (state == 2) {
        /* Both peers tick in lockstep once connected, so leaving on the
         * agreed frame puts the CSS on the same synced frame everywhere. */
        if (pc_net_frame() >= pc_lan_start_frame()) {
            *HSD_RandSeedPtr = pc_lan_seed();
            pc_log_line("lobby: entering CSS at frame %d, seed %u",
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
