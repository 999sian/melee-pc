/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#if defined(__APPLE__)
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void ios_show_open_file_dialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window);

#ifdef __cplusplus
}
#endif
#endif
