/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "paths.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

bool pc_is_portable(void) {
#if defined(__ANDROID__) || defined(__APPLE__) && TARGET_OS_IPHONE
    return false;
#else
    const char* base = SDL_GetBasePath();
    if (!base)
        return false;
    char* marker = NULL;
    SDL_asprintf(&marker, "%sportable.txt", base);
    SDL_PathInfo info;
    bool portable = marker && SDL_GetPathInfo(marker, &info) && info.type == SDL_PATHTYPE_FILE;
    SDL_free(marker);
    return portable;
#endif
}

char* pc_get_pref_path(void) {
    if (!pc_is_portable())
        return SDL_GetPrefPath(NULL, "melee-pc");
    char* path = NULL;
    SDL_asprintf(&path, "%suser/", SDL_GetBasePath());
    if (path && !SDL_CreateDirectory(path)) {
        SDL_free(path);
        return NULL;
    }
    return path;
}
