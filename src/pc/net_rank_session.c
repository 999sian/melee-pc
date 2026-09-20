/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_rank_session.h"
#include "net.h"
#include "net_lan.h"
#include "monocypher.h"
#include <math.h>
#include <string.h>
#include <time.h>
#define MSG_HELLO 0x60
#define MSG_BODY 0x61
#define MSG_SIGNATURE 0x62
#define MSG_ABORT 0x63
#define MSG_SAVED 0x64
#define HELLO_BODY 152
#define HELLO_SIZE (HELLO_BODY + 64)
#define RECORD_BODY (PC_RANK_RECORD_BYTES - 128)
static struct {
    int state;
    const char* reason;
    const PcNetIdentity* identity;
    PcNetRankStore* store;
    PcNetRankRecord record;
    PcNetRankSet set;
    unsigned local, stage, ban_count, bans[2];
    uint32_t seed;
    bool known_peer;
    PcNetRating known_peer_rating;
    uint8_t known_peer_head[32];
    bool chain_verified, chain_pending;
    PcNetRating chain_rating;
    uint8_t chain_head[32];
    unsigned chain_steps;
    uint8_t hello[HELLO_SIZE], nonce[2][16], proof[2][PC_RANK_PUBLIC_BYTES];
    bool sent_hello, got_hello, got_proof[2], stage_started;
    bool sent_body, got_body, sent_signature, got_signature, sent_abort;
    bool appended, sent_saved, got_saved;
    uint8_t peer_saved[32];
    uint8_t peer_body[RECORD_BODY], peer_signature[96];
    time_t deadline;
} session;
static void put(uint8_t** p, uint64_t v, unsigned n) {
    for (unsigned i = n; i; i--)
        *(*p)++ = (uint8_t)(v >> ((i - 1) * 8));
}
static uint64_t get(const uint8_t** p, unsigned n) {
    uint64_t v = 0;
    while (n--)
        v = (v << 8) | *(*p)++;
    return v;
}
static void public_encode(uint8_t out[52], const PcNetRating* r, const uint8_t head[32]) {
    uint8_t* p = out;
    uint64_t bits;
    memcpy(&bits, &r->mu, 8);
    put(&p, bits, 8);
    memcpy(&bits, &r->sigma, 8);
    put(&p, bits, 8);
    put(&p, r->sets, 4);
    memcpy(p, head, 32);
}
static bool public_decode(const uint8_t in[52], PcNetRating* r, uint8_t head[32]) {
    const uint8_t* p = in;
    uint64_t bits = get(&p, 8);
    memcpy(&r->mu, &bits, 8);
    bits = get(&p, 8);
    memcpy(&r->sigma, &bits, 8);
    r->sets = (uint32_t)get(&p, 4);
    memcpy(head, p, 32);
    unsigned tail = 0, prefix = 0;
    for (unsigned i = 0; i < 20; i++)
        prefix |= head[i];
    for (unsigned i = 20; i < 32; i++)
        tail |= head[i];
    if (tail || (r->sets == 0 ? prefix != 0 : prefix == 0))
        return false;
    return isfinite(r->mu) && isfinite(r->sigma) && r->mu >= -1000 && r->mu <= 1000 &&
           r->sigma > 0 && r->sigma <= 100 && r->sets < UINT32_MAX;
}
void pc_rank_session_abort(const char* reason) {
    if (session.state == PC_RANK_SESSION_OFF)
        return;
    if (session.appended) {
        session.state = PC_RANK_SESSION_SAVED;
        session.reason = "set saved locally; peer save confirmation unavailable";
    } else {
        session.state = PC_RANK_SESSION_FAILED;
        session.reason = reason ? reason : "ranked session failed";
    }
}
void pc_rank_session_stop(void) {
    pc_rank_store_close(session.store);
    memset(&session, 0, sizeof session);
}
void pc_rank_session_initial_public(uint8_t out[52]) {
    PcNetRating rating;
    uint8_t head[32] = {0};
    pc_rank_initial(&rating);
    public_encode(out, &rating, head);
}
bool pc_rank_session_public_state(uint8_t out[52]) {
    PcNetRating rating;
    uint8_t head[32];
    if (!out || !pc_rank_store_current(session.store, &rating, head, NULL))
        return false;
    public_encode(out, &rating, head);
    return true;
}
bool pc_rank_session_begin(const char* directory, const PcNetIdentity* id,
    const uint8_t peer_key[32], unsigned local, uint32_t seed) {
    pc_rank_session_stop();
    session.state = PC_RANK_SESSION_WAIT;
    if (!id || !peer_key || local > 1 || !memcmp(id->public_key, peer_key, 32)) {
        pc_rank_session_abort("invalid ranked identities");
        return false;
    }
    PcNetRankStoreResult result;
    session.store = pc_rank_store_open(directory, id->public_key, &result);
    if (!session.store) {
        pc_rank_session_abort("rank history unavailable or damaged");
        return false;
    }
    session.known_peer = pc_rank_store_peer_state(
        session.store, peer_key, &session.known_peer_rating, session.known_peer_head);
    session.chain_verified = !session.known_peer;
    session.identity = id;
    session.local = local;
    session.seed = seed;
    memcpy(session.record.keys[local], id->public_key, 32);
    memcpy(session.record.keys[1 - local], peer_key, 32);
    session.record.ruleset_hash = PC_RANK_RULESET_ID;
    pc_rank_store_current(
        session.store, &session.record.pre[local], session.record.previous[local], NULL);
    if (!pc_identity_random(session.nonce[local], 16)) {
        pc_rank_session_abort("ranked nonce unavailable");
        return false;
    }
    uint8_t* p = session.hello;
    memcpy(p, "RSH2", 4);
    p += 4;
    put(&p, seed, 4);
    *p++ = (uint8_t)local;
    p += 3;
    memcpy(p, session.nonce[local], 16);
    p += 16;
    session.record.timestamp = (uint64_t)time(NULL);
    put(&p, session.record.timestamp, 8);
    public_encode(p, &session.record.pre[local], session.record.previous[local]);
    p += 52;
    memcpy(p, session.record.keys, 64);
    p += 64;
    pc_identity_sign(id, p, session.hello, HELLO_BODY);
    pc_rank_set(NULL, 0, &session.set);
    session.deadline = time(NULL) + 120;
    session.reason = "waiting for signed rating and verified public history";
    return true;
}
static void check_ready(void) {
    if (session.state != PC_RANK_SESSION_WAIT || !session.got_hello || !session.got_proof[0] ||
        !session.got_proof[1] || !session.chain_verified)
        return;
    for (unsigned i = 0; i < 2; i++) {
        uint8_t expected[52];
        public_encode(expected, &session.record.pre[i], session.record.previous[i]);
        if (memcmp(expected, session.proof[i], 52)) {
            pc_rank_session_abort("published rating differs from signed starting history");
            return;
        }
    }
    uint8_t material[100], *p = material;
    memcpy(p, session.record.keys, 64);
    p += 64;
    memcpy(p, session.nonce, 32);
    p += 32;
    put(&p, session.seed, 4);
    crypto_blake2b(session.record.match_id, 16, material, sizeof material);
    session.state = PC_RANK_SESSION_PLAY;
    session.reason = "community rating: published starting state matched";
}
bool pc_rank_session_proof(unsigned player, const void* value, size_t size, int64_t sequence) {
    PcNetRating rating;
    uint8_t head[32];
    if (session.state != PC_RANK_SESSION_WAIT || player > 1 || !value || size != 52 ||
        !public_decode(value, &rating, head) || sequence != (int64_t)rating.sets)
        return false;
    if (player != session.local && session.known_peer &&
        (rating.sets < session.known_peer_rating.sets ||
            (rating.sets == session.known_peer_rating.sets &&
                (memcmp(&rating.mu, &session.known_peer_rating.mu, 8) ||
                    memcmp(&rating.sigma, &session.known_peer_rating.sigma, 8) ||
                    memcmp(head, session.known_peer_head, 32)))))
    {
        pc_rank_session_abort("peer public history omits a locally held signed set");
        return false;
    }
    if (player != session.local && session.known_peer) {
        if (rating.sets == session.known_peer_rating.sets)
            session.chain_verified = true;
        else if (rating.sets - session.known_peer_rating.sets > 64) {
            pc_rank_session_abort("peer ancestry exceeds bounded verification limit");
            return false;
        } else {
            session.chain_verified = false;
            session.chain_pending = true;
            session.chain_rating = rating;
            memcpy(session.chain_head, head, 32);
            session.chain_steps = 0;
        }
    }
    if (session.got_proof[player] && memcmp(session.proof[player], value, 52)) {
        pc_rank_session_abort("conflicting public rank proofs");
        return false;
    }
    memcpy(session.proof[player], value, 52);
    session.got_proof[player] = true;
    check_ready();
    return true;
}
bool pc_rank_session_chain_target(uint8_t target[20]) {
    if (session.state != PC_RANK_SESSION_WAIT || !session.chain_pending)
        return false;
    if (target)
        memcpy(target, session.chain_head, 20);
    return true;
}
bool pc_rank_session_chain_record(const void* value, size_t size) {
    PcNetRankRecord record;
    PcNetRating post[2];
    uint8_t head[32];
    unsigned peer = 1 - session.local;
    if (!session.chain_pending || session.state != PC_RANK_SESSION_WAIT ||
        ++session.chain_steps > 64 || !pc_rank_decode(&record, value, size) ||
        !pc_rank_head(&record, head) || memcmp(head, session.chain_head, 32) ||
        !pc_rank_apply(&record, post))
        goto invalid;
    int player = !memcmp(record.keys[0], session.record.keys[peer], 32) ? 0 :
                 !memcmp(record.keys[1], session.record.keys[peer], 32) ? 1 :
                                                                          -1;
    if (player < 0 || post[player].sets != session.chain_rating.sets ||
        memcmp(&post[player].mu, &session.chain_rating.mu, 8) ||
        memcmp(&post[player].sigma, &session.chain_rating.sigma, 8))
        goto invalid;
    session.chain_rating = record.pre[player];
    memcpy(session.chain_head, record.previous[player], 32);
    if (session.chain_rating.sets < session.known_peer_rating.sets)
        goto invalid;
    if (session.chain_rating.sets == session.known_peer_rating.sets) {
        if (memcmp(session.chain_head, session.known_peer_head, 32) ||
            memcmp(&session.chain_rating.mu, &session.known_peer_rating.mu, 8) ||
            memcmp(&session.chain_rating.sigma, &session.known_peer_rating.sigma, 8))
            goto invalid;
        session.chain_pending = false;
        session.chain_verified = true;
        check_ready();
    }
    return true;
invalid:
    pc_rank_session_abort("peer ancestry omits or contradicts a locally held signed set");
    return false;
}
static void hello_receive(const uint8_t* data, int size) {
    unsigned peer = 1 - session.local;
    if (session.state != PC_RANK_SESSION_WAIT || session.got_hello)
        return;
    if (size != HELLO_SIZE || memcmp(data, "RSH2", 4) || data[8] != peer || data[9] || data[10] ||
        data[11] || memcmp(data + 88, session.record.keys, 64) ||
        !pc_identity_verify(session.record.keys[peer], data + HELLO_BODY, data, HELLO_BODY))
    {
        pc_rank_session_abort("invalid signed ranked hello");
        return;
    }
    const uint8_t* p = data + 4;
    if (get(&p, 4) != session.seed ||
        !public_decode(data + 36, &session.record.pre[peer], session.record.previous[peer]))
    {
        pc_rank_session_abort("invalid ranked starting state");
        return;
    }
    memcpy(session.nonce[peer], data + 12, 16);
    p = data + 28;
    uint64_t timestamp = get(&p, 8);
    if (!timestamp) {
        pc_rank_session_abort("invalid ranked timestamp");
        return;
    }
    if (peer == 0)
        session.record.timestamp = timestamp;
    session.got_hello = true;
    check_ready();
}
static void complete(void) {
    uint8_t wire[PC_RANK_RECORD_BYTES], hash[32];
    if (session.state != PC_RANK_SESSION_SIGN || !pc_rank_encode(&session.record, wire))
        return;
    if (!session.sent_body)
        session.sent_body = pc_net_send_reliable(MSG_BODY, wire, RECORD_BODY);
    if (!session.got_body)
        return;
    if (memcmp(session.peer_body, wire, RECORD_BODY)) {
        pc_rank_session_abort("peers disagree on ranked set record");
        return;
    }
    if (!session.sent_signature) {
        uint8_t message[96];
        crypto_blake2b(message, 32, wire, RECORD_BODY);
        if (!pc_rank_sign(&session.record, session.local, session.identity)) {
            pc_rank_session_abort("ranked signing failed");
            return;
        }
        memcpy(message + 32, session.record.signatures[session.local], 64);
        session.sent_signature = pc_net_send_reliable(MSG_SIGNATURE, message, sizeof message);
    }
    if (!session.got_signature || !session.sent_signature)
        return;
    crypto_blake2b(hash, 32, wire, RECORD_BODY);
    if (memcmp(hash, session.peer_signature, 32)) {
        pc_rank_session_abort("signature belongs to another set");
        return;
    }
    memcpy(session.record.signatures[1 - session.local], session.peer_signature + 32, 64);
    if (!pc_rank_verify(&session.record)) {
        pc_rank_session_abort("ranked peer signature invalid");
        return;
    }
    if (!session.appended) {
        PcNetRankStoreResult result = pc_rank_store_append(session.store, &session.record);
        if (result != PC_RANK_STORE_OK) {
            pc_rank_session_abort(result == PC_RANK_STORE_UNCERTAIN ?
                                      "rank save uncertain: reopen profile" :
                                      "rank save failed: rating unchanged");
            return;
        }
        session.appended = true;
    }
    if (!pc_rank_head(&session.record, hash)) {
        pc_rank_session_abort("saved record hash invalid");
        return;
    }
    if (!session.sent_saved)
        session.sent_saved = pc_net_send_reliable(MSG_SAVED, hash, sizeof hash);
    if (!session.got_saved || !session.sent_saved)
        return;
    if (memcmp(hash, session.peer_saved, sizeof hash)) {
        pc_rank_session_abort("peer saved a different record");
        return;
    }
    session.state = PC_RANK_SESSION_SAVED;
    session.reason = "both peers saved the signed set; publication pending";
}
bool pc_rank_session_receive(uint8_t type, const void* payload, int n) {
    const uint8_t* data = payload;
    if (type < MSG_HELLO || type > MSG_SAVED)
        return false;
    if (session.state == PC_RANK_SESSION_OFF || session.state == PC_RANK_SESSION_SAVED ||
        session.state == PC_RANK_SESSION_FAILED)
        return true;
    if (!payload || n < 0) {
        pc_rank_session_abort("invalid ranked packet");
        return true;
    }
    if (type == MSG_HELLO)
        hello_receive(data, n);
    else if (type == MSG_ABORT)
        pc_rank_session_abort("peer refused ranked set");
    else if (type == MSG_SAVED) {
        if (n != 32 || (session.got_saved && memcmp(session.peer_saved, data, 32)))
            pc_rank_session_abort("invalid or conflicting peer save confirmation");
        else {
            memcpy(session.peer_saved, data, 32);
            session.got_saved = true;
        }
    } else if (type == MSG_BODY) {
        if (n != RECORD_BODY || (session.got_body && memcmp(session.peer_body, data, RECORD_BODY)))
            pc_rank_session_abort("invalid or conflicting ranked record");
        else {
            memcpy(session.peer_body, data, RECORD_BODY);
            session.got_body = true;
        }
    } else if (type == MSG_SIGNATURE) {
        if (n != 96 || (session.got_signature && memcmp(session.peer_signature, data, 96)))
            pc_rank_session_abort("invalid or conflicting ranked signature");
        else {
            memcpy(session.peer_signature, data, 96);
            session.got_signature = true;
        }
    }
    return true;
}
void pc_rank_session_poll(void) {
    if (session.state == PC_RANK_SESSION_OFF || session.state == PC_RANK_SESSION_SAVED ||
        pc_net_resim())
        return;
    if (session.state == PC_RANK_SESSION_FAILED) {
        if (!session.sent_abort)
            session.sent_abort = pc_net_send_reliable(MSG_ABORT, "failed", 6);
        return;
    }
    if (!pc_net_active()) {
        pc_rank_session_abort("ranked peer disconnected");
        return;
    }
    if (!session.sent_hello)
        session.sent_hello = pc_net_send_reliable(MSG_HELLO, session.hello, HELLO_SIZE);
    uint8_t type, data[256];
    int n;
    while ((n = pc_net_recv_reliable(&type, data, sizeof data)) >= 0)
        pc_rank_session_receive(type, data, n);
    if ((session.state == PC_RANK_SESSION_WAIT || session.state == PC_RANK_SESSION_SIGN) &&
        time(NULL) > session.deadline)
        pc_rank_session_abort("ranked proof or signature timed out");
    complete();
}
int pc_rank_session_state(const char** reason) {
    if (reason)
        *reason = session.reason;
    return session.state;
}
bool pc_rank_session_set_complete(void) {
    return session.record.game_count > 0 &&
           (session.set.winner >= 0 || session.record.game_count >= PC_RANK_MAX_GAMES);
}
bool pc_rank_session_active(void) {
    return session.state == PC_RANK_SESSION_PLAY;
}
static const unsigned legal[] = {2, 3, 8, 28, 31, 32};
void pc_rank_session_stage_begin(void) {
    if (!pc_rank_session_active() || session.stage_started)
        return;
    session.stage_started = true;
    session.stage = 0;
    session.ban_count = 0;
    if (!session.record.game_count)
        session.stage = legal[session.record.match_id[0] % 6];
    else if (session.set.tiebreak)
        session.stage = session.record.games[session.record.game_count - 1].stage;
}
int pc_rank_session_stage_port(void) {
    if (!pc_rank_session_active() || session.stage)
        return -1;
    return session.ban_count < 2 ? 1 - session.set.chooser : session.set.chooser;
}
bool pc_rank_session_stage_available(unsigned stage) {
    bool found = false;
    for (unsigned i = 0; i < 6; i++)
        if (stage == legal[i])
            found = true;
    if (!found)
        return false;
    for (unsigned i = 0; i < session.ban_count; i++)
        if (stage == session.bans[i])
            return false;
    return true;
}
bool pc_rank_session_choose_stage(unsigned player, unsigned stage) {
    if (!session.stage_started || (int)player != pc_rank_session_stage_port() ||
        !pc_rank_session_stage_available(stage))
        return false;
    if (session.ban_count < 2)
        session.bans[session.ban_count++] = stage;
    else
        session.stage = stage;
    return true;
}
unsigned pc_rank_session_stage(void) {
    return session.stage;
}
const char* pc_rank_session_stage_prompt(void) {
    if (session.stage)
        return "RANKED: STAGE READY";
    if (session.ban_count == 0)
        return "RANKED: WINNER BANS FIRST STAGE";
    if (session.ban_count == 1)
        return "RANKED: WINNER BANS SECOND STAGE";
    return "RANKED: LOSER CHOOSES STAGE";
}
unsigned pc_rank_session_stocks(void) {
    return session.set.tiebreak ? 1 : 4;
}
unsigned pc_rank_session_seconds(void) {
    return session.set.tiebreak ? 180 : 480;
}
bool pc_rank_session_game(
    unsigned winner, unsigned a, unsigned b, unsigned stage, uint32_t frames) {
    bool previously_failed = session.state == PC_RANK_SESSION_FAILED;
    if ((!pc_rank_session_active() && !previously_failed) || !session.stage_started ||
        !session.stage || stage != session.stage ||
        session.record.game_count >= PC_RANK_MAX_GAMES || winner > PC_RANK_TIE || a > 4 || b > 4)
    {
        pc_rank_session_abort("invalid ranked game outcome");
        return false;
    }
    PcNetRankRecord candidate = session.record;
    candidate.games[candidate.game_count++] =
        (PcNetRankGame){(uint8_t)winner, {(uint8_t)a, (uint8_t)b}, (uint8_t)stage, frames};
    PcNetRankSet set;
    if (!pc_rank_set(candidate.games, candidate.game_count, &set)) {
        pc_rank_session_abort("ranked game violates set rules");
        return false;
    }
    session.record = candidate;
    session.set = set;
    session.stage_started = false;
    if (previously_failed)
        return false; /* Keep deterministic outcomes despite asynchronous connection failure. */
    if (set.winner >= 0) {
        session.state = PC_RANK_SESSION_SIGN;
        session.reason = "waiting for identical set and both signatures";
        session.deadline = time(NULL) + 60;
    } else if (candidate.game_count == PC_RANK_MAX_GAMES) {
        pc_rank_session_abort("too many tied games for ranked record: set not rated");
        return false;
    }
    return true;
}
const PcNetRankRecord* pc_rank_session_record(void) {
    return session.state == PC_RANK_SESSION_SAVED ? &session.record : NULL;
}

bool pc_rank_session_latest_record(uint8_t out[PC_RANK_RECORD_BYTES]) {
    return pc_rank_store_latest_record(session.store, out);
}
