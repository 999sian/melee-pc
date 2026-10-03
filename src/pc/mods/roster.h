/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Character-pack registry: turns a mod's "fighters" entries into new fighter
 * asset kinds (Ft_Kind_PackFirst + n). See src/melee/ft/forward.h for the
 * asset-kind model and docs/modding.md for the manifest format. */
#ifndef PC_MODS_ROSTER_H
#define PC_MODS_ROSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_ROSTER_MAX_COSTUMES 16

typedef struct PcRosterCostume {
    const char* file;          /* costume model file on the virtual disc */
    const char* joint;         /* NULL: the base's symbol for this costume slot */
    const char* matanim;       /* NULL: the base's; "" for none */
    const char* portrait_path; /* host path of the CSS portrait PNG, or NULL */
    const char* stock_path;    /* host path of the HUD stock icon PNG, or NULL */
} PcRosterCostume;

typedef struct PcRosterFighter PcRosterFighter;
struct PcRosterFighter {
    const char* id;          /* "<mod id>/<fighter id>", unique */
    const char* name;        /* display name */
    int base_kind;           /* FighterKind whose moveset/behaviour it uses */
    const char* data_file;   /* Pl*.dat equivalent */
    const char* data_symbol; /* NULL: the base's (e.g. "ftDataFox") */
    const char* anim_file;   /* Pl*AJ.dat equivalent */
    size_t costume_count;
    const PcRosterCostume* costumes;
    const char* mod_id;             /* owning mod, for messages */
    const char* icon_path;          /* host path of a CSS icon image (PNG), or NULL */
    const char* announcer_path;     /* host path of the name call clip, or NULL */
    const char* victory_theme_path; /* host path of the victory theme, or NULL */
    /* The second fighter of an Ice Climbers (Nana) or Zelda/Sheik (the
     * transformation) pack: data, animations and costumes; id, name and
     * base are derived. Required for those bases, ignored otherwise. */
    const PcRosterFighter* partner;
};

/* Returns the new pack index (0-based; pc_pack = index + 1, asset kind =
 * Ft_Kind_PackFirst + index) or -1 with a reason in err. Strings are copied. */
int pc_roster_add_fighter(const PcRosterFighter* desc, char* err, size_t err_size);

/* Every registered entry, partners included (they are asset kinds too). */
int pc_roster_fighter_count(void);
/* The packs the character select lists (partners are not): count, and the
 * pack index of the n-th one. */
int pc_roster_visible_count(void);
int pc_roster_visible_pack(int n);
/* The partner pack of @p pack, or -1. */
int pc_roster_partner(int pack);
/* Host paths of a pack's announcer clip and victory theme, or NULL. */
const char* pc_roster_announcer_path(int pack);
const char* pc_roster_victory_theme_path(int pack);
/* -1 when no pack has that id. */
int pc_roster_find_fighter(const char* id);
const char* pc_roster_fighter_id(int pack);
const char* pc_roster_fighter_name(int pack);
const char* pc_roster_fighter_mod(int pack);
int pc_roster_fighter_base(int pack);
int pc_roster_fighter_costumes(int pack);
/* Asset kind of a pack, or -1. */
int pc_roster_asset_kind(int pack);

/* The pack's CSS icon as GX RGBA8 texture data (GX_TF_RGBA8, 4x4 tiles),
 * resampled to w x h (both multiples of 4). Decoded once and cached; NULL
 * when the pack has no icon or it cannot be read. The buffer is
 * w * h * 4 bytes and owned by the registry. */
const void* pc_roster_icon_gx(int pack, int w, int h);

/* The CSS door portrait / HUD stock icon of one costume, as GX RGBA8 data
 * resampled to w x h; NULL when the pack ships none for that costume. */
const void* pc_roster_portrait_gx(int pack, int costume, int w, int h);
const void* pc_roster_stock_gx(int pack, int costume, int w, int h);

/* The display name converted to the game's full-width Shift-JIS text, as the
 * CSS name plate expects. Owned by the registry. */
const char* pc_roster_fighter_sjis_name(int pack);

/* Why a fighter cannot be a pack base, or NULL when it can. */
const char* pc_roster_base_unsupported(int base_kind);

/* FNV-1a contribution of the registered packs (ids, bases, files), for the
 * netplay gameplay hash. */
uint32_t pc_roster_hash(uint32_t h);

#ifdef __cplusplus
}
#endif

#endif
