/* cc -std=c11 -D_GNU_SOURCE -ffunction-sections -fdata-sections -Isrc/pc
 * -Iextern/monocypher tools/test_net_rank.c src/pc/net_rank.c
 * src/pc/net_identity.c extern/monocypher/monocypher{,-ed25519}.c
 * -Wl,--gc-sections -lm -o /tmp/test_net_rank && /tmp/test_net_rank */
#include "net_rank.h"
#include "monocypher-ed25519.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    PcNetIdentity identities[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        uint8_t seed[32] = {0};
        seed[0] = i + 1;
        crypto_ed25519_key_pair(identities[i].secret_key, identities[i].public_key, seed);
    }
    PcNetRankRecord r = {0};
    r.match_id[0] = 1;
    r.ruleset_hash = 0x12345678;
    r.timestamp = 12345;
    for (unsigned i = 0; i < 2; i++) {
        memcpy(r.keys[i], identities[i].public_key, 32);
        pc_rank_initial(&r.pre[i]);
    }
    PcNetRating post[2], swapped[2];
    assert(pc_rank_display(&r.pre[0]) == 1000);
    assert(pc_rank_update(r.pre, 0, post));
    assert(fabs(post[0].mu - 27.635389493140497) < 1e-10);
    assert(post[0].mu > 25 && post[1].mu < 25);
    assert(post[0].sigma < r.pre[0].sigma && post[0].sets == 1);
    assert(pc_rank_update(r.pre, 1, swapped));
    assert(post[0].mu == swapped[1].mu && post[0].sigma == swapped[1].sigma);
    assert(!pc_rank_update(r.pre, 2, post));
    PcNetRating invalid[2] = {r.pre[0], r.pre[1]};
    invalid[0].sigma = NAN;
    assert(!pc_rank_update(invalid, 0, post));
    r.games[0] = (PcNetRankGame){0, {2, 0}, 31, 1000};
    r.game_count = 1;
    PcNetRankSet set;
    assert(pc_rank_set(r.games, 1, &set) && set.winner == -1 && set.chooser == 1);
    assert(!pc_rank_sign(&r, 0, &identities[0])); /* not a finished Bo3 */
    r.games[1] = (PcNetRankGame){1, {0, 1}, 2, 2000};
    r.games[2] = (PcNetRankGame){0, {1, 0}, 3, 3000};
    r.game_count = 3;
    assert(pc_rank_set(r.games, 3, &set) && set.winner == 0);
    assert(!pc_rank_sign(&r, 0, &identities[1]));
    assert(pc_rank_sign(&r, 0, &identities[0]) && !pc_rank_verify(&r));
    assert(pc_rank_sign(&r, 1, &identities[1]) && pc_rank_verify(&r));
    assert(pc_rank_apply(&r, post));
    uint8_t wire[PC_RANK_RECORD_BYTES], other[PC_RANK_RECORD_BYTES], head[32];
    assert(pc_rank_encode(&r, wire));
    assert(wire[84] == 0x12 && wire[87] == 0x78); /* canonical big endian */
    assert(pc_rank_head(&r, head));
    for (unsigned i = 20; i < 32; i++)
        assert(head[i] == 0);
    assert(wire[2] == 2);
    wire[2] = 1;
    PcNetRankRecord old;
    assert(!pc_rank_decode(&old, wire, sizeof wire));
    wire[2] = 2;
    PcNetRankRecord noncanonical = r;
    noncanonical.previous[0][20] = 1;
    assert(!pc_rank_encode(&noncanonical, other));
    PcNetRankRecord decoded;
    assert(pc_rank_decode(&decoded, wire, sizeof wire));
    assert(pc_rank_verify(&decoded));
    assert(pc_rank_encode(&decoded, other) && !memcmp(wire, other, sizeof wire));
    assert(!pc_rank_decode(&decoded, wire, sizeof wire - 1));
    /* Every bit in every byte is authenticated, including unused game slots. */
    for (size_t i = 0; i < sizeof wire; i++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            memcpy(other, wire, sizeof wire);
            other[i] ^= 1u << bit;
            assert(!pc_rank_decode(&decoded, other, sizeof other) || !pc_rank_verify(&decoded));
        }
    }
    r.games[3] = (PcNetRankGame){1, {0, 1}, 31, 100};
    r.game_count = 4;
    assert(!pc_rank_set(r.games, 4, &set)); /* no games after set won */
    r.games[0] = (PcNetRankGame){PC_RANK_TIE, {1, 1}, 31, 28800};
    r.games[1] = (PcNetRankGame){0, {1, 0}, 31, 1000};
    assert(pc_rank_set(r.games, 1, &set) && set.tiebreak);
    assert(pc_rank_set(r.games, 2, &set) && !set.tiebreak);
    r.games[1].stage = 2;
    assert(!pc_rank_set(r.games, 2, &set));
    r.games[1].stage = 31;
    r.games[1].stocks[0] = 2;
    assert(!pc_rank_set(r.games, 2, &set));
    puts("PASS: Weng-Lin rating, Bo3/tiebreak validation, canonical codec, two signatures, 2880 "
         "bit flips");
}
