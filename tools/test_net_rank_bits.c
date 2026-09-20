/* Cross-platform oracle: compare stdout byte-for-byte after CRLF normalization.
 * Exercises 4096 rating updates and deterministic signed record encodings. */
#include "net_rank.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef PC_RANK_BITS_STANDALONE
#include "../extern/dht/sha1.h"
void pc_dht_sha1(const void* data, size_t len, unsigned char out[20]) {
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, data, (uint32_t)len);
    SHA1Final(out, &ctx);
}
#endif
static void hex(const void* data, size_t n) {
    const unsigned char* p = data;
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}
static void number(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    printf("%016llx", (unsigned long long)bits);
}
int main(void) {
    PcNetIdentity id[2] = {{0}};
    for (unsigned i = 0; i < 2; i++) {
        uint8_t seed[32] = {0};
        seed[0] = (uint8_t)(i + 1);
        crypto_ed25519_key_pair(id[i].secret_key, id[i].public_key, seed);
    }
    PcNetRating pre[2];
    pc_rank_initial(&pre[0]);
    pc_rank_initial(&pre[1]);
    for (unsigned iteration = 0; iteration < 4096; iteration++) {
        if (iteration == 1024) {
            pre[0] = (PcNetRating){-500, 0.01, 10};
            pre[1] = (PcNetRating){500, 100, 10};
        }
        if (iteration == 2048) {
            pre[0] = (PcNetRating){-15, 0.5, 300};
            pre[1] = (PcNetRating){12, 23, 700};
        }
        if (iteration == 3072) {
            pre[0] = (PcNetRating){40, 0.001, 12000};
            pre[1] = (PcNetRating){40, 0.001, 12000};
        }
        unsigned winner = ((iteration * 1664525u + 1013904223u) >> 19) & 1;
        PcNetRating post[2];
        if (!pc_rank_update(pre, winner, post))
            return 1;
        for (unsigned i = 0; i < 2; i++) {
            number(post[i].mu);
            number(post[i].sigma);
            number(pc_rank_display(&post[i]));
            printf("%08x", post[i].sets);
        }
        putchar('\n');
        if (iteration % 128 == 0) {
            PcNetRankRecord record = {0};
            record.match_id[0] = 1;
            record.match_id[1] = (uint8_t)(iteration / 128);
            record.ruleset_hash = 0x12345678;
            record.timestamp = 12345 + iteration;
            record.game_count = 2;
            for (unsigned i = 0; i < 2; i++) {
                memcpy(record.keys[i], id[i].public_key, 32);
                record.pre[i] = pre[i];
                /* Format 2 uses a 20-byte immutable locator padded to 32 bytes. */
                if (pre[i].sets) {
                    record.previous[i][0] = (uint8_t)(i + 1);
                    record.previous[i][19] = (uint8_t)(iteration / 128);
                }
                record.games[i] = (PcNetRankGame){(uint8_t)winner, {0, 0}, 31, 1000};
                record.games[i].stocks[winner] = 1;
            }
            if (!pc_rank_sign(&record, 0, &id[0]) || !pc_rank_sign(&record, 1, &id[1]))
                return 2;
            uint8_t wire[PC_RANK_RECORD_BYTES];
            if (!pc_rank_encode(&record, wire) || !pc_rank_verify(&record))
                return 3;
            hex(wire, sizeof wire);
            putchar('\n');
            uint8_t head[32];
            if (!pc_rank_head(&record, head))
                return 4;
            hex(head, sizeof head);
            putchar('\n');
        }
        memcpy(pre, post, sizeof pre);
    }
    return 0;
}
