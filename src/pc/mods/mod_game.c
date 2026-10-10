/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The half of the mod loader that touches game structures. ftCo_DatAttrs is a
 * DISC_STRUCT (big-endian storage), so fields are only ever read and written
 * through the struct -- never by byte offset -- and GCC does the swapping. */
#include "pc/compat.h"

#include "mod_internal.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include <melee/ft/fighter.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/pl/player.h>

/* Every named scalar in ftCo_DatAttrs. Unknown (xNN) fields and vectors are
 * left out on purpose: a name is a promise about what the value means. */
#define PC_MOD_ATTRS(F, I)                                                                         \
    F(walk_accel_mul)                                                                              \
    F(walk_accel_base)                                                                             \
    F(walk_max_vel)                                                                                \
    F(slow_walk_max)                                                                               \
    F(mid_walk_point)                                                                              \
    F(fast_walk_min)                                                                               \
    F(ground_friction)                                                                             \
    F(dash_initial_velocity)                                                                       \
    F(dash_accel_mul)                                                                              \
    F(dash_accel_base)                                                                             \
    F(dash_max_velocity)                                                                           \
    F(run_animation_scaling)                                                                       \
    F(max_run_brake_frames)                                                                        \
    F(ground_max_horizontal_velocity)                                                              \
    F(jump_startup_time)                                                                           \
    F(jump_h_initial_velocity)                                                                     \
    F(jump_v_initial_velocity)                                                                     \
    F(ground_to_air_jump_momentum_multiplier)                                                      \
    F(jump_h_max_velocity)                                                                         \
    F(hop_v_initial_velocity)                                                                      \
    F(air_jump_v_multiplier)                                                                       \
    F(air_jump_h_multiplier)                                                                       \
    I(max_jumps)                                                                                   \
    F(gravity)                                                                                     \
    F(terminal_velocity)                                                                           \
    F(air_drift_stick_mul)                                                                         \
    F(aerial_drift_base)                                                                           \
    F(air_drift_max)                                                                               \
    F(aerial_friction)                                                                             \
    F(fast_fall_velocity)                                                                          \
    F(air_max_horizontal_velocity)                                                                 \
    F(jab_2_input_window)                                                                          \
    F(jab_3_input_window)                                                                          \
    F(standing_turn_frames)                                                                        \
    F(weight)                                                                                      \
    F(model_scaling)                                                                               \
    F(initial_shield_size)                                                                         \
    F(shield_break_initial_velocity)                                                               \
    I(rapid_jab_window)                                                                            \
    F(clank_animation_length)                                                                      \
    I(hit_spark_variant)                                                                           \
    F(ledge_jump_horizontal_velocity)                                                              \
    F(ledge_jump_vertical_velocity)                                                                \
    F(item_throw_velocity_multiplier)                                                              \
    F(heavy_throw_velocity_multiplier)                                                             \
    F(specials_ground_speed_retention)                                                             \
    F(kirby_b_star_damage)                                                                         \
    F(normal_landing_lag)                                                                          \
    F(landingairn_lag)                                                                             \
    F(landingairf_lag)                                                                             \
    F(landingairb_lag)                                                                             \
    F(landingairhi_lag)                                                                            \
    F(landingairlw_lag)                                                                            \
    F(name_tag_height)                                                                             \
    F(passivewall_vel_x)                                                                           \
    F(wall_jump_horizontal_velocity)                                                               \
    F(wall_jump_vertical_velocity)                                                                 \
    F(passiveceil_vel_x)                                                                           \
    F(trophy_scale)                                                                                \
    F(screw_attack_launch_velocity)                                                                \
    F(wall_jump_min_approach_speed)                                                                \
    F(damageice_ice_size)                                                                          \
    F(damageicejump_vel_y)                                                                         \
    F(damageicejump_vel_x_mult)                                                                    \
    F(respawn_platform_scale)                                                                      \
    F(warp_star_hitbox_scale)                                                                      \
    I(camera_zoom_target_bone)

enum {
#define X(n) ATTR_##n,
    PC_MOD_ATTRS(X, X)
#undef X
        ATTR_COUNT
};

static const char* const s_attr_names[ATTR_COUNT] = {
#define X(n) #n,
    PC_MOD_ATTRS(X, X)
#undef X
};

static const bool s_attr_int[ATTR_COUNT] = {
#define XF(n) false,
#define XI(n) true,
    PC_MOD_ATTRS(XF, XI)
#undef XF
#undef XI
};

int pc_mod_attr_count(void) {
    return ATTR_COUNT;
}

const char* pc_mod_attr_name(int index) {
    return index >= 0 && index < ATTR_COUNT ? s_attr_names[index] : NULL;
}

bool pc_mod_attr_is_int(int index) {
    return index >= 0 && index < ATTR_COUNT && s_attr_int[index];
}

int pc_mod_attr_index(const char* name) {
    if (name == NULL)
        return -1;
    for (int i = 0; i < ATTR_COUNT; ++i)
        if (strcmp(s_attr_names[i], name) == 0)
            return i;
    return -1;
}

float pc_mod_attr_get(const void* co_attrs, int index) {
    const ftCo_DatAttrs* a = (const ftCo_DatAttrs*)co_attrs;
    switch (index) {
#define X(n)                                                                                       \
    case ATTR_##n:                                                                                 \
        return (float)a->n;
        PC_MOD_ATTRS(X, X)
#undef X
    default:
        return 0.0f;
    }
}

void pc_mod_attr_set(void* co_attrs, int index, float value) {
    ftCo_DatAttrs* a = (ftCo_DatAttrs*)co_attrs;
    switch (index) {
#define XF(n)                                                                                      \
    case ATTR_##n:                                                                                 \
        a->n = value;                                                                              \
        break;
#define XI(n)                                                                                      \
    case ATTR_##n:                                                                                 \
        a->n = (int)lroundf(value);                                                                \
        break;
        PC_MOD_ATTRS(XF, XI)
#undef XF
#undef XI
    default:
        break;
    }
}

/* ---- fighter names ------------------------------------------------------ */

static const struct {
    const char* name; /* compared after folding: lowercase, alnum only */
    int kind;
} s_fighter_names[] = {
    {"mario", Ft_Kind_Mario},
    {"fox", Ft_Kind_Fox},
    {"captainfalcon", Ft_Kind_Captain},
    {"captain", Ft_Kind_Captain},
    {"falcon", Ft_Kind_Captain},
    {"donkeykong", Ft_Kind_Donkey},
    {"donkey", Ft_Kind_Donkey},
    {"dk", Ft_Kind_Donkey},
    {"kirby", Ft_Kind_Kirby},
    {"bowser", Ft_Kind_Koopa},
    {"koopa", Ft_Kind_Koopa},
    {"link", Ft_Kind_Link},
    {"sheik", Ft_Kind_Seak},
    {"seak", Ft_Kind_Seak},
    {"ness", Ft_Kind_Ness},
    {"peach", Ft_Kind_Peach},
    {"popo", Ft_Kind_Popo},
    {"nana", Ft_Kind_Nana},
    {"pikachu", Ft_Kind_Pikachu},
    {"samus", Ft_Kind_Samus},
    {"yoshi", Ft_Kind_Yoshi},
    {"jigglypuff", Ft_Kind_Purin},
    {"purin", Ft_Kind_Purin},
    {"puff", Ft_Kind_Purin},
    {"mewtwo", Ft_Kind_Mewtwo},
    {"luigi", Ft_Kind_Luigi},
    {"marth", Ft_Kind_Mars},
    {"mars", Ft_Kind_Mars},
    {"zelda", Ft_Kind_Zelda},
    {"younglink", Ft_Kind_CLink},
    {"clink", Ft_Kind_CLink},
    {"drmario", Ft_Kind_DrMario},
    {"doc", Ft_Kind_DrMario},
    {"falco", Ft_Kind_Falco},
    {"pichu", Ft_Kind_Pichu},
    {"mrgameandwatch", Ft_Kind_GameWatch},
    {"gameandwatch", Ft_Kind_GameWatch},
    {"gamewatch", Ft_Kind_GameWatch},
    {"gnw", Ft_Kind_GameWatch},
    {"ganondorf", Ft_Kind_Ganon},
    {"ganon", Ft_Kind_Ganon},
    {"roy", Ft_Kind_Emblem},
    {"emblem", Ft_Kind_Emblem},
    {"masterhand", Ft_Kind_MasterH},
    {"crazyhand", Ft_Kind_CrezyH},
    {"malewireframe", Ft_Kind_Boy},
    {"boy", Ft_Kind_Boy},
    {"femalewireframe", Ft_Kind_Girl},
    {"girl", Ft_Kind_Girl},
    {"gigabowser", Ft_Kind_GKoops},
    {"sandbag", Ft_Kind_Sandbag},
};

static const char* const s_display_names[PC_MOD_FIGHTER_KINDS] = {"Mario", "Fox", "Captain Falcon",
    "Donkey Kong", "Kirby", "Bowser", "Link", "Sheik", "Ness", "Peach", "Popo", "Nana", "Pikachu",
    "Samus", "Yoshi", "Jigglypuff", "Mewtwo", "Luigi", "Marth", "Zelda", "Young Link", "Dr. Mario",
    "Falco", "Pichu", "Mr. Game & Watch", "Ganondorf", "Roy", "Master Hand", "Crazy Hand",
    "Male Wireframe", "Female Wireframe", "Giga Bowser", "Sandbag"};

_Static_assert(Ft_Kind_Max == PC_MOD_FIGHTER_KINDS, "FighterKind count changed");

int pc_mod_fighter_from_name(const char* name, int* also) {
    char folded[64];
    size_t n = 0;
    if (also)
        *also = -1;
    if (name == NULL)
        return PC_MOD_FIGHTER_UNKNOWN;
    if (strcmp(name, "*") == 0)
        return MELEE_FIGHTER_ALL;
    for (const char* c = name; *c && n < sizeof(folded) - 1; ++c)
        if (isalnum((unsigned char)*c))
            folded[n++] = (char)tolower((unsigned char)*c);
    folded[n] = '\0';
    if (strcmp(folded, "all") == 0)
        return MELEE_FIGHTER_ALL;
    if (strcmp(folded, "iceclimbers") == 0 || strcmp(folded, "ics") == 0) {
        if (also)
            *also = Ft_Kind_Nana;
        return Ft_Kind_Popo;
    }
    for (size_t i = 0; i < sizeof(s_fighter_names) / sizeof(s_fighter_names[0]); ++i)
        if (strcmp(s_fighter_names[i].name, folded) == 0)
            return s_fighter_names[i].kind;
    return PC_MOD_FIGHTER_UNKNOWN;
}

const char* pc_mod_fighter_name(int kind) {
    return kind >= 0 && kind < PC_MOD_FIGHTER_KINDS ? s_display_names[kind] : "all fighters";
}

/* ---- live state --------------------------------------------------------- */

static Fighter* port_fighter(int port) {
    if (port < 0 || port >= PC_MOD_MAX_PORTS)
        return NULL;
    if (Player_GetPlayerSlotType(port) == Gm_PKind_NA)
        return NULL;
    HSD_GObj* gobj = Player_GetEntity(port);
    if (gobj == NULL)
        return NULL;
    Fighter* fp = GET_FIGHTER(gobj);
    if (fp == NULL || fp->gobj != gobj || (unsigned)fp->kind >= PC_MOD_FIGHTER_KINDS)
        return NULL;
    return fp;
}

bool pc_mod_game_get_player(int port, MeleePlayerState* out) {
    if (out == NULL)
        return false;
    memset(out, 0, sizeof(*out));
    Fighter* fp = port_fighter(port);
    if (fp == NULL)
        return false;
    out->present = true;
    out->fighter_kind = fp->kind;
    out->costume = fp->costume_id;
    out->stocks = Player_GetStocks(port);
    out->percent = fp->dmg.x1830_percent;
    out->pos_x = fp->cur_pos.x;
    out->pos_y = fp->cur_pos.y;
    out->vel_x = fp->self_vel.x;
    out->vel_y = fp->self_vel.y;
    out->facing = fp->facing_dir < 0 ? -1 : 1;
    out->action_state = (uint32_t)fp->motion_id;
    return true;
}

void* pc_mod_game_player_attrs(int port) {
    Fighter* fp = port_fighter(port);
    /* Opaque to the loader; only pc_mod_attr_get/set dereference it. */
    return fp ? (void*)(uintptr_t)&fp->co_attrs : NULL;
}

bool pc_mod_game_set_percent(int port, float percent) {
    Fighter* fp = port_fighter(port);
    if (fp == NULL)
        return false;
    if (percent < 0)
        percent = 0;
    if (percent > 999)
        percent = 999;
    ftLib_SetPercent(fp->gobj, (s32)lroundf(percent));
    return true;
}
