/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit test for the Slippi replay serializer (src/pc/slp_format.c): the
 * big-endian field layout of each event at SPEC.md's offsets, the UBJSON
 * wrapper and metadata, and the raw length the writer patches at close. */
#include "pc/slp_format.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pc_log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int s_fail;

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond);                     \
            s_fail++;                                                                              \
        }                                                                                          \
    } while (0)

static bool bytes_at(const uint8_t* p, size_t off, const char* hex) {
    for (size_t i = 0; hex[2 * i] != '\0'; i++) {
        unsigned v;
        char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
        sscanf(b, "%2x", &v);
        if (p[off + i] != (uint8_t)v) {
            fprintf(stderr, "  byte 0x%zx: have %02x want %02x\n", off + i, p[off + i], v);
            return false;
        }
    }
    return true;
}

static void test_stores(void) {
    uint8_t b[8] = {0};
    slp_put_u16(b, 0, 0x1234);
    slp_put_u32(b, 2, 0xA1B2C3D4u);
    EXPECT(bytes_at(b, 0, "1234a1b2c3d4"));
    slp_put_f32(b, 0, -1.0f);
    EXPECT(bytes_at(b, 0, "bf800000"));
    slp_put_f32(b, 4, 60.0f);
    EXPECT(bytes_at(b, 4, "42700000"));
}

static void test_event_payloads(void) {
    uint8_t b[64];
    size_t n = slp_event_payloads(b);
    EXPECT(n == SLP_EVENT_PAYLOADS_SIZE);
    EXPECT(b[0] == 0x35);
    EXPECT(b[1] == n - 1); /* 3 * count + 1, this byte included */
    /* Game Start 760, Pre 66, Post 84, Game End 6, Frame Start 12, Item 44,
     * Bookend 8, FoD 9, Whispy 5, Stadium 8 (payloads, command byte out). */
    EXPECT(bytes_at(b, 2,
        "3602f8"
        "370042"
        "380054"
        "390006"
        "3a000c"
        "3b002c"
        "3c0008"
        "3f0009"
        "400005"
        "410008"));
}

static void test_info_block(void) {
    SlpInfoBlock ib;
    memset(&ib, 0, sizeof ib);
    ib.match_kind = 1; /* stock */
    ib.x0_3 = 4;
    ib.timer_enabled = true;
    ib.bits1 = 0x01; /* friendly fire */
    ib.item_freq = -1;
    ib.sd_penalty = -1;
    ib.stkind = 0x20; /* Final Destination */
    ib.time_limit = 480;
    ib.item_mask = 0x0000001234567890ull;
    ib.damage_ratio = 1.0f;
    ib.game_speed = 1.0f;
    ib.players[0].ckind = 2; /* Fox */
    ib.players[0].slot_type = 0;
    ib.players[0].stocks = 4;
    ib.players[0].color = 1;
    ib.players[0].sub_color = 2;
    ib.players[0].team = 1;
    ib.players[0].flags_c = 0x80; /* rumble */
    ib.players[0].cpu_level = 9;
    ib.players[0].damage = 300;
    ib.players[0].attack_ratio = 1.0f;
    ib.players[0].defense_ratio = 1.0f;
    ib.players[0].model_scale = 1.0f;
    ib.players[1].slot_type = 3;
    uint8_t b[SLP_INFO_BLOCK_SIZE];
    slp_info_block(b, &ib);
    EXPECT(b[0x0] == (0x20 | 0x10 | 0x02)); /* stock 0xE0, places 0x1C, timer 0x03 */
    EXPECT(b[0x1] == 0x01);
    EXPECT(b[0xB] == 0xFF && b[0xC] == 0xFF);
    EXPECT(bytes_at(b, 0xE,
        "0020"
        "000001e0"));
    EXPECT(bytes_at(b, 0x20, "0000001234567890"));
    EXPECT(bytes_at(b, 0x30, "3f800000"));
    EXPECT(bytes_at(b, 0x60,
        "0200040100000002"
        "0001"));
    EXPECT(b[0x6C] == 0x80 && b[0x6F] == 9);
    EXPECT(bytes_at(b, 0x70, "012c"));
    EXPECT(bytes_at(b, 0x78, "3f8000003f8000003f800000"));
    EXPECT(b[0x61 + 0x24] == 3);
    for (int i = 0x38; i < 0x60; i++) {
        EXPECT(b[i] == 0); /* the callback pointers are never written */
    }
}

static void test_game_start(void) {
    SlpGameStart g;
    memset(&g, 0, sizeof g);
    for (int i = 0; i < SLP_INFO_BLOCK_SIZE; i++) {
        g.info[i] = (uint8_t)i;
    }
    g.seed = 0xDEADBEEF;
    g.dashback[1] = 1;
    g.shield_drop[3] = 2;
    memcpy(g.nametag[2], "\x82\x60\x82\x61", 4); /* fullwidth "AB" */
    g.pal = true;
    g.minor_scene = 2;
    g.major_scene = 8;
    g.language = 1;
    uint8_t b[SLP_GAME_START_SIZE];
    EXPECT(slp_game_start(b, &g) == 761);
    EXPECT(bytes_at(b, 0, "36031200"));
    EXPECT(b[0x5] == 0x00 && b[0x5 + 0xE] == 0x0E && b[0x5 + 0x137] == 0x37);
    EXPECT(bytes_at(b, 0x13D, "deadbeef"));
    EXPECT(bytes_at(b, 0x141 + 8,
        "00000001"
        "00000000"));
    EXPECT(bytes_at(b, 0x145 + 24, "00000002"));
    EXPECT(bytes_at(b, 0x161 + 0x20, "82608261"));
    EXPECT(b[0x1A1] == 1 && b[0x1A2] == 0 && b[0x1A3] == 2 && b[0x1A4] == 8);
    EXPECT(b[0x2BD] == 1);
    EXPECT(b[0x2F8] == 0);
}

static void test_frame_events(void) {
    uint8_t b[128];
    SlpFrameStart fs = {-123, 0x01020304, 77};
    EXPECT(slp_frame_start(b, &fs) == 13);
    EXPECT(bytes_at(b, 0,
        "3a"
        "ffffff85"
        "01020304"
        "0000004d"));

    SlpPreFrame pre;
    memset(&pre, 0, sizeof pre);
    pre.frame = 0;
    pre.port = 1;
    pre.follower = true;
    pre.seed = 0xCAFEF00D;
    pre.action = 0x14;
    pre.x = 1.0f;
    pre.y = -2.0f;
    pre.facing = -1.0f;
    pre.joy_x = 0.5f;
    pre.trigger = 1.0f;
    pre.buttons = 0x80000100;
    pre.phys_buttons = 0x0110;
    pre.phys_r = 0.25f;
    pre.raw_x = -80;
    pre.percent = 42.5f;
    pre.raw_y = 127;
    pre.raw_cx = -1;
    pre.raw_cy = 1;
    EXPECT(slp_pre_frame(b, &pre) == 67);
    EXPECT(bytes_at(b, 0,
        "37"
        "00000000"
        "01"
        "01"
        "cafef00d"
        "0014"));
    EXPECT(bytes_at(b, 0xD,
        "3f800000"
        "c0000000"
        "bf800000"
        "3f000000"));
    EXPECT(bytes_at(b, 0x29,
        "3f800000"
        "80000100"
        "0110"));
    EXPECT(bytes_at(b, 0x37,
        "3e800000"
        "b0"
        "422a0000"
        "7f"
        "ff"
        "01"));

    SlpPostFrame po;
    memset(&po, 0, sizeof po);
    po.frame = 5000;
    po.port = 3;
    po.character = 0x13;
    po.action = 0x155;
    po.percent = 100.0f;
    po.shield = 60.0f;
    po.last_attack = 0x11;
    po.combo = 2;
    po.last_hit_by = 6;
    po.stocks = 4;
    po.action_frame = 3.0f;
    po.flags[0] = 0x10;
    po.flags[4] = 0x88;
    po.misc_as = 0x41200000; /* 10.0f */
    po.airborne = true;
    po.ground_id = 0xFFFF;
    po.jumps = 1;
    po.l_cancel = 2;
    po.hurtbox = 1;
    po.hitlag = 7.0f;
    po.animation = 3;
    po.instance_hit_by = 0x1234;
    po.instance_id = 0xABCD;
    EXPECT(slp_post_frame(b, &po) == 85);
    EXPECT(bytes_at(b, 0,
        "38"
        "00001388"
        "03"
        "00"
        "13"
        "0155"));
    EXPECT(bytes_at(b, 0x16,
        "42c80000"
        "42700000"
        "11"
        "02"
        "06"
        "04"
        "40400000"));
    EXPECT(bytes_at(b, 0x26,
        "1000000088"
        "41200000"
        "01"
        "ffff"
        "01"
        "02"
        "01"));
    EXPECT(bytes_at(b, 0x49,
        "40e00000"
        "00000003"
        "1234"
        "abcd"));

    SlpItem it;
    memset(&it, 0, sizeof it);
    it.frame = -1;
    it.type = 0x63;
    it.state = 2;
    it.x = 10.0f;
    it.damage = 5;
    it.expire = -3.0f;
    it.spawn_id = 7;
    it.misc[1] = 4;
    it.owner = -1;
    it.instance_id = 9;
    EXPECT(slp_item(b, &it) == 45);
    EXPECT(bytes_at(b, 0,
        "3b"
        "ffffffff"
        "0063"
        "02"));
    EXPECT(bytes_at(b, 0x14,
        "41200000"
        "00000000"
        "0005"
        "c0400000"
        "00000007"));
    EXPECT(bytes_at(b, 0x26,
        "00040000"
        "ff"
        "0009"));

    EXPECT(slp_frame_bookend(b, 12, 12) == 9);
    EXPECT(bytes_at(b, 0,
        "3c"
        "0000000c"
        "0000000c"));

    SlpGameEnd e = {2, -1, {1, 0, -1, -1}};
    EXPECT(slp_game_end(b, &e) == 7);
    EXPECT(bytes_at(b, 0,
        "39"
        "02"
        "ff"
        "0100ffff"));

    EXPECT(slp_fod_platform(b, 3, 1, 20.5f) == 10);
    EXPECT(bytes_at(b, 0,
        "3f"
        "00000003"
        "01"
        "41a40000"));
    EXPECT(slp_whispy(b, 4, 2) == 6);
    EXPECT(bytes_at(b, 0,
        "40"
        "00000004"
        "02"));
    EXPECT(slp_stadium(b, 5, 3, 9) == 9);
    EXPECT(bytes_at(b, 0,
        "41"
        "00000005"
        "0003"
        "0009"));
}

static void test_metadata(void) {
    SlpMeta* m = calloc(1, sizeof *m);
    snprintf(m->start_at, sizeof m->start_at, "2026-09-24T10:00:00Z");
    m->last_frame = 1200;
    m->players[1].present = true;
    m->players[1].frames[9] = 1324;
    m->played_on = "melee-pc";
    uint8_t b[512];
    size_t n = slp_metadata(b, sizeof b, m);
    /* clang-format off */
    static const uint8_t want[] = {'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{',
        'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', 20, '2', '0', '2', '6', '-', '0',
        '9', '-', '2', '4', 'T', '1', '0', ':', '0', '0', ':', '0', '0', 'Z',
        'U', 9, 'l', 'a', 's', 't', 'F', 'r', 'a', 'm', 'e', 'l', 0, 0, 0x04, 0xB0,
        'U', 7, 'p', 'l', 'a', 'y', 'e', 'r', 's', '{',
        'U', 1, '1', '{', 'U', 5, 'n', 'a', 'm', 'e', 's', '{', '}',
        'U', 10, 'c', 'h', 'a', 'r', 'a', 'c', 't', 'e', 'r', 's', '{',
        'U', 1, '9', 'l', 0, 0, 0x05, 0x2C, '}', '}', '}',
        'U', 8, 'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n', 'S', 'U', 8,
        'm', 'e', 'l', 'e', 'e', '-', 'p', 'c', '}'};
    /* clang-format on */
    EXPECT(n == sizeof want);
    EXPECT(n == sizeof want && memcmp(b, want, n) == 0);
    EXPECT(slp_metadata(b, 16, m) == 0); /* does not fit: nothing half-written */
    free(m);
}

/* Walks a raw element the way slippi-js does: command byte, then the size the
 * Event Payloads event declared for it. */
static int walk_raw(const uint8_t* raw, size_t len, int* frames) {
    uint16_t size[256] = {0};
    if (len < 2 || raw[0] != 0x35) {
        return -1;
    }
    size[0x35] = raw[1];
    for (size_t i = 0; i + 3 <= (size_t)raw[1] - 1; i += 3) {
        size[raw[2 + i]] = (uint16_t)(raw[3 + i] << 8 | raw[4 + i]);
    }
    size_t pos = 0;
    int events = 0;
    *frames = 0;
    while (pos < len) {
        const uint8_t cmd = raw[pos];
        if (size[cmd] == 0) {
            return -1;
        }
        if (cmd == 0x3C) {
            (*frames)++;
        }
        pos += 1 + size[cmd];
        events++;
    }
    return pos == len ? events : -1;
}

static uint8_t* read_file(const char* path, size_t* len) {
    return SDL_LoadFile(path, len);
}

static void test_writer(void) {
    char dir[512];
    const char* tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/slp_test_%d", tmp != NULL && tmp[0] != 0 ? tmp : "/tmp",
        (int)SDL_GetCurrentThreadID() ^ rand());
    /* Raw element: payloads, game start, two frames of one fighter. */
    uint8_t raw[2048];
    size_t n = slp_event_payloads(raw);
    SlpGameStart g;
    memset(&g, 0, sizeof g);
    n += slp_game_start(raw + n, &g);
    for (int f = -123; f <= -122; f++) {
        SlpFrameStart fs = {f, 1, 0};
        SlpPreFrame pre;
        SlpPostFrame po;
        memset(&pre, 0, sizeof pre);
        memset(&po, 0, sizeof po);
        pre.frame = po.frame = f;
        n += slp_frame_start(raw + n, &fs);
        n += slp_pre_frame(raw + n, &pre);
        n += slp_post_frame(raw + n, &po);
        n += slp_frame_bookend(raw + n, f, f);
    }
    SlpGameEnd e = {7, 0, {0, 1, -1, -1}};
    n += slp_game_end(raw + n, &e);
    int frames = 0;
    EXPECT(walk_raw(raw, n, &frames) == 2 + 4 * 2 + 1);
    EXPECT(frames == 2);

    SlpMeta* m = calloc(1, sizeof *m);
    snprintf(m->start_at, sizeof m->start_at, "2026-09-24T10:00:00Z");
    m->last_frame = -122;
    m->players[0].present = true;
    m->players[0].frames[2] = 2;
    uint8_t meta[512];
    size_t mlen = slp_metadata(meta, sizeof meta, m);
    free(m);
    EXPECT(mlen > 0);

    for (int copy = 0; copy < 2; copy++) {
        slp_writer_begin(dir, "Game_20260924T100000");
        /* in pieces, as the recorder appends a frame at a time */
        slp_writer_append(raw, 100);
        slp_writer_append(raw + 100, n - 100);
        slp_writer_end(meta, mlen);
    }
    slp_writer_flush();
    char last[1024];
    slp_writer_last_path(last, sizeof last);
    char want[1024];
    snprintf(want, sizeof want, "%s/Game_20260924T100000_2.slp", dir);
    EXPECT(strcmp(last, want) == 0); /* the second game did not overwrite the first */

    for (int copy = 0; copy < 2; copy++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/Game_20260924T100000%s.slp", dir, copy ? "_2" : "");
        size_t len = 0;
        uint8_t* file = read_file(path, &len);
        EXPECT(file != NULL);
        if (file == NULL) {
            continue;
        }
        EXPECT(len == SLP_RAW_HEADER_SIZE + n + mlen + 1);
        static const uint8_t head[] = {'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l'};
        EXPECT(memcmp(file, head, sizeof head) == 0);
        const uint32_t raw_len = (uint32_t)file[11] << 24 | (uint32_t)file[12] << 16 |
                                 (uint32_t)file[13] << 8 | file[14];
        EXPECT(raw_len == n); /* patched at close */
        EXPECT(memcmp(file + SLP_RAW_HEADER_SIZE, raw, n) == 0);
        EXPECT(memcmp(file + SLP_RAW_HEADER_SIZE + n, meta, mlen) == 0);
        EXPECT(file[len - 1] == '}');
        SDL_free(file);
        SDL_RemovePath(path);
    }
    slp_writer_shutdown();
    SDL_RemovePath(dir);
}

int main(void) {
    test_stores();
    test_event_payloads();
    test_info_block();
    test_game_start();
    test_frame_events();
    test_metadata();
    test_writer();
    if (s_fail != 0) {
        fprintf(stderr, "slp_test: %d failure(s)\n", s_fail);
        return 1;
    }
    printf("slp_test: ok\n");
    return 0;
}
