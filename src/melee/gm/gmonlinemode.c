#include "gmonlinemode.h"

#include <stdio.h>
#include <stdlib.h>
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
#include "pc/net_match.h"
#include "pc/net_rank_session.h"
extern const char* pc_get_net_target(void);
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

static bool awaiting_rank_result;
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
        lbDvdPreload_3,
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
    /* Terminal sets leave Results deterministically. Keep transport alive
     * here while both peers finish signing and durable saving. */
    awaiting_rank_result = online_kind == ONLINE_KIND_RANKED &&
        (pc_rank_session_set_complete() ||
         pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    if (!awaiting_rank_result) {
        pc_net_match_stop();
        pc_net_disconnect();
    }
    pc_lan_stop();
#endif
    /* Same CSS start state on both peers: two human doors, nothing picked. */
    gm_InitVsMode(&online_vs);
    online_vs.start.players[0].slot_type = Gm_PKind_Human;
    online_vs.start.players[1].slot_type = Gm_PKind_Human;
    for (int i = 2; i < GM_MAX_PLAYERS; ++i)
        online_vs.start.players[i].slot_type = Gm_PKind_NA;
}

#ifdef TARGET_PC
static bool rankedMode(void) { return online_kind == ONLINE_KIND_RANKED; }

static void rankedRules(StartMeleeData* start, UNUSED StartMeleeData* previous)
{
    if (!rankedMode() || !pc_rank_session_active()) return;
    start->rules.match_kind = MatchKind_Stock;
    start->rules.is_stock = true;
    start->rules.is_teams = false;
    start->rules.timer_enabled = true;
    start->rules.timer_counts_up = false;
    start->rules.time_limit = pc_rank_session_seconds();
    start->rules.disable_pausing = true;
    start->rules.item_freq = -1;
    start->rules.x20 = 0;
    start->rules.x30 = 1.0f;
    start->rules.game_speed = 1.0f;
    start->rules.stkind = pc_rank_session_stage();
}

static void rankedPlayer(PlayerInitData* start, PlayerInitData* previous)
{
    if (!rankedMode() || !pc_rank_session_active()) return;
    start->stocks = pc_rank_session_stocks();
    start->handicap = 5;
    start->attack_ratio = start->defense_ratio = start->model_scale = 1.0f;
    start->damage = start->damage1 = 0;
    start->vs_metal = start->vs_invisible = false;
    if (previous == &online_vs.start.players[0] || previous == &online_vs.start.players[1])
        start->slot_type = Gm_PKind_Human;
    else
        start->slot_type = Gm_PKind_NA;
}
#endif

void onEnterCss(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter CSS at frame %d", pc_net_frame());
#endif
    gmVsMelee_EnterCss(state, &online_vs, VS_MELEE);
}

void onExitCss(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    CSSData* css = gm_GetGameModeStateExitData(state);
    if (css->pending_scene_change == CSSPendingSceneChange_2) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#ifdef TARGET_PC
    if (rankedMode() && !pc_rank_session_active()) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitCss(state, &online_vs);
}

void onEnterSss(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter SSS at frame %d", pc_net_frame());
#endif
    gmVsMelee_EnterSss(state, &online_vs);
#ifdef TARGET_PC
    if (rankedMode() && pc_rank_session_active()) {
        SSSData* sss = gm_GetGameModeStateEnterData(state);
        pc_rank_session_stage_begin();
        sss->force_stage_id = pc_rank_session_stage() ? (int) pc_rank_session_stage() : -1;
        sss->no_lras = true;
    }
#endif
}

void onExitSss(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitSss(state, &online_vs, state_css);
#ifdef TARGET_PC
    if (rankedMode() && !((SSSData*) gm_GetGameModeStateExitData(state))->start_game) {
        pc_rank_session_abort("ranked stage selection cancelled");
        gm_SetNextGameModeStateId(state_lobby);
    }
#endif
}

void onEnterVs(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter VS at frame %d", pc_net_frame());
#endif
#ifdef TARGET_PC
    gmVsMelee_EnterVs(state, &online_vs, rankedRules, rankedPlayer);
#else
    gmVsMelee_EnterVs(state, &online_vs, NULL, NULL);
#endif
}

void onExitVs(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        if (rankedMode()) {
            pc_rank_session_abort("peer disconnected");
        }
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    MatchExitInfo* mei;
    ssize_t i;

    gmVsMelee_ExitVs(state, state_results, state_sudden_death);
    mei = gm_GetGameModeStateExitData(state);
#ifdef TARGET_PC
    if (rankedMode()) {
        MatchEnd* end = &mei->match_end;
        if (gm_WasMatchCanceled(end->outcome) || end->is_teams ||
            end->player_standings[0].pkind != Gm_PKind_Human ||
            end->player_standings[1].pkind != Gm_PKind_Human) {
            pc_rank_session_abort("ranked game cancelled or invalid");
        } else {
            unsigned winner = end->n_winners == 1 ? end->winners[0] : PC_RANK_TIE;
            int a = end->player_standings[0].stocks;
            int b = end->player_standings[1].stocks;
            pc_rank_session_game(winner, a < 0 ? 0 : a, b < 0 ? 0 : b,
                                 gmVsMelee_StartData.rules.stkind, end->frame_count);
        }
        /* Ties return through CSS to a fresh one-stock, three-minute game;
         * vanilla sudden death would begin at 300 percent. */
        gm_SetNextGameModeStateId(state_results);
    }
#endif
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
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitSuddenDeath(state);
}

void onEnterResults(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter RESULTS at frame %d", pc_net_frame());
#endif
    gmVsMelee_EnterResults(state);
}

void onExitResults(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitResults(state, &online_vs, state_css);
#ifdef TARGET_PC
    if (rankedMode()) {
        /* The results exit is driven by synchronized pads. Reliable-message
         * arrival and disk speed must never choose different next scenes.
         * Both peers leave a terminal set for the lobby; that scene keeps
         * the connection alive until signing/saving finishes. */
        if (pc_rank_session_set_complete() ||
            gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome))
            gm_SetNextGameModeStateId(state_lobby);
    }
#endif
    if (!gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome)) {
        gm_801623A4(&gmVsMelee_ResultsEnterData.match_end);
    }
}

/* ---- lobby scene ------------------------------------------------------- */
#ifdef TARGET_PC
static char profile_message[ONLINE_LOBBY_MSG_LEN];
static void profileRefresh(void) {
    const char* code = pc_net_match_local_code();
    if (!code || !*code) {
        snprintf(profile_message, sizeof profile_message, "Identity unavailable. Check your profile files.");
        return;
    }
    PcNetRankStoreResult result;
    PcNetRankStore* store = pc_rank_store_open(pc_net_match_profile_directory(),
        pc_net_match_identity()->public_key, &result);
    PcNetRating rating;
    unsigned count = 0;
    if (store && pc_rank_store_current(store, &rating, NULL, &count)) {
        double score = pc_rank_display(&rating);
        const char* tier = count < 5 ? "Placement" : score < 1050 ? "Bronze" :
            score < 1200 ? "Silver" : score < 1350 ? "Gold" : score < 1500 ? "Platinum" :
            score < 1650 ? "Diamond" : "Master";
        snprintf(profile_message, sizeof profile_message, "%s %.0f - %u sets. Community rating, unverified. B: back", tier, score, count);
    } else snprintf(profile_message, sizeof profile_message, "Rating history unavailable or damaged. B: back");
    pc_rank_store_close(store);
}
static bool internetLobby(void) {
    return online_kind != ONLINE_KIND_LAN && online_kind != ONLINE_KIND_PROFILE &&
           !(online_kind == ONLINE_KIND_DIRECT && getenv("MELEE_LAN_DIRECT"));
}
#endif

void gm_Scene_OnlineLobby_OnEnter(UNUSED void* unused)
{
    mnOnlineLobby_Create();
#ifdef TARGET_PC
    if (online_kind == ONLINE_KIND_PROFILE) {
        profileRefresh();
    } else if (internetLobby() && !awaiting_rank_result &&
               (online_kind != ONLINE_KIND_RANKED ||
                                  pc_net_match_publication(NULL) == 0)) {
        if (pc_net_peer_status() == PC_NET_PEER_OK) {
            pc_net_match_start(online_kind == ONLINE_KIND_UNRANKED ? PC_MATCH_UNRANKED :
                               online_kind == ONLINE_KIND_RANKED ? PC_MATCH_RANKED : PC_MATCH_DIRECT,
                               online_kind == ONLINE_KIND_DIRECT ? pc_get_net_target() : NULL);
        }
    } else if (!internetLobby()) pc_lan_start();
#endif
}

void gm_Scene_OnlineLobby_OnExit(UNUSED void* unused)
{
    mnOnlineLobby_Destroy();
}

#ifdef TARGET_PC
static const char* const peer_word[] = { "", "Peer left",
                                         "Connection timed out", "Desync",
                                         "Incompatible version",
                                         "Could not resume" };

static void lobbyCopyName(char* dst, const char* src)
{
    snprintf(dst, ONLINE_LOBBY_NAME_LEN, "%s", src);
}

/* Peers on our protocol and build: the only ones counted or started with. */
static int lobbyCompatible(const PcLanPeer* peers, int n)
{
    int i, nc = 0;
    for (i = 0; i < n; i++) {
        nc += peers[i].compatible;
    }
    return nc;
}

/* Fill the view from the LAN state; logs the status line when it changes. */
static void lobbyFillView(OnlineLobbyView* view, int state, const char* why,
                          const PcLanPeer* peers, int n)
{
    /* Indexed by pc_net_quality() 0..3 and pc_net_peer_status() 0..5;
     * anything outside stays blank. */
    static const char* const link_word[] = { "stable", "warning", "stalling",
                                             "reconnecting" };
    static char last_status[ONLINE_LOBBY_MSG_LEN];
    char status[ONLINE_LOBBY_MSG_LEN];
    bool connected = state == 1 || state == 2;
    bool host = connected && pc_lan_is_host();
    int ping = -1, delay;
    unsigned rollbacks;
    int quality, reason;
    int nc = lobbyCompatible(peers, n);
    int i;

    memset(view, 0, sizeof *view);
    view->title =
        online_kind == ONLINE_KIND_DIRECT ? "DIRECT CONNECT" : "LAN PLAY";
    if (connected && !pc_net_stats(&ping, &delay, &rollbacks)) {
        ping = -1;
    }
    quality = connected ? pc_net_quality() : -1;
    if (quality >= 0 && quality < (int) ARRAY_SIZE(link_word)) {
        view->link = link_word[quality];
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
        p->incompatible = !peers[i].compatible;
        /* The session peer: the host we joined, or our first peer as host. */
        p->ping_ms = connected && (peers[i].host || (host && i == 0)) ? ping : -1;
    }

    switch (state) {
    case 0:
        view->phase = nc == 0 ? LOBBY_PHASE_SEARCHING : LOBBY_PHASE_FOUND;
        if (nc == 0) {
            snprintf(status, sizeof status, "LAN: searching...%s",
                     pc_lan_full() ? " - Lobby full" : "");
        } else {
            snprintf(status, sizeof status, "%d players found - press START%s",
                     nc + 1, pc_lan_full() ? " - Lobby full" : "");
        }
        break;
    case 1:
        view->phase = LOBBY_PHASE_CONNECTING;
        snprintf(status, sizeof status, "Connecting...");
        break;
    case 4:
        view->phase = LOBBY_PHASE_CONNECTING;
        snprintf(status, sizeof status, "Ready - waiting for host...");
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
        reason = pc_net_peer_status();
        if (reason <= 0 || reason >= (int) ARRAY_SIZE(peer_word)) {
            reason = 0;
        }
        snprintf(status, sizeof status, "Failed: %s%s%s",
                 why != NULL ? why : "unknown error", reason ? " - " : "",
                 peer_word[reason]);
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

    if (online_kind == ONLINE_KIND_PROFILE || internetLobby()) {
        memset(&view, 0, sizeof view);
        view.title = online_kind == ONLINE_KIND_PROFILE ? "PROFILE" :
                     online_kind == ONLINE_KIND_UNRANKED ? "UNRANKED" :
                     online_kind == ONLINE_KIND_RANKED ? "RANKED" : "DIRECT CONNECT";
        view.player_count = 1;
        view.players[0].is_local = true;
        view.players[0].ping_ms = -1;
        lobbyCopyName(view.players[0].name, pc_net_match_local_code());
        if (online_kind == ONLINE_KIND_PROFILE) {
            view.phase = LOBBY_PHASE_FOUND;
            snprintf(view.message, sizeof view.message, "%s", profile_message);
        } else if (awaiting_rank_result && pc_net_match_publication(NULL) == 0) {
            pc_net_poll();
            pc_rank_session_poll();
            int result = pc_rank_session_state(&why);
            if (result == PC_RANK_SESSION_SAVED) {
                pc_net_match_publish_rank();
            }
            view.phase = result == PC_RANK_SESSION_FAILED ? LOBBY_PHASE_ERROR : LOBBY_PHASE_CONNECTING;
            snprintf(view.message, sizeof view.message, "%s",
                     why ? why : "Finishing signed result...");
            if (result == PC_RANK_SESSION_FAILED)
                snprintf(view.message, sizeof view.message, "%.54s - START: retry",
                         why ? why : "Set could not be rated");
            if (result == PC_RANK_SESSION_FAILED && (input & HSD_PAD_START)) {
                awaiting_rank_result = false;
                pc_net_match_start(PC_MATCH_RANKED, NULL);
            }
        } else if (online_kind == ONLINE_KIND_RANKED && pc_net_match_publication(NULL) != 0) {
            pc_net_match_poll_publication();
            int publication = pc_net_match_publication(&why);
            view.phase = publication < 0 ? LOBBY_PHASE_ERROR :
                         publication == 2 ? LOBBY_PHASE_FOUND : LOBBY_PHASE_CONNECTING;
            snprintf(view.message, sizeof view.message, "%s",
                     publication == 2 ? "Rating saved and published. START: next set" :
                     publication < 0 ? "Rating saved locally. START: retry publication" :
                     "Rating saved. Publishing...");
            if ((input & HSD_PAD_START) && publication != 1) {
                if (publication < 0) pc_net_match_publish_rank();
                else {
                    awaiting_rank_result = false;
                    pc_net_match_start(PC_MATCH_RANKED, NULL);
                }
            }
        } else {
            pc_net_match_poll();
            state = pc_net_match_state(&why);
            int reason = pc_net_peer_status();
            view.phase = state == PC_MATCH_READY ? LOBBY_PHASE_STARTING :
                         (state == PC_MATCH_FAIL || reason != PC_NET_PEER_OK) ? LOBBY_PHASE_ERROR :
                         state == PC_MATCH_CONNECT ? LOBBY_PHASE_CONNECTING : LOBBY_PHASE_SEARCHING;
            if (reason != PC_NET_PEER_OK && reason < (int) ARRAY_SIZE(peer_word)) {
                snprintf(view.message, sizeof view.message, "%s - START: search", peer_word[reason]);
            } else {
                snprintf(view.message, sizeof view.message, "%s", why ? why : "Searching for an opponent...");
            }
            const char* peer = pc_net_match_opponent_code();
            if (peer && peer[0]) {
                view.player_count = 2;
                lobbyCopyName(view.players[1].name, peer);
                view.players[1].ping_ms = -1;
            }
            if (state == PC_MATCH_READY && pc_net_frame() >= pc_net_match_start_frame()) {
                *HSD_RandSeedPtr = pc_net_match_seed();
                pc_log_line("lobby: entering CSS at frame %d, seed %u", pc_net_frame(), pc_net_match_seed());
                gm_801A4B60();
            }
            if ((input & HSD_PAD_START) && (state == PC_MATCH_FAIL || reason != PC_NET_PEER_OK)) {
                pc_net_peer_status_clear();
                pc_net_match_start(online_kind == ONLINE_KIND_RANKED ? PC_MATCH_RANKED : PC_MATCH_UNRANKED, NULL);
            }
        }
        mnOnlineLobby_Update(&view);
        if (input & (HSD_PAD_B | PAD_CANCEL)) {
            sfxBack();
            pc_net_peer_status_clear();
            pc_net_match_stop();
            gm_ChangeGameModeAfterCurrentScene(GM_MENU);
            gm_801A4B60();
        }
        return;
    }
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
    if (input & (HSD_PAD_B | PAD_CANCEL)) {
        sfxBack();
        pc_net_peer_status_clear();
        pc_lan_stop();
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
    } else if ((input & HSD_PAD_START) && state == 0 && lobbyCompatible(peers, n)) {
        sfxForward();
        pc_lan_start_match();
    }
#endif
}
