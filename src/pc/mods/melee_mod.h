/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * melee-pc native mod API.
 *
 * A native plugin is a shared library (plugin.dll / plugin.so / plugin.dylib)
 * inside a mod folder, named by the manifest's "plugin" key. It exports
 *
 *     MELEE_MOD_EXPORT int melee_mod_init(const MeleeModAPI* api, MeleeModHandle self);
 *     MELEE_MOD_EXPORT void melee_mod_shutdown(void);          // optional
 *
 * melee_mod_init runs once, after every enabled mod's files and tunables are
 * registered and before the game's first frame. Return 0 on success; anything
 * else unloads the plugin and the launcher log says so.
 *
 * Compatibility rules:
 *   - api->api_version is MELEE_MOD_API_VERSION of the running game. A plugin
 *     should refuse to start when api->api_version < the version it needs.
 *   - New functions are only ever appended. Check api->size against
 *     offsetof(MeleeModAPI, field) before calling anything newer than v1.
 *
 * Threading: every callback is invoked on the game thread. API functions may
 * only be called from the game thread (inside a hook or melee_mod_init).
 *
 * Netplay: mods that change gameplay make the two peers' games diverge, so the
 * set of enabled gameplay mods is folded into the online handshake and two
 * players with different sets cannot connect. Hooks still run online, but the
 * game re-simulates frames during rollback, so a FRAME hook that writes game
 * state is only safe offline; check api->is_netplay().
 */
#ifndef MELEE_MOD_H
#define MELEE_MOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MELEE_MOD_API_VERSION 1

#if defined(_WIN32)
#define MELEE_MOD_EXPORT __declspec(dllexport)
#else
#define MELEE_MOD_EXPORT __attribute__((visibility("default")))
#endif

/* Opaque identity of the calling mod. Pass it back to the API so logging,
 * config and per-mod state are attributed to the right mod. */
typedef struct MeleeMod* MeleeModHandle;

typedef enum MeleeModHook {
    /* After all plugins are initialised, before the first game frame.
     * arg: unused. */
    MELEE_HOOK_BOOT = 0,
    /* Every presented frame (60 Hz), after the game's own frame work.
     * arg: frame counter (uint32_t, cast through intptr_t). */
    MELEE_HOOK_FRAME = 1,
    /* The game entered a new scene. arg: MeleeSceneKind of the new scene. */
    MELEE_HOOK_SCENE_CHANGE = 2,
    /* A fight scene (VS, Sudden Death, Training, and every 1P/Stadium/Event
     * fight, which all run as VS scenes) started. arg: MeleeSceneKind. */
    MELEE_HOOK_MATCH_START = 3,
    /* The fight scene ended. arg: MeleeSceneKind of the fight that ended. */
    MELEE_HOOK_MATCH_END = 4,
    /* A fighter was created and its attributes loaded and patched.
     * arg: player port (0-5). Query it with get_player(). */
    MELEE_HOOK_FIGHTER_SPAWN = 5,
    MELEE_HOOK_COUNT
} MeleeModHook;

typedef void (*MeleeModHookFn)(MeleeModHook hook, intptr_t arg, void* user);

/* Values match the game's GameSceneKind. */
typedef enum MeleeSceneKind {
    MELEE_SCENE_MENU = 1,
    MELEE_SCENE_VS = 2,
    MELEE_SCENE_SUDDEN_DEATH = 3,
    MELEE_SCENE_TRAINING = 4,
} MeleeSceneKind;

/* Operation for set_fighter_attr. */
typedef enum MeleeAttrOp {
    MELEE_ATTR_SET = 0, /* attr = value */
    MELEE_ATTR_MUL = 1, /* attr *= value */
    MELEE_ATTR_ADD = 2, /* attr += value */
} MeleeAttrOp;

/* Fighter kinds use the game's internal FighterKind numbering:
 * 0 Mario, 1 Fox, 2 Captain Falcon, 3 Donkey Kong, 4 Kirby, 5 Bowser,
 * 6 Link, 7 Sheik, 8 Ness, 9 Peach, 10 Popo, 11 Nana, 12 Pikachu, 13 Samus,
 * 14 Yoshi, 15 Jigglypuff, 16 Mewtwo, 17 Luigi, 18 Marth, 19 Zelda,
 * 20 Young Link, 21 Dr. Mario, 22 Falco, 23 Pichu, 24 Mr. Game & Watch,
 * 25 Ganondorf, 26 Roy, 27 Master Hand, 28 Crazy Hand, 29 Male Wireframe,
 * 30 Female Wireframe, 31 Giga Bowser, 32 Sandbag. -1 means "every fighter". */
#define MELEE_FIGHTER_ALL (-1)

typedef struct MeleePlayerState {
    bool present;         /* a fighter exists on this port right now */
    int32_t fighter_kind; /* FighterKind, see above */
    int32_t costume;
    int32_t stocks;
    float percent;
    float pos_x, pos_y;    /* world position */
    float vel_x, vel_y;    /* self-induced velocity */
    int32_t facing;        /* 1 right, -1 left */
    uint32_t action_state; /* current motion/action state id */
} MeleePlayerState;

typedef struct MeleeModAPI {
    uint32_t api_version; /* MELEE_MOD_API_VERSION of the running game */
    uint32_t size;        /* sizeof(MeleeModAPI) of the running game */
    const char* game_version;

    /* ---- logging ------------------------------------------------------ */
    /* printf-style; written to the game log prefixed with the mod id. */
    void (*log)(MeleeModHandle self, const char* fmt, ...);

    /* ---- this mod ----------------------------------------------------- */
    const char* (*mod_id)(MeleeModHandle self);
    /* Absolute path of the mod folder, no trailing separator. */
    const char* (*mod_dir)(MeleeModHandle self);
    /* True when another mod with this id is enabled. */
    bool (*mod_is_enabled)(const char* mod_id);

    /* ---- configuration -------------------------------------------------
     * Values declared in the manifest's "config" object, overridden by the
     * user's settings. A key that is not declared returns the fallback. */
    double (*config_number)(MeleeModHandle self, const char* key, double fallback);
    bool (*config_bool)(MeleeModHandle self, const char* key, bool fallback);
    const char* (*config_string)(MeleeModHandle self, const char* key, const char* fallback);
    /* Stores a user override and persists it. Returns false for undeclared
     * keys or a value of the wrong type. */
    bool (*config_set_number)(MeleeModHandle self, const char* key, double value);
    bool (*config_set_bool)(MeleeModHandle self, const char* key, bool value);
    bool (*config_set_string)(MeleeModHandle self, const char* key, const char* value);

    /* ---- hooks -------------------------------------------------------- */
    /* Returns a positive id, or 0 on failure. Hooks of the same kind run in
     * mod load order. */
    int (*register_hook)(MeleeModHandle self, MeleeModHook hook, MeleeModHookFn fn, void* user);
    void (*unregister_hook)(MeleeModHandle self, int id);

    /* ---- fighter attribute tunables ------------------------------------
     * Field names are the ftCo_DatAttrs names, e.g. "gravity", "max_jumps",
     * "walk_max_vel", "weight" (docs/modding.md lists them all). Patches are
     * applied every time a fighter's attributes are (re)loaded from its data
     * file, in mod load order and after manifest tunables, so they take effect
     * from the next spawn. kind may be MELEE_FIGHTER_ALL. */
    bool (*set_fighter_attr)(
        MeleeModHandle self, int32_t kind, const char* field, MeleeAttrOp op, float value);
    /* Removes every patch this mod registered through set_fighter_attr. */
    void (*clear_fighter_attrs)(MeleeModHandle self);
    /* Number of patchable fields, and the name of field i (NULL past the end). */
    int32_t (*fighter_attr_count)(void);
    const char* (*fighter_attr_name)(int32_t index);

    /* ---- live game state ----------------------------------------------- */
    int32_t (*scene_kind)(void); /* MeleeSceneKind of the current scene */
    bool (*in_match)(void);
    bool (*is_netplay)(void);
    uint32_t (*frame_count)(void);
    /* Fills *out for port 0-5. Returns false (and present=false) when no
     * fighter is on that port. */
    bool (*get_player)(int32_t port, MeleePlayerState* out);
    /* Reads/writes one attribute of the live fighter on `port`, taking effect
     * immediately (it is reset the next time the fighter's attributes are
     * reloaded). Offline only: returns false during netplay. */
    bool (*get_player_attr)(int32_t port, const char* field, float* out);
    bool (*set_player_attr)(int32_t port, const char* field, float value);
    /* Sets a live fighter's damage percent. Offline only. */
    bool (*set_player_percent)(int32_t port, float percent);

    /* ---- virtual disc --------------------------------------------------
     * True when `disc_path` (e.g. "/PlFxNr.dat") is provided by an enabled
     * mod, and which mod won if several did (NULL when it is from the disc). */
    const char* (*file_provider)(const char* disc_path);
} MeleeModAPI;

typedef int (*MeleeModInitFn)(const MeleeModAPI* api, MeleeModHandle self);
typedef void (*MeleeModShutdownFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* MELEE_MOD_H */
