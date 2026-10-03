/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL_log.h>

/* A private PNG-only stb_image: the shared one (thp_jpeg.cpp) is JPEG-only. */
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "pc/stb_image.h"
#pragma GCC diagnostic pop

/* GX_TF_RGBA8: 4x4 texel tiles of 64 bytes, the first 32 holding A,R pairs
 * and the next 32 G,B pairs, tiles in row-major order. */
static void to_gx_rgba8(const unsigned char* rgba, int sw, int sh, uint8_t* out, int w, int h) {
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4) {
            uint8_t* tile = out + ((ty / 4) * (w / 4) + tx / 4) * 64;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    /* Nearest-neighbour resample of the source to w x h. */
                    int sx = (tx + x) * sw / w;
                    int sy = (ty + y) * sh / h;
                    const unsigned char* px = rgba + ((size_t)sy * sw + sx) * 4;
                    int t = y * 4 + x;
                    tile[t * 2 + 0] = px[3];
                    tile[t * 2 + 1] = px[0];
                    tile[32 + t * 2 + 0] = px[1];
                    tile[32 + t * 2 + 1] = px[2];
                }
        }
}

const void* pc_image_gx(PcImage* img, const char* who, int w, int h) {
    if (img == NULL || img->path == NULL || img->failed || w <= 0 || h <= 0 || (w & 3) || (h & 3))
        return NULL;
    if (img->gx != NULL && img->w == w && img->h == h)
        return img->gx;
    int sw = 0, sh = 0, comp = 0;
    unsigned char* rgba = stbi_load(img->path, &sw, &sh, &comp, 4);
    if (rgba == NULL) {
        SDL_Log("mods: %s: cannot read %s (%s)", who ? who : "?", img->path, stbi_failure_reason());
        img->failed = true;
        return NULL;
    }
    uint8_t* gx = (uint8_t*)malloc((size_t)w * h * 4);
    if (gx != NULL)
        to_gx_rgba8(rgba, sw, sh, gx, w, h);
    stbi_image_free(rgba);
    free(img->gx);
    img->gx = gx;
    img->w = w;
    img->h = h;
    return gx;
}
