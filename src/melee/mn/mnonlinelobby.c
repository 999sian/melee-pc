/* PC only: the online lobby view (docs/netcode-plan.md §8, Double Dash LAN
 * counter screen). gmonlinemode.c owns the flow and the network and hands us
 * an OnlineLobbyView every frame; this file only draws it: the main-menu
 * backdrop (MnMaAll, via the exported mnmain.c helpers), a translucent panel
 * and SIS text on the same ortho canvas the title screen's build stamp uses. */

#include <melee/gm/gmonlinemode.h>

#include <stdio.h>
#include <string.h>

#include "mnmain.h"
#include <dolphin/gx/GXStruct.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lblanguage.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/displayfunc.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/hsd_3915.h>
#include <sysdolphin/baselib/sislib.h>

#ifdef TARGET_PC

/* Full-width parentheses: the SIS ASCII encoder (HSD_SisLib_803A67EC) only
 * knows space ' " , - . : 0-9 A-Z a-z and reads anything else as an SJIS
 * pair, which the font atlas does carry for these two. */
#define SJIS_LPAREN "\x81\x69"
#define SJIS_RPAREN "\x81\x6A"

#define ROW_Y0 128.0f
#define ROW_DY 32.0f
#define COL_YOU 48.0f
#define COL_NAME 104.0f
#define COL_HOST 336.0f
#define COL_PING 480.0f

typedef struct Line {
    int entry;
    char str[ONLINE_LOBBY_MSG_LEN];
} Line;

enum {
    line_title,
    line_status,
    line_hint,
    line_countdown,
    line_subtitle,
    line_rows, /* 4 per player: YOU marker, name, host badge, ping */
    line_menu = line_rows + ONLINE_LOBBY_MAX_PLAYERS * 4, /* cursor + text */
    line_keys = line_menu + ONLINE_LOBBY_MENU_ROWS * 2,
    line_count = line_keys + ONLINE_LOBBY_KEY_ROWS * ONLINE_LOBBY_KEY_COLS,
};

#define KEY_X0 136.0f
#define KEY_DX 48.0f
#define KEY_Y0 200.0f
#define KEY_DY 40.0f

static HSD_Text* lobby_text;
static Line lines[line_count];
static int lobby_frame;
static bool lobby_error_tint;
static int key_lit = -1; /* the key drawn highlighted */

static GXColor col_white = { 0xFF, 0xFF, 0xFF, 0xFF };
static GXColor col_dim = { 0xB0, 0xB0, 0xB0, 0xFF };
static GXColor col_you = { 0xFF, 0xE0, 0x60, 0xFF };
static GXColor col_host = { 0xFF, 0xA0, 0x40, 0xFF };
static GXColor col_error = { 0xFF, 0x60, 0x60, 0xFF };

static void addLine(Line* l, float x, float y, float scale, GXColor* color)
{
    l->entry = HSD_SisLib_803A6B98(lobby_text, x, y, "%s", l->str);
    HSD_SisLib_803A7548(lobby_text, l->entry, scale, scale);
    HSD_SisLib_803A74F0(lobby_text, l->entry, color);
}

/* Only entries whose text changed touch the SIS buffer, so a steady lobby
 * costs nothing per frame. */
static void setLine(Line* l, const char* s)
{
    if (strcmp(l->str, s) == 0) {
        return;
    }
    snprintf(l->str, sizeof(l->str), "%s", s);
    HSD_SisLib_803A70A0(lobby_text, l->entry, "%s", l->str);
}

static void setColor(Line* l, GXColor* c)
{
    HSD_SisLib_803A74F0(lobby_text, l->entry, c);
}

/* Peer names come off the network; keep them inside the encoder's ASCII set
 * so a stray byte cannot swallow the character after it. */
static void sanitizeName(char* dst, size_t cap, const char* src)
{
    size_t i;
    for (i = 0; i + 1 < cap && src[i] != '\0'; i++) {
        char c = src[i];
        /* '#' is in the set now: the encoder draws it (hsd_3A64.c) instead
         * of eating the byte after it, and every connect code has one. */
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') || c == ' ' || c == '-' || c == '.' ||
                  c == '#';
        dst[i] = ok ? c : '-';
    }
    dst[i] = '\0';
}

/* Same 2D immediate-mode setup as lbbgflash.c / the debug console, under the
 * SIS canvas camera; that camera's ortho space has y growing downward as
 * negative, hence the sign flips. Once per frame: the camera runs three
 * passes and SIS text draws on the last, so we do too. */
static void drawPanel(UNUSED HSD_GObj* gobj, int pass)
{
    static GXColor panel = { 0, 0, 0, 0xA8 };
    static GXColor rule = { 0xFF, 0xFF, 0xFF, 0x80 };
    static GXColor rule_dim = { 0xFF, 0xFF, 0xFF, 0x50 };

    if (pass != 2) {
        return;
    }
    hsd_80391A04(1.0f, 1.0f, 1);
    DrawRectangle(32.0f, -456.0f, 576.0f, 432.0f, &panel);
    DrawRectangle(48.0f, -98.0f, 544.0f, 2.0f, &rule);
    DrawRectangle(48.0f, -390.0f, 544.0f, 2.0f, &rule_dim);
}

void mnOnlineLobby_Create(void)
{
    void* dp_[4];
    HSD_GObj* panel;
    int i;

    /* Main-menu backdrop: background model, camera (erases with the fog
     * colour), fog and lights, exactly what mnMain_Scene_OnEnter builds. */
    mn_804D6BB8 = lbArchive_LoadSymbols(
        "MnMaAll", &dp_[0], "MenMainBack_Top_joint", &dp_[1],
        "MenMainBack_Top_animjoint", &dp_[2], "MenMainBack_Top_matanim_joint",
        &dp_[3], "MenMainBack_Top_shapeanim_joint", &MenMain_cam,
        "ScMenMain_cam_int1_camera", &MenMain_lights, "ScMenMain_scene_lights",
        &MenMain_fog, "ScMenMain_fog", 0);
    DP_SET(MenMainBack_Top.joint, dp_[0]);
    DP_SET(MenMainBack_Top.animjoint, dp_[1]);
    DP_SET(MenMainBack_Top.matanim_joint, dp_[2]);
    DP_SET(MenMainBack_Top.shapeanim_joint, dp_[3]);
    if (lbLang_IsSavedLanguageUS()) {
        HSD_SisLib_803A62A0(0, "SdMenu.usd", "SIS_MenuData");
    } else {
        HSD_SisLib_803A62A0(0, "SdMenu.dat", "SIS_MenuData");
    }
    mn_8022C304();
    mn_8022BCF8();
    mn_8022BE34();
    mn_80229B2C();

    /* Ortho 640x480 canvas drawn after the menu camera (render prio 0x13),
     * as gmtitle.c's build stamp. The panel gobj goes on the same GX link
     * before any text so it renders underneath. */
    HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
    panel = GObj_Create(9, 0xD, 0);
    GObj_SetupGXLink(panel, drawPanel, 0xE, 0);
    lobby_text = HSD_SisLib_803A6754(0, 0);
    lobby_text->default_kerning = 1;

    memset(lines, 0, sizeof(lines));
    lobby_frame = 0;
    lobby_error_tint = false;
    key_lit = -1;
    addLine(&lines[line_title], 48.0f, 40.0f, 1.0f, &col_white);
    /* Right of the title (which ends at x ~370), on its baseline. */
    addLine(&lines[line_subtitle], 360.0f, 60.0f, 0.45f, &col_you);
    for (i = 0; i < ONLINE_LOBBY_MENU_ROWS; i++) {
        float y = ROW_Y0 + i * ROW_DY;
        addLine(&lines[line_menu + i * 2], COL_YOU, y, 0.55f, &col_you);
        addLine(&lines[line_menu + i * 2 + 1], COL_NAME - 24.0f, y, 0.55f, &col_white);
    }
    for (i = 0; i < ONLINE_LOBBY_KEY_ROWS * ONLINE_LOBBY_KEY_COLS; i++) {
        addLine(&lines[line_keys + i], KEY_X0 + (i % ONLINE_LOBBY_KEY_COLS) * KEY_DX,
                KEY_Y0 + (i / ONLINE_LOBBY_KEY_COLS) * KEY_DY, 0.8f, &col_dim);
    }
    for (i = 0; i < ONLINE_LOBBY_MAX_PLAYERS; i++) {
        float y = ROW_Y0 + i * ROW_DY;
        Line* row = &lines[line_rows + i * 4];
        addLine(&row[0], COL_YOU, y, 0.5f, &col_you);
        addLine(&row[1], COL_NAME, y, 0.6f, &col_white);
        addLine(&row[2], COL_HOST, y, 0.5f, &col_host);
        addLine(&row[3], COL_PING, y, 0.5f, &col_dim);
    }
    /* Top right, beside the title: eight rows fill the list area. */
    addLine(&lines[line_countdown], 360.0f, 56.0f, 0.7f, &col_you);
    addLine(&lines[line_status], 48.0f, 392.0f, 0.55f, &col_white);
    addLine(&lines[line_hint], 48.0f, 418.0f, 0.45f, &col_dim);
}

void mnOnlineLobby_Update(const OnlineLobbyView* view)
{
    char buf[64];
    int i;
    bool error;

    if (lobby_text == NULL) {
        return;
    }
    lobby_frame++;
    setLine(&lines[line_title], view->title != NULL ? view->title : "");
    setLine(&lines[line_subtitle], view->subtitle);

    /* MENU and KEYS replace the player list; the rows they do not use are
     * blanked, so switching screens leaves nothing behind. */
    for (i = 0; i < ONLINE_LOBBY_MENU_ROWS; i++) {
        bool shown = view->screen != LOBBY_SCREEN_PLAYERS && i < view->menu_count;
        bool here = shown && view->screen == LOBBY_SCREEN_MENU && i == view->cursor;
        setLine(&lines[line_menu + i * 2], here ? ">" : "");
        setLine(&lines[line_menu + i * 2 + 1], shown ? view->menu[i] : "");
        if (shown) {
            setColor(&lines[line_menu + i * 2 + 1],
                     here || view->screen == LOBBY_SCREEN_KEYS ? &col_white : &col_dim);
        }
    }
    for (i = 0; i < ONLINE_LOBBY_KEY_ROWS * ONLINE_LOBBY_KEY_COLS; i++) {
        char key[2] = { 0, 0 };
        if (view->screen == LOBBY_SCREEN_KEYS && view->keys != NULL) {
            key[0] = view->keys[i];
        }
        setLine(&lines[line_keys + i], key);
    }
    {
        int lit = view->screen == LOBBY_SCREEN_KEYS ? view->key_cursor : -1;
        if (lit != key_lit) {
            if (key_lit >= 0) {
                setColor(&lines[line_keys + key_lit], &col_dim);
            }
            if (lit >= 0) {
                setColor(&lines[line_keys + lit], &col_you);
            }
            key_lit = lit;
        }
    }

    for (i = 0; i < ONLINE_LOBBY_MAX_PLAYERS; i++) {
        Line* row = &lines[line_rows + i * 4];
        if (view->screen == LOBBY_SCREEN_PLAYERS && i < view->player_count) {
            const OnlineLobbyPlayer* p = &view->players[i];
            setLine(&row[0], p->is_local ? "YOU" : "");
            sanitizeName(buf, ONLINE_LOBBY_NAME_LEN, p->name);
            setLine(&row[1], buf);
            setColor(&row[1], p->is_local ? &col_you : &col_white);
            setLine(&row[2], p->is_host ? SJIS_LPAREN "HOST" SJIS_RPAREN
                             : p->incompatible
                                 ? SJIS_LPAREN "other version" SJIS_RPAREN
                                 : "");
            if (p->ping_ms >= 0) {
                snprintf(buf, sizeof(buf), "%dms %s", p->ping_ms,
                         view->link != NULL ? view->link : "");
            } else {
                buf[0] = '\0';
            }
            setLine(&row[3], buf);
        } else {
            setLine(&row[0], "");
            setLine(&row[1], "");
            setLine(&row[2], "");
            setLine(&row[3], "");
        }
    }

    if (view->phase == LOBBY_PHASE_STARTING) {
        snprintf(buf, sizeof(buf), "Starting in %d...",
                 view->countdown_frames / 60 + 1);
    } else {
        buf[0] = '\0';
    }
    setLine(&lines[line_countdown], buf);

    setLine(&lines[line_status], view->message);
    error = view->phase == LOBBY_PHASE_ERROR;
    if (error != lobby_error_tint) {
        lobby_error_tint = error;
        setColor(&lines[line_status], error ? &col_error : &col_white);
    }

    if (view->hint != NULL) {
        setLine(&lines[line_hint], view->hint);
        setColor(&lines[line_hint], &col_dim);
    } else if (view->phase == LOBBY_PHASE_FOUND) {
        setLine(&lines[line_hint], "START: begin    B: back");
        /* Subtle blink on the START hint while it is actionable. */
        setColor(&lines[line_hint],
                 (lobby_frame / 30) & 1 ? &col_dim : &col_white);
    } else {
        setLine(&lines[line_hint], "B: back");
        setColor(&lines[line_hint], &col_dim);
    }
}

void mnOnlineLobby_Destroy(void)
{
    if (lobby_text != NULL) {
        HSD_SisLib_803A5CC4(lobby_text);
        lobby_text = NULL;
    }
}

#else

void mnOnlineLobby_Create(void) {}
void mnOnlineLobby_Update(UNUSED const OnlineLobbyView* view) {}
void mnOnlineLobby_Destroy(void) {}

#endif
