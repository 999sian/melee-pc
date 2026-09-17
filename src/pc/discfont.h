/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Fill HSD_DebugFontAtlas and HSD_SisLib_FontAtlas from the disc's main.dol.
 * Call once after the disc path is known and before the game boots. */
bool pc_load_disc_fonts(const char* disc_path);
#ifdef __cplusplus
}
#endif
