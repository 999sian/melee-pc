/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_RANK_H
#define PC_NET_RANK_H
#include "net_identity.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PC_RANK_RECORD_BYTES 360
#define PC_RANK_MAX_GAMES 4
#define PC_RANK_TIE 2
/* All integer fields in the canonical record are big endian; ratings use
 * IEEE binary64 bits. No native structs are ever sent or signed. */
typedef struct PcNetRating {
    double mu, sigma;
    uint32_t sets;
} PcNetRating;
typedef struct PcNetRankGame {
    uint8_t winner, stocks[2], stage;
    uint32_t frames;
} PcNetRankGame;
typedef struct PcNetRankRecord {
    uint8_t match_id[16], keys[2][32];
    uint32_t ruleset_hash;
    uint8_t game_count;
    PcNetRankGame games[PC_RANK_MAX_GAMES];
    PcNetRating pre[2];
    uint8_t previous[2][32];
    uint64_t timestamp;
    uint8_t signatures[2][64];
} PcNetRankRecord;
typedef struct PcNetRankSet {
    uint8_t wins[2];
    int winner;    /* -1 until one player has won twice */
    int chooser;   /* -1: initial random stage; otherwise previous loser */
    bool tiebreak; /* next game: same stage, one stock, three minutes */
} PcNetRankSet;
void pc_rank_initial(PcNetRating* rating);
double pc_rank_display(const PcNetRating* rating);
bool pc_rank_update(const PcNetRating pre[2], unsigned winner, PcNetRating post[2]);
bool pc_rank_set(const PcNetRankGame* games, size_t count, PcNetRankSet* state);
bool pc_rank_encode(const PcNetRankRecord* record, uint8_t out[PC_RANK_RECORD_BYTES]);
/* Decode validates shape and a completed set; call verify before trusting it. */
bool pc_rank_decode(PcNetRankRecord* record, const void* bytes, size_t length);
bool pc_rank_sign(PcNetRankRecord* record, unsigned player, const PcNetIdentity* identity);
bool pc_rank_verify(const PcNetRankRecord* record);
bool pc_rank_apply(const PcNetRankRecord* record, PcNetRating post[2]);
/* Hash of the complete, double-signed record; false for unsigned records. */
bool pc_rank_head(const PcNetRankRecord* record, uint8_t head[32]);
#ifdef __cplusplus
}
#endif
#endif
