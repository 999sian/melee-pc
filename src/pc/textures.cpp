/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "textures.h"
#include "pc.h"

#include <dolphin/gx.h>
#include <aurora/texture.hpp>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

#include <filesystem>
#include <string>

namespace {

aurora::texture::ReplacementGroup s_replacements;
std::filesystem::path s_textures_dir;
std::string s_textures_dir_str;
bool s_initialized = false;

std::filesystem::path resolve_texture_directory() {
    // 1. On Linux/Windows, user data dir is SDL_GetPrefPath("", "melee-pc").
    //    Use a "textures" subdirectory there: <prefPath>/textures
    std::filesystem::path user_dir;
    char* pref = SDL_GetPrefPath("", "melee-pc");
    if (pref != nullptr) {
        user_dir = std::filesystem::path(pref) / "textures";
        SDL_free(pref);
    }

    // 2. Create the user textures directory if it does not exist.
    std::error_code ec;
    if (!user_dir.empty() && !std::filesystem::exists(user_dir, ec)) {
        std::filesystem::create_directories(user_dir, ec);
    }

    // 3. Also check if a local "textures" folder exists beside the executable.
    const char* base = SDL_GetBasePath();
    std::filesystem::path local_dir;
    if (base != nullptr) {
        local_dir = std::filesystem::path(base) / "textures";
    } else {
        local_dir = std::filesystem::path("textures");
    }

    if (std::filesystem::exists(local_dir, ec) && std::filesystem::is_directory(local_dir, ec)) {
        return local_dir;
    }

    return user_dir;
}

}  // namespace

extern "C" void pc_textures_init(void) {
    if (s_initialized) {
        pc_textures_reload();
        return;
    }
    s_initialized = true;

    if (!pc_is_custom_textures_enabled()) {
        return;
    }

    s_textures_dir = resolve_texture_directory();
    s_textures_dir_str = s_textures_dir.string();

    if (!s_textures_dir.empty()) {
        s_replacements = aurora::texture::load_replacement_directory(s_textures_dir);
        SDL_Log("Custom textures: loaded %zu replacement(s) from %s",
            s_replacements.registrations.size(), s_textures_dir_str.c_str());
    }
}

extern "C" void pc_textures_reload(void) {
    if (!pc_is_custom_textures_enabled()) {
        aurora::texture::unregister_replacements(s_replacements);
        s_replacements.registrations.clear();
        return;
    }

    s_textures_dir = resolve_texture_directory();
    s_textures_dir_str = s_textures_dir.string();

    if (!s_textures_dir.empty()) {
        aurora::texture::reload_replacement_directory(s_textures_dir, s_replacements);
        SDL_Log("Custom textures: reloaded %zu replacement(s) from %s",
            s_replacements.registrations.size(), s_textures_dir_str.c_str());
    }
}

extern "C" void pc_textures_shutdown(void) {
    if (!s_replacements.registrations.empty()) {
        aurora::texture::unregister_replacements(s_replacements);
        s_replacements.registrations.clear();
    }
    s_initialized = false;
}

extern "C" const char* pc_textures_get_path(void) {
    return s_textures_dir_str.c_str();
}
