#ifndef MELEE_GM_GMONLINEMODE_H
#define MELEE_GM_GMONLINEMODE_H

#include <melee/gm/types.h>

/* PC only: GM_ONLINE. Lobby (GS_ONLINE_LOBBY) -> CSS -> SSS -> VS -> results
 * -> CSS, every scene after the lobby driven by synced inputs
 * (docs/netcode-plan.md §8, §10). */

extern GameModeState gm_Mode_Online_States[];

/* Which online flavour the menu picked before entering GM_ONLINE. */
typedef enum OnlineKind {
    ONLINE_KIND_LAN = 0,
    ONLINE_KIND_DIRECT = 1,
    ONLINE_KIND_UNRANKED = 2,
    ONLINE_KIND_RANKED = 3,
    ONLINE_KIND_PROFILE = 4,
} OnlineKind;

void gmOnline_SetKind(OnlineKind kind);
OnlineKind gmOnline_GetKind(void);

void gm_Scene_OnlineLobby_OnFrame(void);
void gm_Scene_OnlineLobby_OnEnter(void*);
void gm_Scene_OnlineLobby_OnExit(void*);

/* ---- lobby view (src/melee/mn/mnonlinelobby.c) --------------------------
 * The lobby scene owns the flow and the network; the view only draws. The
 * scene fills an OnlineLobbyView every frame and calls mnOnlineLobby_Update. */

#define ONLINE_LOBBY_MAX_PLAYERS 8
#define ONLINE_LOBBY_NAME_LEN 16
#define ONLINE_LOBBY_MSG_LEN 96

typedef struct OnlineLobbyPlayer {
    char name[ONLINE_LOBBY_NAME_LEN];
    int ping_ms;   /* -1 = unknown */
    bool is_host;
    bool is_local;
    bool incompatible; /* other protocol/build; listed, never counted */
} OnlineLobbyPlayer;

typedef enum OnlineLobbyPhase {
    LOBBY_PHASE_SEARCHING,  /* announcing, nobody found yet */
    LOBBY_PHASE_FOUND,      /* players listed; Start begins */
    LOBBY_PHASE_CONNECTING, /* host elected, session connecting */
    LOBBY_PHASE_STARTING,   /* handshake done, counting down to CSS */
    LOBBY_PHASE_ERROR,      /* message explains; B goes back */
} OnlineLobbyPhase;

/* Direct connect's own screens, drawn in place of the player list. */
#define ONLINE_LOBBY_MENU_ROWS 8
#define ONLINE_LOBBY_KEY_ROWS 4
#define ONLINE_LOBBY_KEY_COLS 8

typedef enum OnlineLobbyScreen {
    LOBBY_SCREEN_PLAYERS, /* the player list (LAN, searching, connecting) */
    LOBBY_SCREEN_MENU,    /* a list of choices with a cursor */
    LOBBY_SCREEN_KEYS,    /* the code keyboard */
} OnlineLobbyScreen;

typedef struct OnlineLobbyView {
    const char* title;                 /* "LAN PLAY" / "DIRECT CONNECT" */
    OnlineLobbyScreen screen;
    char subtitle[ONLINE_LOBBY_MSG_LEN]; /* top right: e.g. your own code */
    /* MENU: rows and the cursor; KEYS: `menu[0]` is the entry line. */
    char menu[ONLINE_LOBBY_MENU_ROWS][ONLINE_LOBBY_MSG_LEN];
    int menu_count;
    int cursor;
    const char* keys; /* KEYS: ROWS*COLS characters, row-major */
    int key_cursor;
    const char* hint; /* the button line, or NULL for the default */
    OnlineLobbyPhase phase;
    OnlineLobbyPlayer players[ONLINE_LOBBY_MAX_PLAYERS];
    int player_count;                  /* includes the local player */
    char message[ONLINE_LOBBY_MSG_LEN]; /* one status line, may be "" */
    const char* link;                  /* quality word by the ping, or NULL */
    int countdown_frames;              /* STARTING only, else 0 */
} OnlineLobbyView;

void mnOnlineLobby_Create(void);
void mnOnlineLobby_Update(const OnlineLobbyView* view);
void mnOnlineLobby_Destroy(void);

#endif
