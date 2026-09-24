/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi replay byte format and the worker-thread file writer. See
 * slp_format.h; offsets are SPEC.md's (project-slippi/slippi-wiki). */
#include "slp_format.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pc_log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- big-endian stores ------------------------------------------------ */

void slp_put_u8(uint8_t* p, size_t off, uint8_t v) {
    p[off] = v;
}

void slp_put_u16(uint8_t* p, size_t off, uint16_t v) {
    p[off] = (uint8_t)(v >> 8);
    p[off + 1] = (uint8_t)v;
}

void slp_put_u32(uint8_t* p, size_t off, uint32_t v) {
    p[off] = (uint8_t)(v >> 24);
    p[off + 1] = (uint8_t)(v >> 16);
    p[off + 2] = (uint8_t)(v >> 8);
    p[off + 3] = (uint8_t)v;
}

void slp_put_f32(uint8_t* p, size_t off, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    slp_put_u32(p, off, u);
}

static void put_i32(uint8_t* p, size_t off, int32_t v) {
    slp_put_u32(p, off, (uint32_t)v);
}

/* ---- Game Info Block --------------------------------------------------- */

void slp_info_block(uint8_t out[SLP_INFO_BLOCK_SIZE], const SlpInfoBlock* b) {
    memset(out, 0, SLP_INFO_BLOCK_SIZE);
    /* u32 match_kind:3, x0_3:3, timer_enabled:1, timer_counts_up:1, ...:
     * PowerPC packs the first-declared field into the most significant bits. */
    out[0x0] = (uint8_t)(((b->match_kind & 7) << 5) | ((b->x0_3 & 7) << 2) |
                         (b->timer_enabled ? 0x02 : 0) | (b->timer_counts_up ? 0x01 : 0));
    out[0x1] = b->bits1;
    out[0x2] = b->bits2;
    out[0x3] = b->bits3;
    out[0x4] = b->bits4;
    out[0x5] = b->bits5;
    out[0x6] = b->x6; /* bomb rain */
    out[0x7] = b->x7;
    out[0x8] = b->is_teams;
    out[0x9] = b->x9;
    out[0xA] = b->xA;
    out[0xB] = (uint8_t)b->item_freq;
    out[0xC] = (uint8_t)b->sd_penalty;
    out[0xD] = b->xD;
    slp_put_u16(out, 0xE, b->stkind);
    slp_put_u32(out, 0x10, b->time_limit);
    out[0x14] = b->x14;
    slp_put_u32(out, 0x18, b->x18);
    slp_put_u32(out, 0x1C, b->x1C);
    slp_put_u32(out, 0x20, (uint32_t)(b->item_mask >> 32));
    slp_put_u32(out, 0x24, (uint32_t)b->item_mask);
    put_i32(out, 0x28, b->x28);
    slp_put_f32(out, 0x2C, b->x2C);
    slp_put_f32(out, 0x30, b->damage_ratio);
    slp_put_f32(out, 0x34, b->game_speed);
    /* 0x38-0x5F: the callbacks and two data pointers, left zero */
    for (int i = 0; i < 6; i++) {
        const SlpInfoPlayer* p = &b->players[i];
        uint8_t* o = out + 0x60 + 0x24 * i;
        o[0x0] = (uint8_t)p->ckind;
        o[0x1] = p->slot_type;
        o[0x2] = (uint8_t)p->stocks;
        o[0x3] = p->color;
        o[0x4] = p->slot;
        o[0x5] = (uint8_t)p->spawn_pos;
        o[0x6] = (uint8_t)p->spawn_dir;
        o[0x7] = p->sub_color;
        o[0x8] = (uint8_t)p->handicap;
        o[0x9] = p->team;
        o[0xA] = p->nametag;
        o[0xB] = p->xB;
        o[0xC] = p->flags_c;
        o[0xD] = p->flags_d;
        o[0xE] = p->cpu_kind;
        o[0xF] = p->cpu_level;
        slp_put_u16(o, 0x10, p->damage);
        slp_put_u16(o, 0x12, p->damage1);
        slp_put_u16(o, 0x14, p->hp);
        slp_put_f32(o, 0x18, p->attack_ratio);
        slp_put_f32(o, 0x1C, p->defense_ratio);
        slp_put_f32(o, 0x20, p->model_scale);
    }
}

/* ---- events ------------------------------------------------------------ */

size_t slp_event_payloads(uint8_t* out) {
    static const struct {
        uint8_t cmd;
        uint16_t size;
    } k[] = {
        {SLP_CMD_GAME_START, SLP_GAME_START_SIZE - 1},
        {SLP_CMD_PRE_FRAME, SLP_PRE_FRAME_SIZE - 1},
        {SLP_CMD_POST_FRAME, SLP_POST_FRAME_SIZE - 1},
        {SLP_CMD_GAME_END, SLP_GAME_END_SIZE - 1},
        {SLP_CMD_FRAME_START, SLP_FRAME_START_SIZE - 1},
        {SLP_CMD_ITEM, SLP_ITEM_SIZE - 1},
        {SLP_CMD_FRAME_BOOKEND, SLP_FRAME_BOOKEND_SIZE - 1},
        {SLP_CMD_FOD_PLATFORM, SLP_FOD_PLATFORM_SIZE - 1},
        {SLP_CMD_WHISPY, SLP_WHISPY_SIZE - 1},
        {SLP_CMD_STADIUM, SLP_STADIUM_SIZE - 1},
    };
    const size_t n = sizeof k / sizeof k[0];
    out[0] = SLP_CMD_EVENT_PAYLOADS;
    out[1] = (uint8_t)(3 * n + 1); /* this byte and the command/size pairs */
    for (size_t i = 0; i < n; i++) {
        out[2 + 3 * i] = k[i].cmd;
        slp_put_u16(out, 3 + 3 * i, k[i].size);
    }
    return 2 + 3 * n;
}

size_t slp_game_start(uint8_t* out, const SlpGameStart* g) {
    memset(out, 0, SLP_GAME_START_SIZE);
    out[0x0] = SLP_CMD_GAME_START;
    out[0x1] = SLP_VERSION_MAJOR;
    out[0x2] = SLP_VERSION_MINOR;
    out[0x3] = SLP_VERSION_BUILD;
    out[0x4] = 0;
    memcpy(out + 0x5, g->info, SLP_INFO_BLOCK_SIZE); /* already big-endian bytes */
    slp_put_u32(out, 0x13D, g->seed);
    for (int i = 0; i < 4; i++) {
        slp_put_u32(out, 0x141 + 0x8 * i, g->dashback[i]);
        slp_put_u32(out, 0x145 + 0x8 * i, g->shield_drop[i]);
        memcpy(out + 0x161 + 0x10 * i, g->nametag[i], 0x10);
    }
    out[0x1A1] = g->pal;
    out[0x1A2] = g->frozen_ps;
    out[0x1A3] = g->minor_scene;
    out[0x1A4] = g->major_scene;
    /* 0x1A5 display names, 0x221 connect codes, 0x249 Slippi UIDs: Slippi
     * Online only, zero */
    out[0x2BD] = g->language;
    /* 0x2BE session id, 0x2F1 game number, 0x2F5 tiebreaker: zero */
    return SLP_GAME_START_SIZE;
}

size_t slp_frame_start(uint8_t* out, const SlpFrameStart* f) {
    out[0x0] = SLP_CMD_FRAME_START;
    put_i32(out, 0x1, f->frame);
    slp_put_u32(out, 0x5, f->seed);
    slp_put_u32(out, 0x9, f->scene_frame);
    return SLP_FRAME_START_SIZE;
}

size_t slp_pre_frame(uint8_t* out, const SlpPreFrame* p) {
    out[0x0] = SLP_CMD_PRE_FRAME;
    put_i32(out, 0x1, p->frame);
    out[0x5] = p->port;
    out[0x6] = p->follower;
    slp_put_u32(out, 0x7, p->seed);
    slp_put_u16(out, 0xB, p->action);
    slp_put_f32(out, 0xD, p->x);
    slp_put_f32(out, 0x11, p->y);
    slp_put_f32(out, 0x15, p->facing);
    slp_put_f32(out, 0x19, p->joy_x);
    slp_put_f32(out, 0x1D, p->joy_y);
    slp_put_f32(out, 0x21, p->c_x);
    slp_put_f32(out, 0x25, p->c_y);
    slp_put_f32(out, 0x29, p->trigger);
    slp_put_u32(out, 0x2D, p->buttons);
    slp_put_u16(out, 0x31, p->phys_buttons);
    slp_put_f32(out, 0x33, p->phys_l);
    slp_put_f32(out, 0x37, p->phys_r);
    out[0x3B] = (uint8_t)p->raw_x;
    slp_put_f32(out, 0x3C, p->percent);
    out[0x40] = (uint8_t)p->raw_y;
    out[0x41] = (uint8_t)p->raw_cx;
    out[0x42] = (uint8_t)p->raw_cy;
    return SLP_PRE_FRAME_SIZE;
}

size_t slp_post_frame(uint8_t* out, const SlpPostFrame* p) {
    out[0x0] = SLP_CMD_POST_FRAME;
    put_i32(out, 0x1, p->frame);
    out[0x5] = p->port;
    out[0x6] = p->follower;
    out[0x7] = p->character;
    slp_put_u16(out, 0x8, p->action);
    slp_put_f32(out, 0xA, p->x);
    slp_put_f32(out, 0xE, p->y);
    slp_put_f32(out, 0x12, p->facing);
    slp_put_f32(out, 0x16, p->percent);
    slp_put_f32(out, 0x1A, p->shield);
    out[0x1E] = p->last_attack;
    out[0x1F] = p->combo;
    out[0x20] = p->last_hit_by;
    out[0x21] = p->stocks;
    slp_put_f32(out, 0x22, p->action_frame);
    for (int i = 0; i < 5; i++) {
        out[0x26 + i] = p->flags[i];
    }
    slp_put_u32(out, 0x2B, p->misc_as);
    out[0x2F] = p->airborne;
    slp_put_u16(out, 0x30, p->ground_id);
    out[0x32] = p->jumps;
    out[0x33] = p->l_cancel;
    out[0x34] = p->hurtbox;
    slp_put_f32(out, 0x35, p->self_air_x);
    slp_put_f32(out, 0x39, p->self_y);
    slp_put_f32(out, 0x3D, p->attack_x);
    slp_put_f32(out, 0x41, p->attack_y);
    slp_put_f32(out, 0x45, p->self_ground_x);
    slp_put_f32(out, 0x49, p->hitlag);
    slp_put_u32(out, 0x4D, p->animation);
    slp_put_u16(out, 0x51, p->instance_hit_by);
    slp_put_u16(out, 0x53, p->instance_id);
    return SLP_POST_FRAME_SIZE;
}

size_t slp_item(uint8_t* out, const SlpItem* it) {
    out[0x0] = SLP_CMD_ITEM;
    put_i32(out, 0x1, it->frame);
    slp_put_u16(out, 0x5, it->type);
    out[0x7] = it->state;
    slp_put_f32(out, 0x8, it->facing);
    slp_put_f32(out, 0xC, it->vel_x);
    slp_put_f32(out, 0x10, it->vel_y);
    slp_put_f32(out, 0x14, it->x);
    slp_put_f32(out, 0x18, it->y);
    slp_put_u16(out, 0x1C, it->damage);
    slp_put_f32(out, 0x1E, it->expire);
    slp_put_u32(out, 0x22, it->spawn_id);
    for (int i = 0; i < 4; i++) {
        out[0x26 + i] = it->misc[i];
    }
    out[0x2A] = (uint8_t)it->owner;
    slp_put_u16(out, 0x2B, it->instance_id);
    return SLP_ITEM_SIZE;
}

size_t slp_frame_bookend(uint8_t* out, int32_t frame, int32_t finalized) {
    out[0x0] = SLP_CMD_FRAME_BOOKEND;
    put_i32(out, 0x1, frame);
    put_i32(out, 0x5, finalized);
    return SLP_FRAME_BOOKEND_SIZE;
}

size_t slp_game_end(uint8_t* out, const SlpGameEnd* e) {
    out[0x0] = SLP_CMD_GAME_END;
    out[0x1] = e->method;
    out[0x2] = (uint8_t)e->lras;
    for (int i = 0; i < 4; i++) {
        out[0x3 + i] = (uint8_t)e->placements[i];
    }
    return SLP_GAME_END_SIZE;
}

size_t slp_fod_platform(uint8_t* out, int32_t frame, uint8_t platform, float height) {
    out[0x0] = SLP_CMD_FOD_PLATFORM;
    put_i32(out, 0x1, frame);
    out[0x5] = platform;
    slp_put_f32(out, 0x6, height);
    return SLP_FOD_PLATFORM_SIZE;
}

size_t slp_whispy(uint8_t* out, int32_t frame, uint8_t direction) {
    out[0x0] = SLP_CMD_WHISPY;
    put_i32(out, 0x1, frame);
    out[0x5] = direction;
    return SLP_WHISPY_SIZE;
}

size_t slp_stadium(uint8_t* out, int32_t frame, uint16_t event, uint16_t type) {
    out[0x0] = SLP_CMD_STADIUM;
    put_i32(out, 0x1, frame);
    slp_put_u16(out, 0x5, event);
    slp_put_u16(out, 0x7, type);
    return SLP_STADIUM_SIZE;
}

/* ---- metadata (UBJSON) ------------------------------------------------- */

typedef struct Ub {
    uint8_t* p;
    size_t n, cap;
    bool full;
} Ub;

static void ub_bytes(Ub* u, const void* src, size_t len) {
    if (u->full || u->cap - u->n < len) {
        u->full = true;
        return;
    }
    memcpy(u->p + u->n, src, len);
    u->n += len;
}

static void ub_byte(Ub* u, uint8_t b) {
    ub_bytes(u, &b, 1);
}

/* Object keys are strings without the 'S' marker (Draft 12). */
static void ub_key(Ub* u, const char* k) {
    size_t len = strlen(k);
    ub_byte(u, 'U');
    ub_byte(u, (uint8_t)len);
    ub_bytes(u, k, len);
}

static void ub_string(Ub* u, const char* s) {
    size_t len = strlen(s);
    ub_byte(u, 'S');
    ub_byte(u, 'U');
    ub_byte(u, (uint8_t)len);
    ub_bytes(u, s, len);
}

static void ub_int32(Ub* u, int32_t v) {
    uint8_t b[4];
    slp_put_u32(b, 0, (uint32_t)v);
    ub_byte(u, 'l');
    ub_bytes(u, b, 4);
}

size_t slp_metadata(uint8_t* out, size_t cap, const SlpMeta* m) {
    Ub u = {out, 0, cap, false};
    char key[8];
    ub_key(&u, "metadata");
    ub_byte(&u, '{');
    ub_key(&u, "startAt");
    ub_string(&u, m->start_at);
    ub_key(&u, "lastFrame");
    ub_int32(&u, m->last_frame);
    ub_key(&u, "players");
    ub_byte(&u, '{');
    for (int port = 0; port < 4; port++) {
        const SlpMetaPlayer* p = &m->players[port];
        if (!p->present) {
            continue;
        }
        snprintf(key, sizeof key, "%d", port);
        ub_key(&u, key);
        ub_byte(&u, '{');
        ub_key(&u, "names");
        ub_byte(&u, '{');
        ub_byte(&u, '}');
        ub_key(&u, "characters");
        ub_byte(&u, '{');
        for (int id = 0; id < 64; id++) {
            if (p->frames[id] != 0) {
                snprintf(key, sizeof key, "%d", id);
                ub_key(&u, key);
                ub_int32(&u, (int32_t)p->frames[id]);
            }
        }
        ub_byte(&u, '}');
        ub_byte(&u, '}');
    }
    ub_byte(&u, '}');
    ub_key(&u, "playedOn");
    ub_string(&u, m->played_on != NULL ? m->played_on : "melee-pc");
    ub_byte(&u, '}');
    return u.full ? 0 : u.n;
}

void slp_raw_header(uint8_t out[SLP_RAW_HEADER_SIZE]) {
    static const uint8_t h[SLP_RAW_HEADER_SIZE] = {
        '{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0};
    memcpy(out, h, sizeof h);
}

/* ---- writer thread -----------------------------------------------------
 * The game thread only allocates a message and links it in; the worker owns
 * the file and does every open, write, seek and close. */

enum { MSG_BEGIN, MSG_DATA, MSG_END };

typedef struct Msg {
    struct Msg* next;
    int kind;
    size_t len;
    uint8_t data[]; /* MSG_BEGIN: "dir\0stem\0" */
} Msg;

static SDL_Mutex* s_lock;
static SDL_Condition* s_cond;
static SDL_Thread* s_thread;
static Msg *s_head, *s_tail;
static uint64_t s_queued, s_done; /* messages linked in / finished */
static bool s_quit;
static char s_last_path[1024]; /* under s_lock */

/* Worker-only state. */
static SDL_IOStream* s_io;
static uint32_t s_raw_len;
static char s_path[1024];
static bool s_io_error;

static void io_write(const void* p, size_t n) {
    if (s_io == NULL || n == 0) {
        return;
    }
    if (SDL_WriteIO(s_io, p, n) != n && !s_io_error) {
        s_io_error = true;
        pc_log_line("slp: write to %s failed: %s", s_path, SDL_GetError());
    }
}

/* Patch the raw element's length and close; `meta` NULL closes a file whose
 * game was cut short without its metadata. */
static void io_close(const uint8_t* meta, size_t meta_len) {
    if (s_io == NULL) {
        return;
    }
    if (meta != NULL) {
        io_write(meta, meta_len);
    }
    io_write("}", 1);
    uint8_t len[4];
    slp_put_u32(len, 0, s_raw_len);
    if (SDL_SeekIO(s_io, SLP_RAW_LENGTH_OFFSET, SDL_IO_SEEK_SET) < 0) {
        pc_log_line("slp: seek in %s failed: %s", s_path, SDL_GetError());
    } else {
        io_write(len, sizeof len);
    }
    if (!SDL_CloseIO(s_io) && !s_io_error) {
        pc_log_line("slp: closing %s failed: %s", s_path, SDL_GetError());
    }
    s_io = NULL;
    pc_log_line("slp: wrote %s (%u bytes of events)", s_path, (unsigned)s_raw_len);
}

static void io_begin(const char* dir, const char* stem) {
    io_close(NULL, 0); /* a file never ended is closed as it stands */
    s_io_error = false;
    s_raw_len = 0;
    if (!SDL_CreateDirectory(dir)) {
        pc_log_line("slp: cannot create %s: %s", dir, SDL_GetError());
    }
    /* Two games cannot share a second on one machine, but two instances on
     * one directory can: a taken name gets a suffix, never an overwrite.
     * "x" creates exclusively, so two processes cannot both claim a name. */
    for (int i = 1; i < 100 && s_io == NULL; i++) {
        if (i == 1) {
            snprintf(s_path, sizeof s_path, "%s/%s.slp", dir, stem);
        } else {
            snprintf(s_path, sizeof s_path, "%s/%s_%d.slp", dir, stem, i);
        }
        s_io = SDL_IOFromFile(s_path, "wbx");
        if (s_io == NULL && !SDL_GetPathInfo(s_path, NULL)) {
            break; /* not a name clash: the directory cannot be written */
        }
    }
    if (s_io == NULL) {
        pc_log_line("slp: cannot open %s: %s", s_path, SDL_GetError());
        return;
    }
    uint8_t h[SLP_RAW_HEADER_SIZE];
    slp_raw_header(h);
    io_write(h, sizeof h);
    SDL_LockMutex(s_lock);
    snprintf(s_last_path, sizeof s_last_path, "%s", s_path);
    SDL_UnlockMutex(s_lock);
    pc_log_line("slp: recording to %s", s_path);
}

static void process(const Msg* m) {
    switch (m->kind) {
    case MSG_BEGIN: {
        const char* dir = (const char*)m->data;
        io_begin(dir, dir + strlen(dir) + 1);
        break;
    }
    case MSG_DATA:
        io_write(m->data, m->len);
        if (s_io != NULL) {
            s_raw_len += (uint32_t)m->len;
        }
        break;
    case MSG_END:
        io_close(m->data, m->len);
        break;
    }
}

static int SDLCALL worker(void* arg) {
    (void)arg;
    SDL_LockMutex(s_lock);
    for (;;) {
        while (s_head == NULL && !s_quit) {
            SDL_WaitCondition(s_cond, s_lock);
        }
        if (s_head == NULL) {
            break; /* quit with nothing left */
        }
        Msg* list = s_head;
        s_head = s_tail = NULL;
        SDL_UnlockMutex(s_lock);
        uint64_t n = 0;
        while (list != NULL) {
            Msg* next = list->next;
            process(list);
            free(list);
            list = next;
            n++;
        }
        SDL_LockMutex(s_lock);
        s_done += n;
        SDL_BroadcastCondition(s_cond);
    }
    SDL_UnlockMutex(s_lock);
    io_close(NULL, 0);
    return 0;
}

static bool writer_start(void) {
    if (s_thread != NULL) {
        return true;
    }
    if (s_lock == NULL) {
        s_lock = SDL_CreateMutex();
        s_cond = SDL_CreateCondition();
    }
    if (s_lock == NULL || s_cond == NULL) {
        return false;
    }
    s_quit = false;
    s_thread = SDL_CreateThread(worker, "slp writer", NULL);
    if (s_thread == NULL) {
        pc_log_line("slp: cannot start the writer thread: %s", SDL_GetError());
    }
    return s_thread != NULL;
}

static void push(int kind, const void* a, size_t alen, const void* b, size_t blen) {
    if (!writer_start()) {
        return;
    }
    Msg* m = malloc(sizeof *m + alen + blen);
    if (m == NULL) {
        return;
    }
    m->next = NULL;
    m->kind = kind;
    m->len = alen + blen;
    if (alen != 0) {
        memcpy(m->data, a, alen);
    }
    if (blen != 0) {
        memcpy(m->data + alen, b, blen);
    }
    SDL_LockMutex(s_lock);
    if (s_tail != NULL) {
        s_tail->next = m;
    } else {
        s_head = m;
    }
    s_tail = m;
    s_queued++;
    SDL_SignalCondition(s_cond);
    SDL_UnlockMutex(s_lock);
}

void slp_writer_begin(const char* dir, const char* stem) {
    push(MSG_BEGIN, dir, strlen(dir) + 1, stem, strlen(stem) + 1);
}

void slp_writer_append(const uint8_t* data, size_t len) {
    if (len != 0) {
        push(MSG_DATA, data, len, NULL, 0);
    }
}

void slp_writer_end(const uint8_t* metadata, size_t len) {
    push(MSG_END, metadata, len, NULL, 0);
}

void slp_writer_flush(void) {
    if (s_thread == NULL) {
        return;
    }
    SDL_LockMutex(s_lock);
    const uint64_t want = s_queued;
    while (s_done < want) {
        SDL_WaitCondition(s_cond, s_lock);
    }
    SDL_UnlockMutex(s_lock);
}

void slp_writer_shutdown(void) {
    if (s_thread == NULL) {
        return;
    }
    slp_writer_flush();
    SDL_LockMutex(s_lock);
    s_quit = true;
    SDL_SignalCondition(s_cond);
    SDL_UnlockMutex(s_lock);
    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
}

void slp_writer_last_path(char* out, size_t cap) {
    if (s_lock == NULL) {
        snprintf(out, cap, "%s", "");
        return;
    }
    SDL_LockMutex(s_lock);
    snprintf(out, cap, "%s", s_last_path);
    SDL_UnlockMutex(s_lock);
}
