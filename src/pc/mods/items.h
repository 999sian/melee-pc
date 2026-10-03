/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Item-pack registry: new items built on an existing common item.
 *
 * An item pack runs as its base item -- the same ItemKind and item code, so
 * pickup, throw, swing, shoot and break all behave like the base -- but takes
 * its model, attributes and hitboxes from its own Article, loaded from the
 * mod's .dat (the base's Article layout, as ItCo.dat holds it). Packs join
 * the random item draw next to their base (it_8026D018), so they spawn
 * wherever and whenever the base could. */
#ifndef PC_MODS_ITEMS_H
#define PC_MODS_ITEMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcItemPackDesc {
    const char* id;   /* "<mod id>/<item id>" */
    const char* name; /* display name (log, pack lists) */
    const char* mod_id;
    int base_kind;      /* ItemKind of the common item it runs as */
    const char* file;   /* .dat on the virtual disc */
    const char* symbol; /* public symbol of the Article in it */
    float frequency;    /* spawn weight relative to the base (1 = as often) */
    /* Character, projectile and stage item bases only (pc_item_base_class
     * PC_ITEM_OWNED): spawns of the base become the pack only for this owner
     * and/or on this stage; -1 = any. */
    int owner_pack; /* character pack index (its fighters) */
    int owner_kind; /* FighterKind, vanilla or a pack's base */
    int stage_pack; /* map pack index */
    int stage_kind; /* StKind */
} PcItemPackDesc;

typedef enum PcItemBaseClass {
    PC_ITEM_NONE,    /* not a base */
    PC_ITEM_COMMON,  /* a common item: joins the random item draw */
    PC_ITEM_POKEMON, /* a Poke Ball Pokemon: joins the Poke Ball draw */
    PC_ITEM_OWNED,   /* spawned by a fighter, item, Pokemon or stage (Link's
                      * bomb, Fox's laser, a Goomba): replaces spawns of it */
} PcItemBaseClass;

/* Returns the new item-pack index or -1 with a reason. Strings are copied. */
int pc_items_add(const PcItemPackDesc* desc, char* err, size_t err_size);
int pc_items_count(void);
const char* pc_items_id(int pack);
const char* pc_items_name(int pack);
int pc_items_base(int pack); /* ItemKind, or -1 */
const char* pc_items_file(int pack);
const char* pc_items_symbol(int pack);
float pc_items_frequency(int pack);
int pc_items_owner_pack(int pack);
int pc_items_owner_kind(int pack);
int pc_items_stage_pack(int pack);
int pc_items_stage_kind(int pack);
PcItemBaseClass pc_item_base_class(int kind);

/* Base item by English or internal name ("Beam Sword", "Sword", "Snorlax",
 * "Kabigon", "Link_Bomb", "Old Kuri"), or -1: a common item other than the
 * Poke Ball (which spawns Pokemon, not itself), a Poke Ball Pokemon other
 * than Mew and Celebi (which only come from their own rare roll), or a
 * character, projectile or stage item by its ItemKind name (forward.h). */
int pc_item_from_name(const char* name);
const char* pc_item_display_name(int kind);

/* Name in the game's full-width text encoding (Training's item menu). */
const char* pc_items_sjis_name(int pack);
/* Whether `kind` is a Pokemon (spawned from Poke Balls) rather than a common
 * item. */
bool pc_item_kind_is_pokemon(int kind);

uint32_t pc_items_hash(uint32_t h);

#ifdef __cplusplus
}
#endif

#endif
