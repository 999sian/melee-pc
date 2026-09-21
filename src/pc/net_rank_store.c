/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_rank_store.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#define HEADER_SIZE 44
/* Explicit bounded history, never silently compact or discard old records. */
#define MAX_RECORDS 100000
static const uint8_t magic[8] = {'M', 'R', 'H', 'I', 'S', 'T', 2, 0};
struct PcNetRankStore {
    char *path, *temporary;
    uint8_t key[32], head[32];
    PcNetRating rating;
    uint8_t *records, *ids;
    uint32_t count;
    bool poisoned;
#ifdef _WIN32
    HANDLE lock;
#else
    int lock, directory;
#endif
};
static char* path_join(const char* dir, const char* name) {
    size_t a = strlen(dir), b = strlen(name);
    if (a > 32700 || b > 100)
        return NULL;
    char* p = malloc(a + b + 2);
    if (p) {
        memcpy(p, dir, a);
        p[a] = '/';
        memcpy(p + a + 1, name, b + 1);
    }
    return p;
}
static size_t id_position(const PcNetRankStore* s, const uint8_t id[16], bool* found) {
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (memcmp(s->ids + mid * 16, id, 16) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < s->count && memcmp(s->ids + lo * 16, id, 16) == 0;
    return lo;
}
static bool validate(PcNetRankStore* s, const PcNetRankRecord* r, PcNetRating* next,
    uint8_t head[32], size_t* position) {
    if (!r || s->count >= MAX_RECORDS)
        return false;
    int player = !memcmp(s->key, r->keys[0], 32) ? 0 : !memcmp(s->key, r->keys[1], 32) ? 1 : -1;
    if (player < 0 || r->pre[player].sets != s->rating.sets ||
        memcmp(&r->pre[player].mu, &s->rating.mu, 8) ||
        memcmp(&r->pre[player].sigma, &s->rating.sigma, 8) ||
        memcmp(r->previous[player], s->head, 32))
        return false;
    bool duplicate;
    *position = id_position(s, r->match_id, &duplicate);
    PcNetRating post[2];
    if (duplicate || !pc_rank_apply(r, post) || !pc_rank_head(r, head))
        return false;
    *next = post[player];
    return true;
}
static void accept_record(PcNetRankStore* s, const PcNetRankRecord* r, const PcNetRating* next,
    const uint8_t head[32], size_t pos) {
    memmove(s->ids + (pos + 1) * 16, s->ids + pos * 16, (s->count - pos) * 16);
    memcpy(s->ids + pos * 16, r->match_id, 16);
    s->rating = *next;
    memcpy(s->head, head, 32);
    s->count++;
}
static bool reserve(PcNetRankStore* s, size_t count) {
    uint8_t* p = realloc(s->records, count * PC_RANK_RECORD_BYTES);
    if (!p)
        return false;
    s->records = p;
    p = realloc(s->ids, count * 16);
    if (!p)
        return false;
    s->ids = p;
    return true;
}
void pc_rank_store_close(PcNetRankStore* s) {
    if (!s)
        return;
#ifdef _WIN32
    if (s->lock != INVALID_HANDLE_VALUE)
        CloseHandle(s->lock);
#else
    if (s->lock >= 0)
        close(s->lock);
    if (s->directory >= 0)
        close(s->directory);
#endif
    free(s->records);
    free(s->ids);
    free(s->path);
    free(s->temporary);
    free(s);
}
PcNetRankStore* pc_rank_store_open(
    const char* directory, const uint8_t key[32], PcNetRankStoreResult* result) {
    PcNetRankStoreResult status = PC_RANK_STORE_IO;
    PcNetRankStore* s = NULL;
    FILE* file = NULL;
    if (!directory || !key) {
        status = PC_RANK_STORE_INVALID;
        goto fail;
    }
    s = calloc(1, sizeof *s);
    if (!s)
        goto fail;
#ifdef _WIN32
    s->lock = INVALID_HANDLE_VALUE;
#else
    s->lock = s->directory = -1;
#endif
    s->path = path_join(directory, "rank.history");
    s->temporary = path_join(directory, "rank.history.tmp");
    char* lock_path = path_join(directory, "rank.history.lock");
    if (!s->path || !s->temporary || !lock_path) {
        free(lock_path);
        goto fail;
    }
#ifdef _WIN32
    s->lock = CreateFileA(
        lock_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lock_path);
    if (s->lock == INVALID_HANDLE_VALUE)
        goto fail;
#else
    s->directory = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    s->lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lock_path);
    if (s->directory < 0 || s->lock < 0 || flock(s->lock, LOCK_EX | LOCK_NB))
        goto fail;
#endif
    memcpy(s->key, key, 32);
    pc_rank_initial(&s->rating);
    file = fopen(s->path, "rb");
    if (!file) {
        if (errno == ENOENT)
            goto success;
        goto fail;
    }
    if (fseek(file, 0, SEEK_END))
        goto fail;
    long length = ftell(file);  // NOLINT: ftell returns long; bounds are checked before conversion.
    if (length < HEADER_SIZE || (length - HEADER_SIZE) % PC_RANK_RECORD_BYTES ||
        (length - HEADER_SIZE) / PC_RANK_RECORD_BYTES > MAX_RECORDS)
    {
        status = PC_RANK_STORE_INVALID;
        goto fail;
    }
    rewind(file);
    uint8_t header[HEADER_SIZE];
    if (fread(header, 1, sizeof header, file) != sizeof header)
        goto fail;
    if (memcmp(header, magic, 8) || memcmp(header + 8, key, 32)) {
        status = PC_RANK_STORE_INVALID;
        goto fail;
    }
    size_t count = (size_t)(length - HEADER_SIZE) / PC_RANK_RECORD_BYTES;
    uint32_t committed = (uint32_t)header[40] << 24 | (uint32_t)header[41] << 16 |
                         (uint32_t)header[42] << 8 | header[43];
    if (count != committed) {
        status = PC_RANK_STORE_INVALID;
        goto fail;
    }
    if (count && !reserve(s, count))
        goto fail;
    for (size_t i = 0; i < count; i++) {
        uint8_t* wire = s->records + i * PC_RANK_RECORD_BYTES;
        PcNetRankRecord r;
        PcNetRating next;
        uint8_t head[32];
        size_t pos;
        if (fread(wire, 1, PC_RANK_RECORD_BYTES, file) != PC_RANK_RECORD_BYTES)
            goto fail;
        if (!pc_rank_decode(&r, wire, PC_RANK_RECORD_BYTES) || !validate(s, &r, &next, head, &pos))
        {
            status = PC_RANK_STORE_INVALID;
            goto fail;
        }
        accept_record(s, &r, &next, head, pos);
    }
    bool read_error = ferror(file) != 0;
    if (fclose(file))
        read_error = true;
    file = NULL;
    if (read_error)
        goto fail;
success:
    if (result)
        *result = PC_RANK_STORE_OK;
    return s;
fail:
    if (file)
        fclose(file);
    pc_rank_store_close(s);
    if (result)
        *result = status;
    return NULL;
}
bool pc_rank_store_current(
    const PcNetRankStore* s, PcNetRating* rating, uint8_t head[32], uint32_t* count) {
    if (!s || s->poisoned)
        return false;
    if (rating)
        *rating = s->rating;
    if (head)
        memcpy(head, s->head, 32);
    if (count)
        *count = s->count;
    return true;
}
PcNetRankStoreResult pc_rank_store_append(PcNetRankStore* s, const PcNetRankRecord* r) {
    if (!s || s->poisoned)
        return PC_RANK_STORE_UNCERTAIN;
    PcNetRating next;
    uint8_t head[32], wire[PC_RANK_RECORD_BYTES];
    size_t pos;
    if (!validate(s, r, &next, head, &pos) || !pc_rank_encode(r, wire))
        return PC_RANK_STORE_INVALID;
    if (!reserve(s, s->count + 1))
        return PC_RANK_STORE_IO;
    FILE* f = fopen(s->temporary, "wb");
    if (!f)
        return PC_RANK_STORE_IO;
    uint32_t count = s->count + 1;
    uint8_t count_bytes[4] = {
        (uint8_t)(count >> 24), (uint8_t)(count >> 16), (uint8_t)(count >> 8), (uint8_t)count};
    bool ok = fwrite(magic, 1, 8, f) == 8 && fwrite(s->key, 1, 32, f) == 32 &&
              fwrite(count_bytes, 1, 4, f) == 4 &&
              fwrite(s->records, PC_RANK_RECORD_BYTES, s->count, f) == s->count &&
              fwrite(wire, 1, sizeof wire, f) == sizeof wire && !fflush(f);
#ifdef _WIN32
    if (ok && !FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f))))
        ok = false;
#else
    if (ok && fsync(fileno(f)))
        ok = false;
#endif
    if (fclose(f))
        ok = false;
    if (!ok) {
        remove(s->temporary);
        return PC_RANK_STORE_IO;
    }
#ifdef _WIN32
    if (!MoveFileExA(s->temporary, s->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(s->temporary);
        return PC_RANK_STORE_IO;
    }
#else
    if (rename(s->temporary, s->path)) {
        remove(s->temporary);
        return PC_RANK_STORE_IO;
    }
    if (fsync(s->directory)) {
        s->poisoned = true;
        return PC_RANK_STORE_UNCERTAIN;
    }
#endif
    memcpy(s->records + s->count * PC_RANK_RECORD_BYTES, wire, sizeof wire);
    accept_record(s, r, &next, head, pos);
    return PC_RANK_STORE_OK;
}

bool pc_rank_store_peer_state(
    const PcNetRankStore* s, const uint8_t key[32], PcNetRating* rating, uint8_t head[32]) {
    if (!s || s->poisoned || !key || !rating || !head)
        return false;
    bool found = false;
    for (uint32_t i = 0; i < s->count; i++) {
        PcNetRankRecord record;
        PcNetRating post[2];
        if (!pc_rank_decode(&record, s->records + i * PC_RANK_RECORD_BYTES, PC_RANK_RECORD_BYTES))
            return false;
        int player = !memcmp(key, record.keys[0], 32) ? 0 :
                     !memcmp(key, record.keys[1], 32) ? 1 :
                                                        -1;
        if (player < 0 || (found && record.pre[player].sets < rating->sets))
            continue;
        if (!pc_rank_apply(&record, post) || !pc_rank_head(&record, head))
            return false;
        *rating = post[player];
        found = true;
    }
    return found;
}

bool pc_rank_store_latest_record(const PcNetRankStore* s, uint8_t out[PC_RANK_RECORD_BYTES]) {
    if (!s || s->poisoned || !s->count || !out)
        return false;
    memcpy(out, s->records + (s->count - 1) * PC_RANK_RECORD_BYTES, PC_RANK_RECORD_BYTES);
    return true;
}
