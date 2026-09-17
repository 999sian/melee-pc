/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disc_open.h"

#include <SDL3/SDL_iostream.h>
#include <stdint.h>
#include <stdlib.h>

static int64_t nod_sdl_read_at(void* userData, uint64_t offset, void* out, size_t len) {
    SDL_IOStream* io = (SDL_IOStream*)userData;
    if (io == NULL || out == NULL || offset > (uint64_t)INT64_MAX) {
        return -1;
    }
    if (SDL_SeekIO(io, (Sint64)offset, SDL_IO_SEEK_SET) < 0) {
        return -1;
    }
    size_t total = 0;
    uint8_t* dst = (uint8_t*)out;
    while (total < len) {
        size_t n = SDL_ReadIO(io, dst + total, len - total);
        if (n == 0) {
            break;
        }
        total += n;
    }
    return (int64_t)total;
}

static int64_t nod_sdl_stream_len(void* userData) {
    SDL_IOStream* io = (SDL_IOStream*)userData;
    if (io == NULL) {
        return -1;
    }
    return (int64_t)SDL_GetIOSize(io);
}

static void nod_sdl_stream_close(void* userData) {
    SDL_IOStream* io = (SDL_IOStream*)userData;
    if (io != NULL) {
        SDL_CloseIO(io);
    }
}

NodResult pc_open_nod_disc(const char* path, NodHandle** out) {
    if (path == NULL || out == NULL) {
        return NOD_RESULT_ERR_OTHER;
    }

    SDL_IOStream* io = SDL_IOFromFile(path, "rb");
    if (io != NULL) {
        const NodDiscStream stream = {
            .user_data = io,
            .read_at = nod_sdl_read_at,
            .stream_len = nod_sdl_stream_len,
            .close = nod_sdl_stream_close,
        };
        const NodDiscOptions options = {
            .preloader_threads = 1,
        };
        NodResult res = nod_disc_open_stream(&stream, &options, out);
        if (res == NOD_RESULT_OK && *out != NULL) {
            return NOD_RESULT_OK;
        }
    }

    return nod_disc_open(path, NULL, out);
}
