/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bridge between the loader (mods.c, which knows nothing about game structs)
 * and mod_game.c (which includes the game headers). */
#ifndef PC_MODS_INTERNAL_H
#define PC_MODS_INTERNAL_H

#include "melee_mod.h"

#include <stdbool.h>

#define PC_MOD_FIGHTER_UNKNOWN (-2)
#define PC_MOD_FIGHTER_KINDS 33 /* FighterKind count: Mario .. Sandbag */
#define PC_MOD_MAX_PORTS 6
#define PC_MOD_PACK_FIRST 34 /* Ft_Kind_PackFirst */

/* ftCo_DatAttrs field table. */
int pc_mod_attr_count(void);
const char* pc_mod_attr_name(int index);
int pc_mod_attr_index(const char* name); /* -1 when unknown */
bool pc_mod_attr_is_int(int index);
float pc_mod_attr_get(const void* co_attrs, int index);
void pc_mod_attr_set(void* co_attrs, int index, float value);

/* "Fox", "captain falcon", "ice_climbers", "*" ... -> FighterKind,
 * MELEE_FIGHTER_ALL, or PC_MOD_FIGHTER_UNKNOWN. Ice Climbers resolves to
 * Popo and also sets *also to Nana; *also is -1 otherwise. */
int pc_mod_fighter_from_name(const char* name, int* also);
const char* pc_mod_fighter_name(int kind);

/* ASCII -> full-width Shift-JIS as the game's name text expects (letters,
 * digits and common punctuation; anything else becomes a space). The caller
 * frees the result. */
char* pc_mod_ascii_to_sjis(const char* ascii);

/* Live state; only meaningful while a fight scene is running. */
bool pc_mod_game_get_player(int port, MeleePlayerState* out);
void* pc_mod_game_player_attrs(int port); /* the live ftCo_DatAttrs, or NULL */
bool pc_mod_game_set_percent(int port, float percent);

#endif
