/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_rank.h"
#include "monocypher.h"
#include "../../extern/dht/sha1.h"
#include "libm/pc_libm.h"
#include <float.h>
#include <math.h>
#include <string.h>
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
    "rank records require IEEE binary64");
#define BODY_BYTES (PC_RANK_RECORD_BYTES - 128)
static bool rating_valid(const PcNetRating* r) {
    return isfinite(r->mu) && isfinite(r->sigma) && r->mu >= -1000 && r->mu <= 1000 &&
           r->sigma > 0 && r->sigma <= 100 && r->sets < UINT32_MAX;
}
void pc_rank_initial(PcNetRating* r) {
    *r = (PcNetRating){25, 25.0 / 3, 0};
}
double pc_rank_display(const PcNetRating* r) {
    return 1000 + 40 * (r->mu - 3 * r->sigma);
}
bool pc_rank_update(const PcNetRating pre[2], unsigned winner, PcNetRating post[2]) {
    if (!pre || !post || winner > 1 || !rating_valid(&pre[0]) || !rating_valid(&pre[1]))
        return false;
    const double beta = 25.0 / 6, tau = 25.0 / 300;
    double variance[2] = {
        pre[0].sigma * pre[0].sigma + tau * tau, pre[1].sigma * pre[1].sigma + tau * tau};
    double c = pc_rank_sqrt(variance[0] + variance[1] + 2 * beta * beta);
    PcNetRating result[2];
    for (unsigned i = 0; i < 2; i++) {
        double p = 1 / (1 + pc_rank_exp((pre[1 - i].mu - pre[i].mu) / c));
        double omega = variance[i] / c * ((winner == i ? 1.0 : 0.0) - p);
        double delta = pc_rank_sqrt(variance[i]) / c * variance[i] / (c * c) * p * (1 - p);
        result[i] = (PcNetRating){pre[i].mu + omega,
            pc_rank_sqrt(variance[i] * fmax(1 - delta, 0.0001)), pre[i].sets + 1};
    }
    memcpy(post, result, sizeof result);
    return true;
}
static bool legal_stage(unsigned s) {
    return s == 2 || s == 3 || s == 8 || s == 28 || s == 31 || s == 32;
}
bool pc_rank_set(const PcNetRankGame* games, size_t count, PcNetRankSet* out) {
    if (count > PC_RANK_MAX_GAMES || (count && !games) || !out)
        return false;
    PcNetRankSet state = {{0, 0}, -1, -1, false};
    for (size_t i = 0; i < count; ++i) {
        const PcNetRankGame* g = &games[i];
        unsigned stocks = state.tiebreak ? 1 : 4;
        if (state.winner >= 0 || !legal_stage(g->stage) || g->winner > PC_RANK_TIE || !g->frames ||
            g->frames > (state.tiebreak ? 180u : 480u) * 60u || g->stocks[0] > stocks ||
            g->stocks[1] > stocks || (state.tiebreak && g->stage != games[i - 1].stage))
            return false;
        if (g->winner == PC_RANK_TIE) {
            if (g->stocks[0] != g->stocks[1])
                return false;
            state.tiebreak = true;
        } else {
            if (!g->stocks[g->winner] || g->stocks[g->winner] < g->stocks[1 - g->winner])
                return false;
            state.tiebreak = false;
            state.chooser = 1 - g->winner;
            if (++state.wins[g->winner] == 2)
                state.winner = g->winner;
        }
    }
    *out = state;
    return true;
}
static void put(uint8_t** p, uint64_t x, unsigned n) {
    for (unsigned i = n; i; --i)
        *(*p)++ = (uint8_t)(x >> ((i - 1) * 8));
}
static uint64_t get(const uint8_t** p, unsigned n) {
    uint64_t v = 0;
    while (n--)
        v = (v << 8) | *(*p)++;
    return v;
}
static bool nonzero(const uint8_t* p, size_t n) {
    unsigned v = 0;
    while (n--)
        v |= *p++;
    return v != 0;
}
static bool valid(const PcNetRankRecord* r) {
    PcNetRankSet state;
    if (!r || !r->ruleset_hash || !r->timestamp || !nonzero(r->match_id, 16) ||
        !nonzero(r->keys[0], 32) || !nonzero(r->keys[1], 32) ||
        !memcmp(r->keys[0], r->keys[1], 32) || !rating_valid(&r->pre[0]) ||
        !rating_valid(&r->pre[1]) || !pc_rank_set(r->games, r->game_count, &state) ||
        state.winner < 0)
        return false;
    for (unsigned i = 0; i < 2; i++) {
        if (nonzero(r->previous[i] + 20, 12) ||
            (r->pre[i].sets == 0 ? nonzero(r->previous[i], 20) : !nonzero(r->previous[i], 20)))
            return false;
    }
    for (unsigned i = r->game_count; i < PC_RANK_MAX_GAMES; ++i) {
        const PcNetRankGame* g = &r->games[i];
        if (g->winner || g->stocks[0] || g->stocks[1] || g->stage || g->frames)
            return false;
    }
    return true;
}
bool pc_rank_encode(const PcNetRankRecord* r, uint8_t out[PC_RANK_RECORD_BYTES]) {
    if (!out || !valid(r))
        return false;
    uint8_t* p = out;
    *p++ = 'M';
    *p++ = 'R';
    *p++ = 2;
    *p++ = r->game_count;
    memcpy(p, r->match_id, 16);
    p += 16;
    memcpy(p, r->keys, 64);
    p += 64;
    put(&p, r->ruleset_hash, 4);
    for (unsigned i = 0; i < 4; i++) {
        const PcNetRankGame* g = &r->games[i];
        *p++ = g->winner;
        *p++ = g->stocks[0];
        *p++ = g->stocks[1];
        *p++ = g->stage;
        put(&p, g->frames, 4);
    }
    for (unsigned i = 0; i < 2; i++) {
        uint64_t bits;
        memcpy(&bits, &r->pre[i].mu, 8);
        put(&p, bits, 8);
        memcpy(&bits, &r->pre[i].sigma, 8);
        put(&p, bits, 8);
        put(&p, r->pre[i].sets, 4);
    }
    memcpy(p, r->previous, 64);
    p += 64;
    put(&p, r->timestamp, 8);
    memcpy(p, r->signatures, 128);
    return true;
}
bool pc_rank_decode(PcNetRankRecord* out, const void* bytes, size_t length) {
    if (!out || !bytes || length != PC_RANK_RECORD_BYTES)
        return false;
    const uint8_t* p = bytes;
    if (*p++ != 'M' || *p++ != 'R' || *p++ != 2)
        return false;
    PcNetRankRecord r = {0};
    r.game_count = *p++;
    memcpy(r.match_id, p, 16);
    p += 16;
    memcpy(r.keys, p, 64);
    p += 64;
    r.ruleset_hash = (uint32_t)get(&p, 4);
    for (unsigned i = 0; i < 4; i++) {
        PcNetRankGame* g = &r.games[i];
        g->winner = *p++;
        g->stocks[0] = *p++;
        g->stocks[1] = *p++;
        g->stage = *p++;
        g->frames = (uint32_t)get(&p, 4);
    }
    for (unsigned i = 0; i < 2; i++) {
        uint64_t bits = get(&p, 8);
        memcpy(&r.pre[i].mu, &bits, 8);
        bits = get(&p, 8);
        memcpy(&r.pre[i].sigma, &bits, 8);
        r.pre[i].sets = (uint32_t)get(&p, 4);
    }
    memcpy(r.previous, p, 64);
    p += 64;
    r.timestamp = get(&p, 8);
    memcpy(r.signatures, p, 128);
    if (!valid(&r))
        return false;
    *out = r;
    return true;
}
bool pc_rank_sign(PcNetRankRecord* r, unsigned player, const PcNetIdentity* id) {
    uint8_t bytes[PC_RANK_RECORD_BYTES];
    if (player > 1 || !id || !r || memcmp(r->keys[player], id->public_key, 32) ||
        !pc_rank_encode(r, bytes))
        return false;
    pc_identity_sign(id, r->signatures[player], bytes, BODY_BYTES);
    return true;
}
bool pc_rank_verify(const PcNetRankRecord* r) {
    uint8_t bytes[PC_RANK_RECORD_BYTES];
    if (!pc_rank_encode(r, bytes))
        return false;
    return pc_identity_verify(r->keys[0], r->signatures[0], bytes, BODY_BYTES) &&
           pc_identity_verify(r->keys[1], r->signatures[1], bytes, BODY_BYTES);
}
bool pc_rank_apply(const PcNetRankRecord* r, PcNetRating post[2]) {
    PcNetRankSet state;
    return pc_rank_verify(r) && pc_rank_set(r->games, r->game_count, &state) &&
           pc_rank_update(r->pre, (unsigned)state.winner, post);
}
bool pc_rank_head(const PcNetRankRecord* r, uint8_t head[32]) {
    uint8_t bytes[PC_RANK_RECORD_BYTES];
    if (!head || !pc_rank_verify(r) || !pc_rank_encode(r, bytes))
        return false;
    SHA1_CTX hash;
    SHA1Init(&hash);
    static const uint8_t prefix[] = {'3', '6', '0', ':'};
    SHA1Update(&hash, prefix, sizeof prefix);
    SHA1Update(&hash, bytes, sizeof bytes);
    SHA1Final(head, &hash);
    memset(head + 20, 0, 12);
    return true;
}
