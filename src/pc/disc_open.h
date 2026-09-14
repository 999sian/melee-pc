/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <nod.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opens a GameCube disc image from either a standard filesystem path or
 * an SDL_IOStream-supported URI (such as Android content://).
 * On success, writes the handle to *out and returns NOD_RESULT_OK.
 * The handle should be freed with nod_free().
 */
NodResult pc_open_nod_disc(const char* path, NodHandle** out);

#ifdef __cplusplus
}
#endif
