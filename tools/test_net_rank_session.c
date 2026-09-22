/* Two isolated session states, actual codec/crypto/durable store, fake reliable
 * datagrams. Compile as GNU C with rank/store/identity + Monocypher (do not
 * separately compile net_rank_session.c; its state is explicitly swapped). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "monocypher-ed25519.h"
#include "../src/pc/net_rank_session.c"
typedef __typeof__(session) Session;
static Session peers[2];
static unsigned current;
static bool hold_saved;
static struct {
    uint8_t type, data[256];
    int size;
} queue[2][128];
static unsigned read_at[2], write_at[2];
bool pc_net_active(void) {
    return true;
}
bool pc_net_resim(void) {
    return false;
}
bool pc_net_send_reliable(uint8_t type, const void* data, int size) {
    if (type == MSG_SAVED && hold_saved)
        return false;
    assert(size <= 256 && write_at[1 - current] < 128);
    unsigned i = write_at[1 - current]++;
    queue[1 - current][i].type = type;
    queue[1 - current][i].size = size;
    memcpy(queue[1 - current][i].data, data, size);
    return true;
}
int pc_net_recv_reliable(uint8_t* type, void* out, int max) {
    if (read_at[current] == write_at[current])
        return -1;
    unsigned i = read_at[current]++;
    assert(queue[current][i].size <= max);
    *type = queue[current][i].type;
    memcpy(out, queue[current][i].data, queue[current][i].size);
    return queue[current][i].size;
}
static void enter(unsigned p) {
    current = p;
    session = peers[p];
}
static void leave(void) {
    peers[current] = session;
}
static void poll(void) {
    for (unsigned n = 0; n < 5; n++)
        for (unsigned p = 0; p < 2; p++) {
            enter(p);
            pc_rank_session_poll();
            leave();
        }
}
static void close_all(char dirs[2][64]) {
    for (unsigned p = 0; p < 2; p++) {
        enter(p);
        pc_rank_session_stop();
        leave();
        char path[128];
        snprintf(path, sizeof path, "%s/rank.history", dirs[p]);
        unlink(path);
        snprintf(path, sizeof path, "%s/rank.history.lock", dirs[p]);
        unlink(path);
        rmdir(dirs[p]);
    }
}
static void begin_all(PcNetIdentity ids[2], char dirs[2][64], bool proof) {
    memset(peers, 0, sizeof peers);
    memset(read_at, 0, sizeof read_at);
    memset(write_at, 0, sizeof write_at);
    for (unsigned p = 0; p < 2; p++) {
        snprintf(dirs[p], 64, "/tmp/rank-session-%u-XXXXXX", p);
        assert(mkdtemp(dirs[p]));
        enter(p);
        assert(pc_rank_session_begin(dirs[p], &ids[p], ids[1 - p].public_key, p, 12345));
        if (proof) {
            uint8_t value[52];
            pc_rank_session_initial_public(value);
            assert(pc_rank_session_proof(0, value, 52, 0));
            assert(pc_rank_session_proof(1, value, 52, 0));
        }
        leave();
    }
    poll();
}
static void play(unsigned winner) {
    for (unsigned p = 0; p < 2; p++) {
        enter(p);
        pc_rank_session_stage_begin();
        if (!pc_rank_session_stage()) {
            int chooser = pc_rank_session_stage_port();
            assert(!pc_rank_session_choose_stage(1 - chooser, 2));
            assert(!pc_rank_session_choose_stage(chooser, 4));
            assert(pc_rank_session_choose_stage(chooser, 2));
            assert(!pc_rank_session_choose_stage(chooser, 2));
            assert(pc_rank_session_choose_stage(chooser, 3));
            assert(pc_rank_session_stage_port() == 1 - chooser);
            assert(pc_rank_session_choose_stage(1 - chooser, 31));
        }
        assert(pc_rank_session_game(
            winner, winner == 0 ? 1 : 0, winner == 1 ? 1 : 0, pc_rank_session_stage(), 1000));
        leave();
    }
}
/* Two-set signed history for identity b (slot 1) against accomplice a (slot 0).
 * With honest genesis both records carry the canonical initial pre-rating;
 * otherwise the first record claims a fabricated genesis pre-rating. */
static void ancestry_chain(
    PcNetRankRecord chain[2], const PcNetIdentity* a, const PcNetIdentity* b, bool honest) {
    memset(chain, 0, 2 * sizeof *chain);
    PcNetRankRecord* first = &chain[0], *second = &chain[1];
    for (unsigned i = 0; i < 2; i++) {
        memcpy(first->keys[i], i ? b->public_key : a->public_key, 32);
        memcpy(second->keys[i], i ? b->public_key : a->public_key, 32);
    }
    first->match_id[0] = 98;
    first->ruleset_hash = PC_RANK_RULESET_ID;
    first->timestamp = 1234;
    pc_rank_initial(&first->pre[0]);
    if (honest)
        pc_rank_initial(&first->pre[1]);
    else
        first->pre[1] = (PcNetRating){1000, 0.0001, 0};
    first->game_count = 2;
    first->games[0] = (PcNetRankGame){1, {0, 1}, 2, 1000};
    first->games[1] = first->games[0];
    PcNetRating post[2];
    uint8_t head[32];
    assert(pc_rank_sign(first, 0, a) && pc_rank_sign(first, 1, b));
    assert(pc_rank_apply(first, post) && pc_rank_head(first, head));
    second->pre[0] = post[0];
    second->pre[1] = post[1];
    memcpy(second->previous[0], head, 32);
    memcpy(second->previous[1], head, 32);
    second->match_id[0] = 99;
    second->ruleset_hash = PC_RANK_RULESET_ID;
    second->timestamp = 1235;
    second->game_count = 2;
    second->games[0] = (PcNetRankGame){1, {0, 1}, 2, 1000};
    second->games[1] = second->games[0];
    assert(pc_rank_sign(second, 0, a) && pc_rank_sign(second, 1, b));
}
int main(void) {
    PcNetIdentity ids[2] = {0};
    for (unsigned p = 0; p < 2; p++) {
        uint8_t seed[32] = {0};
        seed[0] = p + 1;
        crypto_ed25519_key_pair(ids[p].secret_key, ids[p].public_key, seed);
    }
    char dirs[2][64];
    begin_all(ids, dirs, false);
    assert(peers[0].state == PC_RANK_SESSION_WAIT && peers[1].state == PC_RANK_SESSION_WAIT);
    close_all(dirs);
    begin_all(ids, dirs, true);
    assert(peers[0].state == PC_RANK_SESSION_PLAY && peers[1].state == PC_RANK_SESSION_PLAY);
    assert(!memcmp(peers[0].record.match_id, peers[1].record.match_id, 16));
    play(0);
    assert(peers[0].state == PC_RANK_SESSION_PLAY);
    play(0);
    assert(peers[0].state == PC_RANK_SESSION_SIGN);
    hold_saved = true;
    poll();
    assert(peers[0].state == PC_RANK_SESSION_SIGN && peers[1].state == PC_RANK_SESSION_SIGN);
    assert(peers[0].appended && peers[1].appended);
    for (unsigned p = 0; p < 2; p++) {
        enter(p);
        assert(pc_rank_session_set_complete());
        PcNetRating r;
        assert(pc_rank_store_current(session.store, &r, NULL, NULL));
        assert(r.sets == 1);
        leave();
    }
    hold_saved = false;
    poll();
    assert(peers[0].state == PC_RANK_SESSION_SAVED && peers[1].state == PC_RANK_SESSION_SAVED);
    uint8_t wire[2][360];
    for (unsigned p = 0; p < 2; p++) {
        enter(p);
        assert(pc_rank_encode(pc_rank_session_record(), wire[p]));
        PcNetRating rating;
        assert(pc_rank_store_current(session.store, &rating, NULL, NULL) && rating.sets == 1);
        assert(p == 0 ? rating.mu > 25 : rating.mu < 25);
        leave();
    }
    assert(!memcmp(wire[0], wire[1], 360));
    uint8_t peer_current[52], genesis[52];
    enter(1);
    assert(pc_rank_session_public_state(peer_current));
    leave();
    enter(0);
    assert(pc_rank_session_begin(dirs[0], &ids[0], ids[1].public_key, 0, 999));
    pc_rank_session_initial_public(genesis);
    assert(!pc_rank_session_proof(1, genesis, 52, 0));
    assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    assert(pc_rank_session_begin(dirs[0], &ids[0], ids[1].public_key, 0, 999));
    peer_current[20] ^= 1;
    assert(!pc_rank_session_proof(1, peer_current, 52, 1));
    assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    peer_current[20] ^= 1;
    assert(pc_rank_session_begin(dirs[0], &ids[0], ids[1].public_key, 0, 999));
    assert(pc_rank_session_proof(1, peer_current, 52, 1));
    PcNetIdentity other = {0};
    uint8_t other_seed[32] = {77};
    crypto_ed25519_key_pair(other.secret_key, other.public_key, other_seed);
    PcNetRankRecord successor = {0};
    memcpy(successor.keys[0], other.public_key, 32);
    memcpy(successor.keys[1], ids[1].public_key, 32);
    successor.match_id[0] = 99;
    successor.ruleset_hash = PC_RANK_RULESET_ID;
    successor.timestamp = 1234;
    pc_rank_initial(&successor.pre[0]);
    successor.pre[1] = session.known_peer_rating;
    memcpy(successor.previous[1], session.known_peer_head, 32);
    successor.game_count = 2;
    successor.games[0] = (PcNetRankGame){0, {1, 0}, 2, 1000};
    successor.games[1] = successor.games[0];
    for (unsigned branch = 0; branch < 2; branch++) {
        if (branch)
            successor.previous[1][0] ^= 1;
        assert(pc_rank_sign(&successor, 0, &other) && pc_rank_sign(&successor, 1, &ids[1]));
        PcNetRating post[2];
        uint8_t head[32], encoded[360], proof[52];
        assert(pc_rank_apply(&successor, post) && pc_rank_head(&successor, head) &&
               pc_rank_encode(&successor, encoded));
        public_encode(proof, &post[1], head);
        assert(pc_rank_session_begin(dirs[0], &ids[0], ids[1].public_key, 0, 999));
        assert(pc_rank_session_proof(1, proof, 52, 2));
        assert(pc_rank_session_chain_target(NULL));
        assert(pc_rank_session_chain_record(encoded, sizeof encoded) == !branch);
        if (!branch)
            assert(!pc_rank_session_chain_target(NULL));
        else
            assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    }
    assert(pc_rank_session_begin(dirs[0], &ids[0], ids[1].public_key, 0, 999));
    uint8_t excessive[52];
    PcNetRating far = session.known_peer_rating;
    far.sets += 65;
    public_encode(excessive, &far, session.known_peer_head);
    assert(!pc_rank_session_proof(1, excessive, 52, far.sets));
    assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    /* RANK-CLAIMED-PRE: unknown peers are no longer trusted by default. A
     * claimed fresh identity (sets == 0) must carry exactly the initial
     * Weng-Lin state, and any history must walk a full hash chain to genesis. */
    uint8_t unknown_proof[52], zero_head[32] = {0}, deep_head[32] = {1};
    assert(pc_rank_session_begin(dirs[0], &ids[0], other.public_key, 0, 777));
    PcNetRating lie = {1000, 0.0001, 0};
    public_encode(unknown_proof, &lie, zero_head);
    assert(!pc_rank_session_proof(1, unknown_proof, 52, 0));
    assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    PcNetRating deep = {25, 25.0 / 3, 65};
    public_encode(unknown_proof, &deep, deep_head);
    assert(pc_rank_session_begin(dirs[0], &ids[0], other.public_key, 0, 778));
    assert(!pc_rank_session_proof(1, unknown_proof, 52, 65));
    assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    PcNetIdentity accomplice = {0};
    uint8_t accomplice_seed[32] = {78};
    crypto_ed25519_key_pair(accomplice.secret_key, accomplice.public_key, accomplice_seed);
    PcNetRankRecord chain[2];
    PcNetRating chain_post[2][2];
    uint8_t chain_wire[2][360], chain_head[2][32], remote_proof[52];
    ancestry_chain(chain, &accomplice, &other, true);
    for (unsigned i = 0; i < 2; i++) {
        assert(pc_rank_apply(&chain[i], chain_post[i]));
        assert(pc_rank_head(&chain[i], chain_head[i]));
        assert(pc_rank_encode(&chain[i], chain_wire[i]));
    }
    public_encode(remote_proof, &chain_post[1][1], chain_head[1]);
    assert(pc_rank_session_begin(dirs[0], &ids[0], other.public_key, 0, 779));
    assert(pc_rank_session_proof(1, remote_proof, 52, 2));
    assert(pc_rank_session_chain_target(NULL));
    assert(pc_rank_session_chain_record(chain_wire[1], sizeof chain_wire[1]));
    assert(pc_rank_session_chain_target(NULL));
    assert(pc_rank_session_chain_record(chain_wire[0], sizeof chain_wire[0]));
    assert(!pc_rank_session_chain_target(NULL));
    assert(session.chain_verified && !session.chain_pending &&
           pc_rank_session_state(NULL) == PC_RANK_SESSION_WAIT);
    ancestry_chain(chain, &accomplice, &other, false);
    for (unsigned i = 0; i < 2; i++) {
        assert(pc_rank_apply(&chain[i], chain_post[i]));
        assert(pc_rank_head(&chain[i], chain_head[i]));
        assert(pc_rank_encode(&chain[i], chain_wire[i]));
    }
    public_encode(remote_proof, &chain_post[1][1], chain_head[1]);
    assert(pc_rank_session_begin(dirs[0], &ids[0], other.public_key, 0, 780));
    assert(pc_rank_session_proof(1, remote_proof, 52, 2));
    assert(pc_rank_session_chain_target(NULL));
    assert(pc_rank_session_chain_record(chain_wire[1], sizeof chain_wire[1]));
    assert(!pc_rank_session_chain_record(chain_wire[0], sizeof chain_wire[0]));
    assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    assert(!pc_rank_session_chain_target(NULL));
    leave();
    close_all(dirs);
    begin_all(ids, dirs, true);
    play(0);
    play(0);
    /* Both sets valid independently, but different outcome metadata must stop signing. */
    peers[1].record.games[1].frames++;
    poll();
    assert(peers[0].state == PC_RANK_SESSION_FAILED && peers[1].state == PC_RANK_SESSION_FAILED);
    for (unsigned p = 0; p < 2; p++) {
        PcNetRating rating;
        assert(pc_rank_store_current(peers[p].store, &rating, NULL, NULL) && rating.sets == 0);
    }
    close_all(dirs);
    begin_all(ids, dirs, true);
    for (unsigned round = 0; round < 4; round++)
        for (unsigned p = 0; p < 2; p++) {
            enter(p);
            pc_rank_session_stage_begin();
            bool ok = pc_rank_session_game(PC_RANK_TIE, 1, 1, pc_rank_session_stage(), 1000);
            assert(ok == (round < 3));
            leave();
        }
    assert(peers[0].state == PC_RANK_SESSION_FAILED && peers[1].state == PC_RANK_SESSION_FAILED);
    close_all(dirs);
    puts("PASS: signed proof barrier, synced bans/loser picks, identical dual signatures, durable "
         "ratings, known-opponent rollback/fork refusal and bounded ancestry, unknown-peer genesis "
         "pin and full ancestry verification, mismatched records and repeated ties fail closed");
}
