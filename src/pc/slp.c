/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi replay recording (MELEE_SLP_DIR, see slp.h). This file reads game
 * state into host structs; slp_format.c turns those into bytes.
 *
 * Every field is read through the decomp's named struct members and written
 * at SPEC.md's offset. The addresses in the comments are the GameCube player
 * block offsets Slippi's recording ASM (project-slippi/slippi-ssbm-asm
 * Recording/) reads, so each mapping can be checked against it.
 *
 * Capture happens once per simulation tick, keyed by the netplay frame the
 * tick simulates: the state entering the tick when it begins (pre-frame), the
 * inputs and resulting state when it ends (post-frame, items, stage). A
 * rollback re-runs a tick and overwrites its slot; a frame is serialized only
 * when it is confirmed, in order, so the file holds the pass that stood.
 * Replay frame numbers are handed out at that point: -123 for the first
 * confirmed tick in which the fighters ran, one more for each after it. A
 * paused tick, and every tick from GAME! on, has the fighters' procs masked
 * and is no replay frame -- as in a Slippi file, where the frame index only
 * moves while the match runs. */
#include "compat.h"
#include "pc/net.h"
#include "pc/pc.h"
#include "pc/region.h"
#include "pc/slp.h"
#include "pc/slp_format.h"

#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/random.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/ft/fighter.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/forward.h>
#include <melee/ft/types.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/types.h>
#include <melee/gr/forward.h>
#include <melee/gr/types.h>
#include <melee/it/forward.h>
#include <melee/it/types.h>
#include <melee/lb/lblanguage.h>
#include <melee/mn/mnname.h>
#include <melee/mn/types.h>
#include <melee/pl/player.h>
#pragma GCC diagnostic pop

#include <SDL3/SDL_time.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct GameSceneInfo* gm_804D6720; /* current scene, gmscene.c */

#define SLP_RING 64 /* frames staged: the size of net_internal.h's RING */
#define NO_FRAME INT32_MIN

/* Game bytes are MSB-first bit order: the first-declared bitfield of a byte
 * is its 0x80 bit on the GameCube, whatever the host compiler packs. */
#define BITS8(a, b, c, d, e, f, g, h)                                                              \
    (uint8_t)((!!(a) << 7) | (!!(b) << 6) | (!!(c) << 5) | (!!(d) << 4) | (!!(e) << 3) |           \
              (!!(f) << 2) | (!!(g) << 1) | !!(h))

typedef struct Staged {
    int32_t net_frame;   /* NO_FRAME when empty */
    bool frame;          /* the fighters ran: this tick is a replay frame */
    uint8_t result;      /* VsSceneState.match_result after the tick */
    SlpGameEnd game_end; /* when result is set */
    SlpFrameStart start;
    int nf;
    SlpPreFrame pre[SLP_MAX_FIGHTERS];
    SlpPostFrame post[SLP_MAX_FIGHTERS];
    int ni;
    SlpItem items[SLP_MAX_ITEMS];
    int nfod;
    uint8_t fod_side[2];
    float fod_height[2];
    bool have_whispy, have_ps;
    uint8_t whispy;
    uint16_t ps_event, ps_type;
    uint32_t vs_frame_count; /* VsSceneState.frame_count, for the frame 0 check */
} Staged;

/* State entering the tick's GObj procs, taken when they begin, and each
 * fighter's pre-frame event, taken where Slippi takes it. */
typedef struct Begin {
    bool on;
    int32_t net_frame;
    uint32_t seed, scene_frame;
    HSD_GObj* gobj[SLP_MAX_FIGHTERS];
    bool have_pre[SLP_MAX_FIGHTERS];
    SlpPreFrame pre[SLP_MAX_FIGHTERS];
    bool raw_ok[4];
    PADStatus raw[4];
} Begin;

static const char* s_dir;
static bool s_rec; /* a match is being recorded */
static bool s_net; /* ...in a netplay session */
static bool s_atexit;
static uint16_t s_stage; /* StKind */
static Staged s_ring[SLP_RING];
static Begin s_begin;
static bool s_have;          /* at least one tick staged */
static int32_t s_emitted;    /* newest net frame serialized (or skipped) */
static int32_t s_staged_max; /* newest net frame staged */
static int32_t s_frame;      /* newest replay frame serialized */
static bool s_ended;         /* Game End serialized */
static bool s_timer_logged;
static unsigned s_lost;
static uint32_t s_usage[4][64]; /* post-frames per port per internal character */
static char s_start_at[32];
/* The stage events are sent when the value changes; what was sent last. */
static float s_fod_last[2];
static uint8_t s_whispy_last;
static uint16_t s_ps_last_event, s_ps_last_type;
/* Final values, for the log line a test compares against the game's. */
static SlpPostFrame s_last_post[SLP_MAX_FIGHTERS];
static int s_last_nf;

static bool slp_scene(void) {
    const int k = gm_804D6720 != NULL ? gm_804D6720->scene_kind : -1;
    return k == GS_VS || k == GS_SUDDEN_DEATH;
}

/* The fighter recorded as (port, follower): the active one of the port, or
 * its Ice Climbers partner. NULL when the port is empty or has none. */
static HSD_GObj* fighter_gobj(int port, bool follower) {
    if (Player_GetPlayerSlotType(port) == Gm_PKind_NA) {
        return NULL;
    }
    HSD_GObj* g = follower ? Player_GetEntityAtIndex(port, 1) : Player_GetEntity(port);
    if (g == NULL || g->classifier != HSD_GOBJ_CLASS_FIGHTER || g->user_data == NULL) {
        return NULL;
    }
    if (follower && GET_FIGHTER(g)->kind != Ft_Kind_Nana) {
        return NULL;
    }
    return g;
}

/* ---- match start: Event Payloads and Game Start ----------------------- */

static void build_info(SlpInfoBlock* b, const StartMeleeData* d) {
    const StartMeleeRules* r = &d->rules;
    memset(b, 0, sizeof *b);
    b->match_kind = (uint8_t)r->match_kind;
    b->x0_3 = (uint8_t)r->x0_3;
    b->timer_enabled = r->timer_enabled;
    b->timer_counts_up = r->timer_counts_up;
    b->bits1 = BITS8(r->x1_0, r->x1_1, r->x1_2, r->x1_3, r->x1_4, r->x1_5, r->timer_shows_hours,
        r->friendly_fire);
    b->bits2 = BITS8(r->is_stock, r->x2_1, r->x2_2, r->single_button, r->disable_pausing, r->x2_5,
        r->x2_6, r->x2_7);
    b->bits3 = BITS8(r->x3_0, r->x3_1, r->x3_2, r->x3_3, r->x3_4, r->x3_5, r->x3_6, r->x3_7);
    b->bits4 = BITS8(r->x4_0, r->is_vs, r->x4_2, r->x4_3, r->x4_4, r->x4_5, r->x4_6, r->x4_7);
    b->bits5 = BITS8(r->x5_0, r->x5_1, r->x5_2, r->x5_3, r->x5_4, r->x5_5, r->x5_6, r->x5_7);
    b->x6 = r->x6;
    b->x7 = r->x7;
    b->is_teams = r->is_teams;
    b->x9 = r->x9;
    b->xA = r->xA;
    b->item_freq = r->item_freq;
    b->sd_penalty = r->sd_penalty;
    b->xD = r->xD;
    b->stkind = r->stkind;
    b->time_limit = r->time_limit;
    b->x14 = r->x14;
    b->x18 = r->x18;
    b->x1C = r->x1C_pad[0];
    b->item_mask = r->x20;
    b->x28 = r->x28;
    b->x2C = r->x2C;
    b->damage_ratio = r->x30;
    b->game_speed = r->game_speed;
    for (int i = 0; i < 6; i++) {
        const PlayerInitData* p = &d->players[i];
        SlpInfoPlayer* o = &b->players[i];
        o->ckind = p->ckind;
        o->slot_type = p->slot_type;
        o->stocks = p->stocks;
        o->color = p->color;
        o->slot = p->slot;
        o->spawn_pos = p->spawn_pos;
        o->spawn_dir = p->spawn_dir;
        o->sub_color = p->sub_color;
        o->handicap = p->handicap;
        o->team = p->team;
        o->nametag = p->nametag;
        o->xB = p->xB;
        o->flags_c = BITS8(p->rumble_enabled, p->xC_b1, p->vs_metal, p->xC_b3, p->vs_invisible,
            p->xC_b5, p->xC_b6, p->xC_b7);
        o->flags_d =
            BITS8(p->xD_b0, p->xD_b1, p->xD_b2, p->xD_b3, p->xD_b4, p->xD_b5, p->xD_b6, p->xD_b7);
        o->cpu_kind = p->cpu_kind;
        o->cpu_level = p->cpu_level;
        o->damage = p->damage;
        o->damage1 = p->damage1;
        o->hp = p->hp;
        o->attack_ratio = p->attack_ratio;
        o->defense_ratio = p->defense_ratio;
        o->model_scale = p->model_scale;
        /* Holding A as the match loads starts Zelda as Sheik and Sheik as
         * Zelda. The block records the character that starts, the rule
         * gmvs.c fn_8016D8AC applies right after this hook (Slippi's
         * SendGameInfo makes the same substitution). */
        if (p->slot_type == Gm_PKind_Human && (p->ckind == CKind_Zelda || p->ckind == CKind_Seak)) {
            const int pid = p->slot == 0 ? i : p->slot - 1;
            if (HSD_PadCopyStatus[(u8)pid].button & HSD_PAD_A) {
                o->ckind = p->ckind == CKind_Zelda ? CKind_Seak : CKind_Zelda;
            }
        }
    }
}

static bool recorded_mode(void) {
    switch (gm_GetCurrentGameMode()) {
    case GM_VS:
    case GM_DEBUG_VS:
    case GM_ONLINE:
    case GM_TOURNAMENT:
    case GM_SUPER_SUDDEN_DEATH_VS:
    case GM_INVISIBLE_VS:
    case GM_SLOMO_VS:
    case GM_LIGHTNING_VS:
    case GM_TINY_VS:
    case GM_GIANT_VS:
    case GM_STAMINA_VS:
    case GM_SINGLE_BUTTON_VS:
    case GM_CAMERA_VS:
        return true;
    default:
        return false;
    }
}

static void finish(const char* why);
static void fill_pre(SlpPreFrame* pre, int i, const Fighter* fp);
static void confirm(int32_t upto);

static void on_exit_finish(void) {
    finish("exit");
    slp_writer_shutdown();
}

void pc_slp_match_start(const struct StartMeleeData* data) {
    if (s_rec) {
        finish("a new match began"); /* the last scene left without its exit hook */
    }
    if (s_dir == NULL) {
        const char* d = getenv("MELEE_SLP_DIR");
        s_dir = d != NULL ? d : "";
    }
    if (s_dir[0] == '\0' || data == NULL || !recorded_mode()) {
        return;
    }
    const int scene = gm_804D6720 != NULL ? gm_804D6720->scene_kind : -1;

    SDL_Time now = 0;
    SDL_DateTime local, utc;
    SDL_GetCurrentTime(&now);
    if (!SDL_TimeToDateTime(now, &local, true)) {
        memset(&local, 0, sizeof local);
    }
    if (!SDL_TimeToDateTime(now, &utc, false)) {
        memset(&utc, 0, sizeof utc);
    }
    char stem[64];
    snprintf(stem, sizeof stem, "Game_%04d%02d%02dT%02d%02d%02d", local.year, local.month,
        local.day, local.hour, local.minute, local.second);
    snprintf(s_start_at, sizeof s_start_at, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.year, utc.month,
        utc.day, utc.hour, utc.minute, utc.second);

    SlpInfoBlock info;
    build_info(&info, data);
    SlpGameStart gs;
    memset(&gs, 0, sizeof gs);
    slp_info_block(gs.info, &info);
    gs.seed = *HSD_RandSeedPtr;
    const uint32_t ucf = pc_is_ucf_enabled() ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        gs.dashback[i] = ucf;
        gs.shield_drop[i] = ucf;
        const PlayerInitData* p = &data->players[i];
        if (p->slot_type != Gm_PKind_NA && p->nametag != 0x78) {
            const char* tag = GetNameText(p->nametag);
            if (tag != NULL) {
                /* NameTagData.namedata: eight bytes of Shift JIS */
                memcpy(gs.nametag[i], tag, strnlen(tag, 8));
            }
        }
    }
    gs.pal = pc_region_pal;
    gs.frozen_ps = pc_is_frozen_stadium_enabled();
    gs.minor_scene = scene == GS_SUDDEN_DEATH ? 3 : 2;
    /* SPEC: 2 for VS mode, 8 for an online game (one with rollbacks) */
    s_net = pc_net_active();
    gs.major_scene = s_net ? 8 : 2;
    gs.language = (uint8_t)lbLang_GetSavedLanguage();

    uint8_t head[SLP_EVENT_PAYLOADS_SIZE + SLP_GAME_START_SIZE];
    size_t n = slp_event_payloads(head);
    n += slp_game_start(head + n, &gs);
    slp_writer_begin(s_dir, stem);
    slp_writer_append(head, n);

    for (int i = 0; i < SLP_RING; i++) {
        s_ring[i].net_frame = NO_FRAME;
    }
    memset(&s_begin, 0, sizeof s_begin);
    memset(s_usage, 0, sizeof s_usage);
    s_stage = data->rules.stkind;
    s_have = false;
    s_frame = SLP_FIRST_FRAME - 1;
    s_ended = false;
    s_timer_logged = false;
    s_lost = 0;
    s_last_nf = 0;
    s_fod_last[0] = s_fod_last[1] = 0.0F;
    s_whispy_last = 0;   /* SendDreamlandInfo: no wind before the first frame */
    s_ps_last_event = 0; /* SendStadiumInfo: word 0x00000005 before the first */
    s_ps_last_type = 5;
    s_rec = true;
    if (!s_atexit) {
        s_atexit = true;
        atexit(on_exit_finish); /* runs before main.c's shutdown, registered earlier */
    }
    pc_log_line(
        "slp: recording match on stage %u (%s)", (unsigned)s_stage, s_net ? "netplay" : "offline");
}

/* ---- per tick ----------------------------------------------------------- */

void pc_slp_tick_begin(void) {
    s_begin.on = s_rec && slp_scene();
    if (!s_begin.on) {
        return;
    }
    const int32_t f = pc_net_sim_frame();
    /* Netplay: what the peer has confirmed (net.c fresh_tick, before this
     * tick). Offline, or once a session has dropped, nothing is ever re-run
     * past the tick after it (the sync test re-runs a tick at once, then
     * moves on): the previous frame is final. */
    confirm(s_net && pc_net_active() ? pc_net_confirmed_frame() : f - 1);
    s_begin.net_frame = f;
    s_begin.seed = *HSD_RandSeedPtr;     /* where Slippi's frame-start proc reads it */
    s_begin.scene_frame = gm_801A4BB8(); /* Slippi's global frame, 0x80479D60 */
    for (int i = 0; i < SLP_MAX_FIGHTERS; i++) {
        s_begin.gobj[i] = fighter_gobj(i / 2, i & 1);
        s_begin.have_pre[i] = false;
    }
    /* The raw sample this tick consumed (HSD_PadRenewMasterStatus, at its
     * start) is the queue entry behind the read cursor: the one Slippi reads
     * its UCF bytes from (gmMain_8046B108 at HSD_PadLibData.qread - 1). In
     * netplay it is the synced sample net.c wrote there. */
    const PadLibData* p = &HSD_PadLibData;
    const bool ok = p->queue != NULL && p->qnum > 0;
    const int slot = ok ? (p->qread + p->qnum - 1) % p->qnum : 0;
    for (int port = 0; port < 4; port++) {
        s_begin.raw_ok[port] = ok && HSD_PadMasterStatus[port].err == 0;
        if (s_begin.raw_ok[port]) {
            s_begin.raw[port] = p->queue[slot].stat[port];
        }
    }
}

static bool landing_air(uint16_t action) {
    return action >= ftCo_MS_LandingAirN && action <= ftCo_MS_LandingAirLw;
}

/* SendGamePreFrame.asm, at its hook: the fighter's state entering its
 * input read (fp+10, +B0, +B4, +2C, +1830) and the processed inputs it just
 * read into its buffer (fp+620 sticks, +638 C-stick, +650 trigger, +65C
 * buttons). */
static void fill_pre(SlpPreFrame* pre, int i, const Fighter* fp) {
    const int port = i / 2;
    memset(pre, 0, sizeof *pre);
    pre->port = (uint8_t)port;
    pre->follower = i & 1;
    pre->seed = *HSD_RandSeedPtr;
    pre->action = (uint16_t)fp->motion_id;
    pre->x = fp->cur_pos.x;
    pre->y = fp->cur_pos.y;
    pre->facing = fp->facing_dir;
    pre->percent = fp->dmg.x1830_percent;
    pre->joy_x = fp->input.lstick[0].x;
    pre->joy_y = fp->input.lstick[0].y;
    pre->c_x = fp->input.cstick[0].x;
    pre->c_y = fp->input.cstick[0].y;
    pre->trigger = fp->input.triggers[0];
    pre->buttons = fp->input.held_buttons[0];
    /* The port's physical pad (HSD_PadMasterStatus, 0x804C1FAC): the low
     * half of its button word and the two analog triggers. */
    const HSD_PadStatus* mp = &HSD_PadMasterStatus[port];
    pre->phys_buttons = (uint16_t)(mp->button & 0xFFFF);
    pre->phys_l = mp->nml_analogL;
    pre->phys_r = mp->nml_analogR;
    if (s_begin.raw_ok[port]) {
        pre->raw_x = s_begin.raw[port].stickX;
        pre->raw_y = s_begin.raw[port].stickY;
        pre->raw_cx = s_begin.raw[port].substickX;
        pre->raw_cy = s_begin.raw[port].substickY;
    }
}

void pc_slp_pre_frame(struct HSD_GObj* gobj) {
    if (!s_begin.on) {
        return;
    }
    for (int i = 0; i < SLP_MAX_FIGHTERS; i++) {
        if (s_begin.gobj[i] == gobj) {
            fill_pre(&s_begin.pre[i], i, GET_FIGHTER(gobj));
            s_begin.have_pre[i] = true;
            return;
        }
    }
}

static void capture_fighter(Staged* s, int i, HSD_GObj* g) {
    const int port = i / 2;
    const bool follower = i & 1;
    const Fighter* fp = GET_FIGHTER(g);
    SlpPreFrame* pre = &s->pre[s->nf];
    SlpPostFrame* po = &s->post[s->nf];
    /* A fighter whose input read did not run this tick (a stamina KO holds
     * it) still gets its pre-frame, as it stands after the tick. */
    if (s_begin.gobj[i] == g && s_begin.have_pre[i]) {
        *pre = s_begin.pre[i];
    } else {
        fill_pre(pre, i, fp);
    }
    memset(po, 0, sizeof *po);

    /* Post-frame: the fighter after the tick (SendGamePostFrame.asm). */
    po->port = (uint8_t)port;
    po->follower = follower;
    po->character = (uint8_t)fp->kind;                   /* fp+4 */
    po->action = (uint16_t)fp->motion_id;                /* fp+10 */
    po->x = fp->cur_pos.x;                               /* fp+B0 */
    po->y = fp->cur_pos.y;                               /* fp+B4 */
    po->facing = fp->facing_dir;                         /* fp+2C */
    po->percent = fp->dmg.x1830_percent;                 /* fp+1830 */
    po->shield = fp->shield_health;                      /* fp+1998 */
    po->last_attack = (uint8_t)fp->x208C;                /* fp+208C, low byte */
    po->combo = (uint8_t)fp->x2090;                      /* fp+2090, low byte */
    po->last_hit_by = (uint8_t)fp->dmg.x18c4_source_ply; /* fp+18C4 */
    po->stocks = (uint8_t)Player_GetStocks(port);
    po->action_frame = fp->cur_anim_frame; /* fp+894 */
    po->flags[0] = BITS8(fp->allow_interrupt, fp->x2218_b1, fp->x2218_b2, fp->reflecting,
        fp->x2218_b4, fp->x2218_b5, fp->x2218_b6, fp->x2218_b7); /* fp+2218 */
    po->flags[1] = BITS8(fp->x221A_b0, fp->x221A_b1, fp->allow_sdi, fp->x221A_b3, fp->fall_fast,
        fp->x221A_b5, fp->x221A_b6, fp->x221A_b7); /* fp+221A */
    po->flags[2] = BITS8(fp->x221B_b0, fp->x221B_b1, fp->x221B_b2, fp->x221B_b3, fp->x221B_b4,
        fp->x221B_b5, fp->x221B_b6, fp->x221B_b7); /* fp+221B */
    /* fp+221C is the high byte of a u16 of bitfields; its last bit is the top
     * bit of the three-bit x221C_u16_y. */
    po->flags[3] = BITS8(fp->x221C_b0, fp->x221C_b1, fp->x221C_b2, fp->x221C_b3, fp->x221C_b4,
        fp->x221C_b5, fp->x221C_b6, (fp->x221C_u16_y >> 2) & 1);
    /* fp+221F. Its 0x80, offscreen, is written by the render pass
     * (ftLib_80086A8C against the current camera), not the simulation: a
     * rollback re-runs ticks without drawing them, so in netplay the bit
     * depends on which predicted frames a peer drew. Left clear there, so
     * both peers' replays hold the same frames. */
    po->flags[4] = BITS8(fp->x221F_b0 && !s_net, fp->x221F_b1, fp->x221F_b2, fp->x221F_b3,
        fp->is_sub_fighter, fp->x221F_b5, fp->x221F_b6, fp->x221F_b7);
    /* fp+2340, the first motion-variable word (hitstun left, a float, while
     * in hitstun): its 32 bits as they are, whatever the state stores. */
    memcpy(&po->misc_as, &fp->mv, sizeof po->misc_as);
    po->airborne = fp->ground_or_air == GA_Air;                          /* fp+E0 */
    po->ground_id = (uint16_t)fp->coll_data.floor.index;                 /* fp+83C */
    po->jumps = (uint8_t)(fp->co_attrs.max_jumps - fp->x1968_jumpsUsed); /* fp+168 - fp+1968 */
    /* Slippi stamps 1/2 in ftCo_LandingAir_EnterWithLag's L-cancel compare
     * (x67F against the window) and clears it every frame: the frame a
     * fighter enters an aerial's lagged landing. */
    if (landing_air((uint16_t)fp->motion_id) && !landing_air(pre->action)) {
        po->l_cancel = fp->x67F < p_ftCommonData->xE4 ? 1 : 2;
    }
    /* Move-induced collision state wins over the game-induced one. */
    po->hurtbox = (uint8_t)(fp->x1988 != 0 ? fp->x1988 : fp->x198C); /* fp+1988, +198C */
    po->self_air_x = fp->self_vel.x;                                 /* fp+80 */
    po->self_y = fp->self_vel.y;                                     /* fp+84 */
    po->attack_x = fp->x8c_kb_vel.x;                                 /* fp+8C */
    po->attack_y = fp->x8c_kb_vel.y;                                 /* fp+90 */
    po->self_ground_x = fp->gr_vel;                                  /* fp+EC */
    po->hitlag = fp->dmg.x195c_hitlag_frames;                        /* fp+195C */
    po->animation = (uint32_t)fp->anim_id;                           /* fp+14 */
    po->instance_hit_by = fp->dmg.x18ec_instancehitby;               /* fp+18EC */
    po->instance_id = fp->x2074.x2088;                               /* fp+2088 */
    s->nf++;
}

static void capture_items(Staged* s) {
    s->ni = 0;
    for (HSD_GObj* g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_ITEM]; g != NULL; g = g->next) {
        if (s->ni >= SLP_MAX_ITEMS) {
            break;
        }
        const Item* ip = g->user_data;
        if (ip == NULL) {
            continue;
        }
        SlpItem* o = &s->items[s->ni++];
        memset(o, 0, sizeof *o);
        o->type = (uint16_t)ip->kind;    /* ip+10 */
        o->state = (uint8_t)ip->msid;    /* ip+24 */
        o->facing = ip->facing_dir;      /* ip+2C */
        o->vel_x = ip->x40_vel.x;        /* ip+40 */
        o->vel_y = ip->x40_vel.y;        /* ip+44 */
        o->x = ip->pos.x;                /* ip+4C */
        o->y = ip->pos.y;                /* ip+50 */
        o->damage = (uint16_t)ip->xC9C;  /* ip+C9C, low half */
        o->expire = ip->xD44_lifeTimer;  /* ip+D44 */
        o->spawn_id = (uint32_t)ip->x1C; /* ip+1C */
        /* ip+DD7/DDB/DEB/DEF: the low bytes of item-variable words. Filled for
         * the kinds SPEC.md documents them for, zero for the rest. */
        switch (ip->kind) {
        case It_Kind_Samus_Missile:
            o->misc[0] = (uint8_t)ip->xDD4_itemVar.samusmissile.is_smash_missile;
            break;
        case It_Kind_Peach_Turnip:
            o->misc[1] = (uint8_t)ip->xDD4_itemVar.peachturnip.xDD8;
            break;
        case It_Kind_Samus_Charge:
            o->misc[2] = (uint8_t)ip->xDD4_itemVar.samuschargeshot.xDE8;
            o->misc[3] = (uint8_t)ip->xDD4_itemVar.samuschargeshot.xDEC;
            break;
        case It_Kind_Mewtwo_ShadowBall:
            o->misc[2] = (uint8_t)ip->xDD4_itemVar.mewtwoshadowball.x14;
            o->misc[3] = (uint8_t)ip->xDD4_itemVar.mewtwoshadowball.x18;
            break;
        default:
            break;
        }
        o->owner = -1; /* ip+518 -> owner's fighter -> player slot (fp+C) */
        HSD_GObj* owner = ip->owner;
        if (owner != NULL && owner->classifier == HSD_GOBJ_CLASS_FIGHTER &&
            owner->user_data != NULL)
        {
            o->owner = (int8_t)GET_FIGHTER(owner)->player_id;
        }
        o->instance_id = ip->xDA8_short; /* ip+DA8 */
    }
}

/* Fountain of Dreams platforms (map 4, grIzumi_801CC358), Dream Land's
 * Whispy (map 7, grOldPupupu_802113E0) and Pokemon Stadium's
 * transformation (map 2, grStadium_801D4548), sampled after the tick. */
static void capture_stage(Staged* s) {
    s->nfod = 0;
    s->have_whispy = s->have_ps = false;
    if (s_stage != St_Kind_Izumi && s_stage != St_Kind_OldPupupu && s_stage != St_Kind_PStadium) {
        return;
    }
    for (HSD_GObj* g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_GROUND]; g != NULL; g = g->next) {
        if (g->classifier != HSD_GOBJ_CLASS_STAGE || g->user_data == NULL) {
            continue;
        }
        const Ground* gp = g->user_data;
        if (gp->gobj != g) {
            continue;
        }
        if (s_stage == St_Kind_Izumi && gp->map_id == 4 && s->nfod < 2) {
            HSD_JObj* jobj = gp->u.izumi3.xCC; /* gp+CC, the platform's own joint */
            /* SendFountainInfo: the sign of its translate.x picks the side */
            s->fod_side[s->nfod] = jobj != NULL && HSD_JObjGetTranslationX(jobj) < 0.0F ? 1 : 0;
            s->fod_height[s->nfod] = gp->u.izumi3.xD0; /* gp+D0 */
            s->nfod++;
        } else if (s_stage == St_Kind_OldPupupu && gp->map_id == 7) {
            s->have_whispy = true;
            s->whispy = (uint8_t)gp->u.oldpupupu.xDC; /* gp+DC */
        } else if (s_stage == St_Kind_PStadium && gp->map_id == 2) {
            s->have_ps = true;
            s->ps_event = (uint16_t)gp->u.stadium.xDC; /* gp+DC */
            s->ps_type = (uint16_t)gp->u.stadium.xDE;  /* gp+DE */
        }
    }
}

/* SendGameEnd.asm: the outcome, the LRA+Start player and each port's
 * placement, MatchPlayerData.is_small_loser as gm_80166378 computes it for
 * the results screen (worked here on a copy, as Slippi does). */
static void capture_end(Staged* s, const VsSceneController* vs) {
    static MatchEnd scratch;
    SlpGameEnd* e = &s->game_end;
    e->method = vs->state.match_result;
    e->lras = vs->state.match_result == OUTCOME_NO_CONTEST ? vs->state.pauser : -1;
    for (int i = 0; i < 4; i++) {
        e->placements[i] = -1;
    }
    /* A bonus match's scoring calls pl_80039450, which is not read-only. */
    if (vs->start.match_kind == 3) {
        return;
    }
    scratch = vs->state.x24C;
    scratch.is_teams = vs->start.is_teams;
    scratch.outcome = vs->state.match_result;
    gm_80166378(&scratch);
    for (int i = 0; i < 4; i++) {
        const struct MatchPlayerData* p = &scratch.player_standings[i];
        e->placements[i] = p->pkind == Gm_PKind_NA ? -1 : (int8_t)p->is_small_loser;
    }
}

void pc_slp_tick_end(uint64_t proc_mask) {
    if (!s_begin.on || !s_rec || !slp_scene()) {
        return;
    }
    const int32_t f = s_begin.net_frame;
    if (s_have && f <= s_emitted) {
        return; /* a frame already final: only the resim audit re-runs one */
    }
    Staged* s = &s_ring[f & (SLP_RING - 1)];
    if (s_have && s->net_frame != NO_FRAME && s->net_frame != f && s->net_frame > s_emitted) {
        if (s_lost++ == 0) {
            pc_log_line("slp: tick %d dropped from the ring unconfirmed; the replay has a gap",
                s->net_frame);
        }
    }
    const VsSceneController* vs = gmVs_GetSceneController();
    /* The tick before this one is staged (a rollback re-runs from the frame
     * it restores, so everything behind it is the timeline that stands). */
    const Staged* prev = &s_ring[(f - 1) & (SLP_RING - 1)];
    const bool have_prev = s_have && prev->net_frame == f - 1;
    s->net_frame = f;
    /* The fighters' procs ran unless the p_link mask the tick used held them
     * (pause, the GAME! freeze, a slow-motion skip). */
    s->frame = !(proc_mask & (1ULL << HSD_GOBJ_PLINK_FIGHTER));
    s->result = vs->state.match_result;
    s->nf = 0;
    s->ni = 0;
    s->vs_frame_count = vs->state.frame_count;
    if (s->frame) {
        s->start.seed = s_begin.seed;
        s->start.scene_frame = s_begin.scene_frame;
        for (int i = 0; i < SLP_MAX_FIGHTERS; i++) {
            HSD_GObj* g = fighter_gobj(i / 2, i & 1);
            /* fp+221F 0x10: asleep (Sheik/Zelda's other half, a Nana waiting
             * for Popo, a player out of stocks) sends nothing */
            if (g != NULL && !GET_FIGHTER(g)->x221F_b3) {
                capture_fighter(s, i, g);
            }
        }
        capture_items(s);
        capture_stage(s);
    }
    if (s->result != OUTCOME_NONE) {
        if (have_prev && prev->result != OUTCOME_NONE) {
            s->game_end = prev->game_end; /* stocks are frozen from GAME! on */
        } else {
            capture_end(s, vs);
        }
    }
    if (!s_have) {
        s_have = true;
        s_emitted = f - 1;
        s_staged_max = f;
    } else if (f > s_staged_max) {
        s_staged_max = f;
    }
}

/* ---- serialization, in frame order, once final ------------------------ */

static void emit(const Staged* s) {
    uint8_t buf[4096];
    size_t n = 0;
    if (s->frame && !s_ended) {
        const int32_t fr = ++s_frame;
        SlpFrameStart fs = s->start;
        fs.frame = fr;
        n += slp_frame_start(buf + n, &fs);
        for (int i = 0; i < s->nf; i++) {
            SlpPreFrame pre = s->pre[i];
            pre.frame = fr;
            n += slp_pre_frame(buf + n, &pre);
        }
        /* SendFountainInfo sends a height only when the platform moves:
         * the heights the match starts with are no event. */
        for (int i = 0; i < s->nfod; i++) {
            const uint8_t side = s->fod_side[i];
            if (fr != SLP_FIRST_FRAME && s_fod_last[side] != s->fod_height[i]) {
                n += slp_fod_platform(buf + n, fr, side, s->fod_height[i]);
            }
            s_fod_last[side] = s->fod_height[i];
        }
        if (s->have_whispy && s->whispy != s_whispy_last) {
            s_whispy_last = s->whispy;
            n += slp_whispy(buf + n, fr, s->whispy);
        }
        if (s->have_ps && (s->ps_event != s_ps_last_event || s->ps_type != s_ps_last_type)) {
            s_ps_last_event = s->ps_event;
            s_ps_last_type = s->ps_type;
            n += slp_stadium(buf + n, fr, s->ps_event, s->ps_type);
        }
        for (int i = 0; i < s->ni; i++) {
            SlpItem it = s->items[i];
            it.frame = fr;
            n += slp_item(buf + n, &it);
        }
        for (int i = 0; i < s->nf; i++) {
            SlpPostFrame po = s->post[i];
            po.frame = fr;
            n += slp_post_frame(buf + n, &po);
            s_usage[po.port & 3][po.character & 63]++;
        }
        n += slp_frame_bookend(buf + n, fr, fr);
        memcpy(s_last_post, s->post, sizeof s_last_post);
        s_last_nf = s->nf;
        if (!s_timer_logged && s->vs_frame_count != 0) {
            s_timer_logged = true;
            pc_log_line("slp: match timer started on replay frame %d (tick %d)", fr, s->net_frame);
        }
        if (fr == SLP_FIRST_FRAME) {
            pc_log_line("slp: replay frame %d is tick %d", fr, s->net_frame);
        }
    }
    if (s->result != OUTCOME_NONE && !s_ended) {
        s_ended = true;
        n += slp_game_end(buf + n, &s->game_end);
        pc_log_line("slp: game end on replay frame %d: method %u, lras %d, placements %d %d %d %d",
            s_frame, s->game_end.method, s->game_end.lras, s->game_end.placements[0],
            s->game_end.placements[1], s->game_end.placements[2], s->game_end.placements[3]);
    }
    slp_writer_append(buf, n);
}

/* Frames up to `upto` are final: serialize them, in order. */
static void confirm(int32_t upto) {
    if (!s_rec || !s_have) {
        return;
    }
    if (upto > s_staged_max) {
        upto = s_staged_max;
    }
    for (int32_t f = s_emitted + 1; f <= upto; f++) {
        const Staged* s = &s_ring[f & (SLP_RING - 1)];
        if (s->net_frame == f) {
            emit(s);
        } else if (s_lost++ == 0) {
            pc_log_line("slp: tick %d missing when confirmed; the replay has a gap", f);
        }
        s_emitted = f;
    }
}

static void finish(const char* why) {
    if (!s_rec) {
        return;
    }
    /* Offline every staged tick is final once the scene is over. Netplay
     * writes only what the peer confirmed: a session cut short (a disconnect,
     * the harness's exit) leaves its predicted tail out rather than in. */
    if (s_have && !s_net) {
        confirm(s_staged_max);
    }
    const int32_t unconfirmed = s_have ? s_staged_max - s_emitted : 0;
    SlpMeta* m = calloc(1, sizeof *m);
    uint8_t* meta = malloc(4096);
    if (m != NULL && meta != NULL) {
        snprintf(m->start_at, sizeof m->start_at, "%s", s_start_at);
        m->last_frame = s_frame < SLP_FIRST_FRAME ? SLP_FIRST_FRAME : s_frame;
        for (int port = 0; port < 4; port++) {
            for (int id = 0; id < 64; id++) {
                m->players[port].frames[id] = s_usage[port][id];
                m->players[port].present |= s_usage[port][id] != 0;
            }
        }
        m->played_on = "melee-pc";
        const size_t n = slp_metadata(meta, 4096, m);
        slp_writer_end(meta, n);
    } else {
        slp_writer_end(NULL, 0);
    }
    free(m);
    free(meta);
    for (int i = 0; i < s_last_nf; i++) {
        const SlpPostFrame* p = &s_last_post[i];
        pc_log_line("slp: last frame %d port %u%s: char %u action %u stocks %u percent %.2f "
                    "pos (%.4f, %.4f)",
            s_frame, p->port + 1, p->follower ? " (follower)" : "", p->character, p->action,
            p->stocks, p->percent, p->x, p->y);
    }
    pc_log_line("slp: match closed (%s): frames %d..%d, %s, %d ticks left unconfirmed, %u lost",
        why, SLP_FIRST_FRAME, s_frame, s_ended ? "game end written" : "no game end",
        (int)unconfirmed, s_lost);
    s_rec = false;
    s_have = false;
}

void pc_slp_match_end(void) {
    finish("match scene left");
}
