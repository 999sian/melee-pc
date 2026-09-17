/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_TEXTURES_H
#define PC_TEXTURES_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize texture replacements if custom textures are enabled.
 * Resolves user textures directory (<prefPath>/textures) and checks
 * local textures directory (<basePath>/textures beside the executable).
 * Creates the user textures directory if it does not exist.
 * Calls aurora::texture::load_replacement_directory(textures_dir). */
void pc_textures_init(void);

/* Reload texture replacements from disk. */
void pc_textures_reload(void);

/* Unregister and unload active texture replacements. */
void pc_textures_shutdown(void);

/* Returns the resolved textures directory path (or empty string if none). */
const char* pc_textures_get_path(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_TEXTURES_H */
