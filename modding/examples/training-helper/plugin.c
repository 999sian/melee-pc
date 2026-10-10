/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Example native mod for melee-pc. Shows the plugin lifecycle, config reads,
 * hooks, live state queries and an offline-only state write.
 *
 * Build by hand (the CMake build already does this):
 *   gcc -shared -O2 -I <melee-pc>/src/pc/mods plugin.c -o training_helper.dll
 */
#include "melee_mod.h"

#include <stddef.h>

static const MeleeModAPI* api;
static MeleeModHandle self;
static double heal_per_second;
static bool log_spawns;

static void on_match(MeleeModHook hook, intptr_t arg, void* user) {
    (void)user;
    api->log(self, "match %s (scene kind %d)", hook == MELEE_HOOK_MATCH_START ? "started" : "ended",
        (int)arg);
}

static void on_spawn(MeleeModHook hook, intptr_t port, void* user) {
    (void)hook;
    (void)user;
    MeleePlayerState p;
    if (!log_spawns || !api->get_player((int32_t)port, &p))
        return;
    float jumps = 0, gravity = 0;
    api->get_player_attr((int32_t)port, "max_jumps", &jumps);
    api->get_player_attr((int32_t)port, "gravity", &gravity);
    api->log(self, "port %d: fighter kind %d, costume %d, %d stock(s), %d jumps, gravity %.4f",
        (int)port + 1, p.fighter_kind, p.costume, p.stocks, (int)jumps, gravity);
}

static void on_frame(MeleeModHook hook, intptr_t frame, void* user) {
    (void)hook;
    (void)user;
    /* Writing game state from a FRAME hook is not rollback-safe. */
    if (heal_per_second <= 0 || !api->in_match() || api->is_netplay())
        return;
    if ((uint32_t)frame % 60 != 0)
        return;
    for (int32_t port = 0; port < 6; ++port) {
        MeleePlayerState p;
        if (api->get_player(port, &p) && p.percent > 0) {
            float next = p.percent - (float)heal_per_second;
            api->set_player_percent(port, next < 0 ? 0 : next);
        }
    }
}

MELEE_MOD_EXPORT int melee_mod_init(const MeleeModAPI* game, MeleeModHandle handle) {
    if (game->api_version < 1 || game->size < offsetof(MeleeModAPI, file_provider))
        return 1;
    api = game;
    self = handle;
    heal_per_second = api->config_number(self, "heal_per_second", 0);
    log_spawns = api->config_bool(self, "log_spawns", true);

    api->register_hook(self, MELEE_HOOK_MATCH_START, on_match, NULL);
    api->register_hook(self, MELEE_HOOK_MATCH_END, on_match, NULL);
    api->register_hook(self, MELEE_HOOK_FIGHTER_SPAWN, on_spawn, NULL);
    api->register_hook(self, MELEE_HOOK_FRAME, on_frame, NULL);

    api->log(self, "loaded on melee-pc %s (mod API v%u); heal %.1f%%/s, %d patchable attributes",
        api->game_version ? api->game_version : "?", api->api_version, heal_per_second,
        api->fighter_attr_count());
    return 0;
}

MELEE_MOD_EXPORT void melee_mod_shutdown(void) {
    if (api)
        api->log(self, "shutting down");
}
