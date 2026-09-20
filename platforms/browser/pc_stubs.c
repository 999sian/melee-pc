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

#include "pc/file_cache.h"
#include "pc/pc.h"

static bool env_flag(const char* name, bool fallback) {
    const char* v = getenv(name);
    return v != NULL && v[0] != '\0' ? v[0] != '0' : fallback;
}

/* Desktop launcher preferences; defaults match launcher_data.hpp. */
bool pc_is_unlock_all_enabled(void) {
    return env_flag("MELEE_UNLOCK_ALL", false);
}
bool pc_is_frozen_stadium_enabled(void) {
    return env_flag("MELEE_FROZEN_STADIUM", false);
}
bool pc_is_free_camera_enabled(void) {
    return false;
}
bool pc_is_ucf_enabled(void) {
    return env_flag("MELEE_UCF", false);
}
int pc_get_hud_mode(void) {
    return 0;
}
bool pc_is_custom_textures_enabled(void) {
    return false;
}
/* Netplay lobby identity (net_lan.c); the lobby itself is unreachable here. */
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
