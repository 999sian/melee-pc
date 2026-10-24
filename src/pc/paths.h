/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PATHS_H
#define PC_PATHS_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller frees the returned UTF-8 path with SDL_free, like SDL_GetPrefPath.
 * Desktop portable.txt beside the executable selects user/ beside it. */
char* pc_get_pref_path(void);
bool pc_is_portable(void);
#ifdef __cplusplus
}
#endif
#endif
