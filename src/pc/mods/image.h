/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PNGs from mod folders as GX RGBA8 textures (character and map packs). */
#ifndef PC_MODS_IMAGE_H
#define PC_MODS_IMAGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One image file, decoded on first use and kept at the size last asked for. */
typedef struct PcImage {
    char* path; /* host path, owned; NULL for "none" */
    void* gx;   /* cached GX_TF_RGBA8 data (4x4 tiles), w * h * 4 bytes */
    int w, h;
    bool failed;
} PcImage;

/* The image resampled to w x h (multiples of 4) as GX RGBA8, or NULL when
 * there is no image or it cannot be read; @p who names it in the log. */
const void* pc_image_gx(PcImage* img, const char* who, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
