/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Mod loader: discovery, load order, virtual-disc overlays, tunables, native
 * plugins and the hook dispatch the game calls into. The public plugin ABI is
 * melee_mod.h; this header is the game/launcher side. docs/modding.md is the
 * modder-facing description. */
#ifndef PC_MODS_H
#define PC_MODS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcModInfo {
    const char* id;
    const char* name;
    const char* version;
    const char* author;
    const char* description;
    const char* dir;
    bool enabled;
    bool active; /* enabled and successfully activated this session */
    bool has_plugin;
    bool affects_gameplay;
    int priority;
    size_t file_count;    /* files under the mod's files/ folder */
    size_t tunable_count; /* fighter attribute patches from the manifest */
    size_t fighter_count; /* characters ("fighters") the manifest declares */
    size_t stage_count;   /* map packs ("stages") the manifest declares */
    size_t item_count;    /* item packs ("items") the manifest declares */
    const char* error;    /* NULL, or why the mod cannot load */
} PcModInfo;

/* ---- launcher ---------------------------------------------------------- */
/* (Re)scans the mod folders and reads mods.cfg. Safe to call repeatedly
 * before pc_mods_activate; ignored afterwards. */
void pc_mods_scan(void);
size_t pc_mods_count(void);
/* Sorted for display: load order (priority, then dependencies, then id). */
const PcModInfo* pc_mods_info(size_t index);
/* Persists to mods.cfg. Takes effect on the next pc_mods_activate. */
bool pc_mods_set_enabled(const char* id, bool enabled);
/* Folder the user should drop mods into (created on scan). */
const char* pc_mods_user_dir(void);

/* The characters, map packs and items a mod declares, each with its own
 * switch (mods.cfg "pack" lines, on by default). Takes effect on the next
 * pc_mods_activate, like pc_mods_set_enabled. */
typedef struct PcModPack {
    char id[160]; /* "<mod id>/<pack id>" */
    const char* name;
    const char* kind; /* "character", "stage" or "item" */
    bool enabled;
} PcModPack;
size_t pc_mods_pack_count(size_t mod_index);
bool pc_mods_pack_info(size_t mod_index, size_t index, PcModPack* out);
bool pc_mods_set_pack_enabled(const char* full_id, bool enabled);

/* ---- boot ---------------------------------------------------------------- */
/* Call once, after the disc is open and before the game starts: resolves the
 * load order, registers the virtual-disc overlay, compiles tunables, loads
 * plugins and fires MELEE_HOOK_BOOT. */
void pc_mods_activate(void);
void pc_mods_shutdown(void);
/* Number of mods active this session. */
size_t pc_mods_active_count(void);

/* ---- game hooks ---------------------------------------------------------- */
void pc_mods_on_scene(int scene_kind);
void pc_mods_on_frame(void);
/* Fighter attributes were just copied from the fighter's data file:
 * apply every tunable for `kind` to `co_attrs` (an ftCo_DatAttrs). */
void pc_mods_patch_fighter_attrs(int kind, void* co_attrs);
/* A fighter was created on `port`; the SPAWN hook fires on the next frame,
 * once the fighter is fully set up. */
void pc_mods_on_fighter_spawn(int port);

/* FNV-1a over every active mod that affects gameplay, or 0 with none, so an
 * unmodded build stays wire-compatible. Mixed into the netplay handshake. */
uint32_t pc_mods_gameplay_hash(void);

#ifdef __cplusplus
}
#endif

#endif
