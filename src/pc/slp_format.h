/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi replay (.slp) byte format and file writer, independent of the game
 * (src/pc/slp.c fills these structs from game state). Layout per the
 * project-slippi wiki SPEC.md: a UBJSON object whose "raw" element is the
 * event byte stream and whose "metadata" element is plain UBJSON.
 *
 * Every event is serialized field by field, big-endian, at the offset the
 * spec gives it. The structs below hold host values only; nothing here is
 * ever copied into the file as memory. */
#ifndef PC_SLP_FORMAT_H
#define PC_SLP_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The replay version written: 3.18.0, the last one SPEC.md documents every
 * field of (it adds the FoD/Whispy/Stadium stage events). */
#define SLP_VERSION_MAJOR 3
#define SLP_VERSION_MINOR 18
#define SLP_VERSION_BUILD 0

#define SLP_FIRST_FRAME (-123)

enum {
    SLP_CMD_EVENT_PAYLOADS = 0x35,
    SLP_CMD_GAME_START = 0x36,
    SLP_CMD_PRE_FRAME = 0x37,
    SLP_CMD_POST_FRAME = 0x38,
    SLP_CMD_GAME_END = 0x39,
    SLP_CMD_FRAME_START = 0x3A,
    SLP_CMD_ITEM = 0x3B,
    SLP_CMD_FRAME_BOOKEND = 0x3C,
    SLP_CMD_FOD_PLATFORM = 0x3F,
    SLP_CMD_WHISPY = 0x40,
    SLP_CMD_STADIUM = 0x41,
};

/* Whole event sizes, command byte included (payload size + 1). */
enum {
    SLP_EVENT_PAYLOADS_SIZE = 2 + 3 * 10,
    SLP_GAME_START_SIZE = 761,
    SLP_PRE_FRAME_SIZE = 67,
    SLP_POST_FRAME_SIZE = 85,
    SLP_GAME_END_SIZE = 7,
    SLP_FRAME_START_SIZE = 13,
    SLP_ITEM_SIZE = 45,
    SLP_FRAME_BOOKEND_SIZE = 9,
    SLP_FOD_PLATFORM_SIZE = 10,
    SLP_WHISPY_SIZE = 6,
    SLP_STADIUM_SIZE = 9,
    SLP_INFO_BLOCK_SIZE = 0x138,
    SLP_RAW_HEADER_SIZE = 15, /* {U\3raw[$U#l + int32 length */
    SLP_RAW_LENGTH_OFFSET = 11,
};

#define SLP_MAX_ITEMS 15
#define SLP_MAX_FIGHTERS 8 /* four ports, each with an Ice Climbers follower */

/* ---- big-endian stores ------------------------------------------------ */
void slp_put_u8(uint8_t* p, size_t off, uint8_t v);
void slp_put_u16(uint8_t* p, size_t off, uint16_t v);
void slp_put_u32(uint8_t* p, size_t off, uint32_t v);
void slp_put_f32(uint8_t* p, size_t off, float v);

/* ---- Game Info Block (StartMeleeData as the GameCube lays it out) -------
 * One field per decomp name (src/melee/mn/types.h StartMeleeRules and
 * PlayerInitData); the bitfields are packed MSB-first as PowerPC does. The
 * rules' eight callback pointers at 0x38-0x5B are written as zero, as
 * Slippi's SendGameInfo nulls them. */
typedef struct SlpInfoPlayer {
    int8_t ckind;      /* external character id */
    uint8_t slot_type; /* 0 human, 1 CPU, 2 demo, 3 none */
    int8_t stocks;
    uint8_t color;
    uint8_t slot;
    int8_t spawn_pos, spawn_dir;
    uint8_t sub_color; /* team shade */
    int8_t handicap;
    uint8_t team, nametag, xB;
    uint8_t flags_c; /* rumble 0x80 .. stamina 0x01, see PlayerInitData */
    uint8_t flags_d;
    uint8_t cpu_kind, cpu_level;
    uint16_t damage, damage1, hp;
    float attack_ratio, defense_ratio, model_scale;
} SlpInfoPlayer;

typedef struct SlpInfoBlock {
    uint8_t match_kind, x0_3;
    bool timer_enabled, timer_counts_up;
    uint8_t bits1; /* x1_0 .. friendly_fire, already MSB-first */
    uint8_t bits2; /* is_stock .. x2_7 */
    uint8_t bits3; /* x3_0 .. x3_7 */
    uint8_t bits4; /* x4_0 .. x4_7 */
    uint8_t bits5; /* x5_0 .. x5_7 */
    uint8_t x6, x7, is_teams, x9, xA;
    int8_t item_freq, sd_penalty;
    uint8_t xD;
    uint16_t stkind;
    uint32_t time_limit;
    uint8_t x14;
    uint32_t x18, x1C;
    uint64_t item_mask; /* x20 */
    int32_t x28;
    float x2C, damage_ratio, game_speed;
    SlpInfoPlayer players[6];
} SlpInfoBlock;

void slp_info_block(uint8_t out[SLP_INFO_BLOCK_SIZE], const SlpInfoBlock* b);

/* ---- events ------------------------------------------------------------ */
typedef struct SlpGameStart {
    uint8_t info[SLP_INFO_BLOCK_SIZE];
    uint32_t seed;
    uint32_t dashback[4], shield_drop[4];
    uint8_t nametag[4][16]; /* Shift JIS, zero padded */
    bool pal, frozen_ps;
    uint8_t minor_scene, major_scene;
    uint8_t language;
} SlpGameStart;

typedef struct SlpFrameStart {
    int32_t frame;
    uint32_t seed;
    uint32_t scene_frame;
} SlpFrameStart;

typedef struct SlpPreFrame {
    int32_t frame;
    uint8_t port;
    bool follower;
    uint32_t seed;
    uint16_t action;
    float x, y, facing;
    float joy_x, joy_y, c_x, c_y, trigger;
    uint32_t buttons;
    uint16_t phys_buttons;
    float phys_l, phys_r;
    int8_t raw_x;
    float percent;
    int8_t raw_y, raw_cx, raw_cy;
} SlpPreFrame;

typedef struct SlpPostFrame {
    int32_t frame;
    uint8_t port;
    bool follower;
    uint8_t character; /* internal id */
    uint16_t action;
    float x, y, facing, percent, shield;
    uint8_t last_attack, combo, last_hit_by, stocks;
    float action_frame;
    uint8_t flags[5];
    uint32_t misc_as; /* raw bits of the motion-var word (hitstun left, a float) */
    bool airborne;
    uint16_t ground_id;
    uint8_t jumps, l_cancel, hurtbox;
    float self_air_x, self_y, attack_x, attack_y, self_ground_x;
    float hitlag;
    uint32_t animation;
    uint16_t instance_hit_by, instance_id;
} SlpPostFrame;

typedef struct SlpItem {
    int32_t frame;
    uint16_t type;
    uint8_t state;
    float facing, vel_x, vel_y, x, y;
    uint16_t damage;
    float expire;
    uint32_t spawn_id;
    uint8_t misc[4];
    int8_t owner;
    uint16_t instance_id;
} SlpItem;

typedef struct SlpGameEnd {
    uint8_t method; /* 1 TIME!, 2 GAME!, 7 No Contest */
    int8_t lras;
    int8_t placements[4];
} SlpGameEnd;

size_t slp_event_payloads(uint8_t* out);
size_t slp_game_start(uint8_t* out, const SlpGameStart* g);
size_t slp_frame_start(uint8_t* out, const SlpFrameStart* f);
size_t slp_pre_frame(uint8_t* out, const SlpPreFrame* p);
size_t slp_post_frame(uint8_t* out, const SlpPostFrame* p);
size_t slp_item(uint8_t* out, const SlpItem* it);
size_t slp_frame_bookend(uint8_t* out, int32_t frame, int32_t finalized);
size_t slp_game_end(uint8_t* out, const SlpGameEnd* e);
size_t slp_fod_platform(uint8_t* out, int32_t frame, uint8_t platform, float height);
size_t slp_whispy(uint8_t* out, int32_t frame, uint8_t direction);
size_t slp_stadium(uint8_t* out, int32_t frame, uint16_t event, uint16_t type);

/* ---- metadata ----------------------------------------------------------
 * The "metadata" key and object, UBJSON: startAt, lastFrame, players (per
 * port: names {} and characters {internal id: frames}), playedOn. */
typedef struct SlpMetaPlayer {
    bool present;
    uint32_t frames[64]; /* by internal character id */
} SlpMetaPlayer;

typedef struct SlpMeta {
    char start_at[32]; /* ISO 8601 UTC, e.g. 2026-09-24T10:00:00Z */
    int32_t last_frame;
    SlpMetaPlayer players[4];
    const char* played_on;
} SlpMeta;

/* Bytes written to `out` (at most `cap`), 0 if it does not fit. */
size_t slp_metadata(uint8_t* out, size_t cap, const SlpMeta* m);

/* The 15 bytes a file starts with; the raw length stays 0 until close. */
void slp_raw_header(uint8_t out[SLP_RAW_HEADER_SIZE]);

/* ---- writer ------------------------------------------------------------
 * One worker thread owns the file, so the caller never waits on the disk:
 * every call copies its bytes into a queue and returns. Files are written one
 * at a time, in call order. */

/* Starts a file <dir>/<stem>.slp (a numeric suffix if that name is taken),
 * creating `dir` if needed, and writes the raw header. */
void slp_writer_begin(const char* dir, const char* stem);
void slp_writer_append(const uint8_t* data, size_t len);
/* Appends the metadata element and the closing brace, patches the raw
 * length and closes the file. */
void slp_writer_end(const uint8_t* metadata, size_t len);
/* Blocks until everything queued so far is on disk. */
void slp_writer_flush(void);
/* flush, then stop the worker (process exit). */
void slp_writer_shutdown(void);
/* Path of the most recently begun file, after a flush; "" if none. */
void slp_writer_last_path(char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
