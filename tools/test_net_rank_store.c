/* Compile with net_rank_store.c, net_rank.c, net_identity.c and Monocypher,
 * -D_GNU_SOURCE -ffunction-sections -Wl,--gc-sections -lm. */
#include "net_rank_store.h"
#include "monocypher-ed25519.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static PcNetIdentity id[2];
static PcNetRankRecord record(PcNetRankStore* s, unsigned match) {
    PcNetRankRecord r = {0};
    r.match_id[0] = match;
    r.ruleset_hash = 123;
    r.timestamp = 456;
    for (unsigned i = 0; i < 2; i++) {
        memcpy(r.keys[i], id[i].public_key, 32);
        pc_rank_initial(&r.pre[i]);
    }
    assert(pc_rank_store_current(s, &r.pre[0], r.previous[0], NULL));
    r.game_count = 2;
    for (unsigned i = 0; i < 2; i++)
        r.games[i] = (PcNetRankGame){0, {1, 0}, 31, 1000};
    assert(pc_rank_sign(&r, 0, &id[0]) && pc_rank_sign(&r, 1, &id[1]));
    return r;
}
static void write_file(const char* p, const uint8_t* bytes, size_t n) {
    FILE* f = fopen(p, "wb");
    assert(f);
    assert(fwrite(bytes, 1, n, f) == n);
    assert(!fclose(f));
}
int main(void) {
    for (unsigned i = 0; i < 2; i++) {
        uint8_t seed[32] = {0};
        seed[0] = i + 1;
        crypto_ed25519_key_pair(id[i].secret_key, id[i].public_key, seed);
    }
    char dir[] = "/tmp/melee-rank-store-XXXXXX";
    assert(mkdtemp(dir));
    char path[256], temp[256], lock[256];
    snprintf(path, sizeof path, "%s/rank.history", dir);
    snprintf(temp, sizeof temp, "%s/rank.history.tmp", dir);
    snprintf(lock, sizeof lock, "%s/rank.history.lock", dir);
    PcNetRankStoreResult status;
    PcNetRankStore* s = pc_rank_store_open(dir, id[0].public_key, &status);
    assert(s && status == PC_RANK_STORE_OK);
    assert(!pc_rank_store_open(dir, id[0].public_key, &status)); /* lifetime writer lock */
    PcNetRankRecord a = record(s, 1);
    assert(pc_rank_store_append(s, &a) == PC_RANK_STORE_OK);
    PcNetRating before, after;
    uint8_t head[32], head2[32];
    uint32_t count;
    assert(pc_rank_store_current(s, &before, head, &count) && count == 1);
    assert(pc_rank_store_append(s, &a) == PC_RANK_STORE_INVALID);
    PcNetRankRecord b = record(s, 2), bad = b;
    bad.previous[0][0] ^= 1;
    assert(pc_rank_sign(&bad, 0, &id[0]) && pc_rank_sign(&bad, 1, &id[1]));
    assert(pc_rank_store_append(s, &bad) == PC_RANK_STORE_INVALID);
    bad = b;
    bad.match_id[0] = 1;
    assert(pc_rank_sign(&bad, 0, &id[0]) && pc_rank_sign(&bad, 1, &id[1]));
    assert(pc_rank_store_append(s, &bad) ==
           PC_RANK_STORE_INVALID); /* valid predecessor, duplicate id */
    bad = b;
    bad.pre[1].mu = 1000, bad.pre[1].sigma = 0.0001; /* fabricated genesis pre-rating */
    assert(pc_rank_sign(&bad, 0, &id[0]) && pc_rank_sign(&bad, 1, &id[1]));
    assert(pc_rank_store_append(s, &bad) == PC_RANK_STORE_INVALID);
    bad = b;
    bad.pre[0].mu += 1;
    assert(pc_rank_sign(&bad, 0, &id[0]) && pc_rank_sign(&bad, 1, &id[1]));
    assert(pc_rank_store_append(s, &bad) == PC_RANK_STORE_INVALID);
    bad = b;
    memset(bad.signatures[1], 0, 64);
    assert(pc_rank_store_append(s, &bad) == PC_RANK_STORE_INVALID);
    assert(!mkdir(temp, 0700)); /* deterministic write failure */
    assert(pc_rank_store_append(s, &b) == PC_RANK_STORE_IO);
    assert(pc_rank_store_current(s, &after, head2, &count) && count == 1);
    assert(before.mu == after.mu && before.sigma == after.sigma && !memcmp(head, head2, 32));
    assert(!rmdir(temp));
    pc_rank_store_close(s);
    s = pc_rank_store_open(dir, id[0].public_key, &status);
    assert(s);
    assert(pc_rank_store_current(s, &after, head2, &count) && count == 1);
    assert(before.mu == after.mu && before.sigma == after.sigma && !memcmp(head, head2, 32));
    assert(pc_rank_store_append(s, &b) == PC_RANK_STORE_OK);
    pc_rank_store_close(s);
    uint8_t bytes[44 + 2 * PC_RANK_RECORD_BYTES], mutated[sizeof bytes];
    FILE* f = fopen(path, "rb");
    assert(f);
    assert(fread(bytes, 1, sizeof bytes, f) == sizeof bytes && fgetc(f) == EOF);
    fclose(f);
    assert(!pc_rank_store_open(dir, id[1].public_key, &status) && status == PC_RANK_STORE_INVALID);
    for (unsigned mode = 0; mode < 5; mode++) {
        memcpy(mutated, bytes, sizeof bytes);
        size_t length = sizeof bytes;
        if (mode == 4)
            mutated[6] = 1; /* unreleased format1 explicitly rejected */
        if (mode == 0)
            mutated[100] ^= 1;
        if (mode == 1)
            length--; /* partial record */
        if (mode == 2)
            length -= PC_RANK_RECORD_BYTES; /* exact-boundary truncation */
        if (mode == 3) {
            memcpy(mutated + 44, bytes + 44 + PC_RANK_RECORD_BYTES, PC_RANK_RECORD_BYTES);
            memcpy(mutated + 44 + PC_RANK_RECORD_BYTES, bytes + 44, PC_RANK_RECORD_BYTES);
        }
        write_file(path, mutated, length);
        assert(
            !pc_rank_store_open(dir, id[0].public_key, &status) && status == PC_RANK_STORE_INVALID);
        f = fopen(path, "rb");
        assert(f);
        assert(!fseek(f, 0, SEEK_END));
        assert(ftell(f) == (long)length);
        fclose(f);
    }
    write_file(path, bytes, sizeof bytes);
    s = pc_rank_store_open(dir, id[0].public_key, &status);
    assert(s);
    assert(pc_rank_store_current(s, &after, head2, &count) && count == 2);
    pc_rank_store_close(s);
    unlink(path);
    unlink(lock);
    rmdir(dir);
    puts("PASS: durable restart, signatures/chain/key/duplicate validation, failed-write state "
         "preservation, corruption/truncation/reordering rejection");
}
