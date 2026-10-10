/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Map-pack registry: new stage-select slots built on an existing stage.
 *
 * A map pack runs as its base stage -- the same stage code, StKind and
 * GrKind, so every per-stage behaviour works -- but loads its own stage file
 * (the base's Gr*.dat layout, including a grGroundParam entry for the base's
 * StKind). The pack travels in StartMeleeRules::pc_stage_pack and is made
 * active around the two places the stage file is opened (ground.c). */
#ifndef PC_MODS_STAGES_H
#define PC_MODS_STAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcStagePackDesc {
    const char* id;   /* "<mod id>/<stage id>" */
    const char* name; /* display name */
    const char* mod_id;
    int base_stkind;             /* StKind whose code the map runs with */
    const char* file;            /* stage file on the virtual disc ("/GrXx.dat") */
    const char* icon_path;       /* stage-select icon PNG, or NULL (base's) */
    const char* preview_path;    /* stage-select preview PNG, or NULL */
    const char* name_image_path; /* stage-select name plate PNG, or NULL */
    const char* music_path;      /* .ogg/.wav replacing the stage music, or NULL */
} PcStagePackDesc;

/* Returns the new map-pack index (pc_stage_pack = index + 1) or -1 with a
 * reason. Strings are copied. */
int pc_stages_add(const PcStagePackDesc* desc, char* err, size_t err_size);
int pc_stages_count(void);
const char* pc_stages_id(int pack);
const char* pc_stages_name(int pack);
int pc_stages_base(int pack); /* StKind, or -1 */
const char* pc_stages_music(int pack);
const void* pc_stages_icon_gx(int pack, int w, int h);
const void* pc_stages_preview_gx(int pack, int w, int h);
const void* pc_stages_name_gx(int pack, int w, int h);

/* "Battlefield", "final destination", "PStadium" ... -> StKind of a stage
 * on the VS stage select, or -1. */
int pc_stage_from_name(const char* name);
const char* pc_stage_display_name(int stkind);

/* The map pack whose file the next stage load uses (-1 none). Callers set
 * it right before a load and clear it right after. */
void pc_stages_set_active(int pack);
/* ground.c: the active pack's stage file when it is built on @p grkind,
 * else NULL. */
const char* pc_stages_file_for(int grkind);

uint32_t pc_stages_hash(uint32_t h);

#ifdef __cplusplus
}
#endif

#endif
