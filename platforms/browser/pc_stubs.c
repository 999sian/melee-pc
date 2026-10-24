/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Native-only pieces of src/pc that the browser platform replaces.
 *
 * The desktop launcher (launcher.cpp), the libusb GameCube adapter
 * (gcadapter.c) and the archive file cache (file_cache.cpp) have no browser
 * counterpart: settings come from the hosting page through the environment,
 * pads arrive through SDL, and disc reads are cached by
 * disc-cache.mjs. Everything else in src/pc is compiled unchanged.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include "pc/file_cache.h"
#include "pc/pc.h"
#include "pc/paths.h"
#include "pc/slp.h"

/* Desktop launcher preferences; defaults match launcher_data.hpp. MELEE_UCF is
 * the one the launcher also reads from the environment (launcher.cpp). */
int pc_get_key_binding(int slot) {
    static const int keys[] = {SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_C,
        SDL_SCANCODE_V, SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB, SDL_SCANCODE_Q, SDL_SCANCODE_E,
        SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_L, SDL_SCANCODE_J,
        SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_Q, SDL_SCANCODE_E};
    return slot >= 0 && slot < (int)SDL_arraysize(keys) ? keys[slot] : 0;
}
bool pc_is_portable(void) {
    return false;
}
char* pc_get_pref_path(void) {
    /* shell.mjs mounts persistent browser storage here before boot. */
    return SDL_strdup("/saves/");
}
bool pc_is_unlock_all_enabled(void) {
    return false;
}
bool pc_is_frozen_stadium_enabled(void) {
    return false;
}
bool pc_is_free_camera_enabled(void) {
    return false;
}
bool pc_is_ucf_enabled(void) {
    const char* env = getenv("MELEE_UCF");
    return env != NULL && env[0] != '0';
}
int pc_get_hud_mode(void) {
    return 0;
}
bool pc_is_custom_textures_enabled(void) {
    return false;
}
/* Netplay lobby identity and Direct Connect settings (net_lan.c, net_match.c); the lobby itself is
 * unreachable here. */
const char* pc_get_net_name(void) {
    return "";
}
const char* pc_get_net_target(void) {
    return "";
}
void pc_set_net_target(const char* code) {
    (void)code;
}
int pc_get_net_port(void) {
    return 0;
}
uint64_t pc_install_id(void) {
    return 0;
}
const char* pc_app_rev(void) {
    return "browser";
}

/* libusb GameCube adapter. */
void pc_gcadapter_init(void) {}
void pc_gcadapter_poll(void) {}
void pc_gcadapter_apply(void) {}
bool pc_gcadapter_status(int port, struct PADStatus* out) {
    (void)port;
    (void)out;
    return false;
}
bool pc_gcadapter_raw(int port, uint8_t raw[6], bool* wireless) {
    (void)port;
    (void)raw;
    (void)wireless;
    return false;
}
uint64_t pc_gcadapter_report_count(void) {
    return 0;
}

/* Archive cache and background prewarm: the page owns disc caching. */
bool pc_file_cache_get(const char* filename, void* dst, size_t* size) {
    (void)filename;
    (void)dst;
    (void)size;
    return false;
}
bool pc_file_cache_get_size(const char* filename, size_t* size) {
    (void)filename;
    (void)size;
    return false;
}
void pc_file_cache_put(const char* filename, const void* data, size_t size) {
    (void)filename;
    (void)data;
    (void)size;
}
void pc_file_cache_start_prewarm(void) {}
bool pc_file_cache_require(const char* filename) {
    (void)filename;
    return false;
}

/* Replay recording writes to a native file; the browser has no recorder. */
void pc_slp_match_start(const struct StartMeleeData* data) {
    (void)data;
}
void pc_slp_match_end(void) {}
void pc_slp_tick_begin(void) {}
void pc_slp_tick_end(uint64_t proc_mask) {
    (void)proc_mask;
}
void pc_slp_pre_frame(struct HSD_GObj* gobj) {
    (void)gobj;
}
