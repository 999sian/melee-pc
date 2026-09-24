/* Signed internet-pairing transcript checks without public DHT access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../src/pc/net_match.c"
#include "../extern/dht/sha1.h"

void pc_dht_sha1(const void* data, size_t len, uint8_t out[20]) {
    SHA1_CTX hash;
    SHA1Init(&hash);
    SHA1Update(&hash, data, (uint32_t)len);
    SHA1Final(out, &hash);
}

static intptr_t dht_fd = -1, game_fd = -1;
static uint16_t local_port, peer_port;
static pc_dht_datagram_fn dht_cb;
static void* dht_ctx;
static PcNetDatagramHandler game_cb;
static int candidate = 1, barrier, dropped_offer, injected_bad;
static uint8_t messages[32][256], types[32];
static int lengths[32], count;
#include "../src/pc/net_dht_item.c"
static uint8_t item_response[1600];
static size_t item_response_length;
static struct pc_dht_endpoint item_endpoint;
static bool mismatch_verified;
static unsigned recovery_puts;
bool pc_dht_ready(void) {
    return true;
}
size_t pc_dht_item_seeds(struct pc_dht_endpoint* out, size_t capacity) {
    assert(capacity);
    out[0] = (struct pc_dht_endpoint){htonl(0x08080808), 6881};
    return 1;
}
void pc_dht_item_node_id(uint8_t out[20]) {
    memset(out, 42, 20);
}
bool pc_dht_item_send(const void* data, size_t length, const struct pc_dht_endpoint* ep) {
    if (getenv("MATCH_PROOF_TIMEOUT"))
        return true;
    struct slice tid;
    assert(string_field((struct slice){data, length}, "t", &tid));
    uint8_t* p = item_response;
    memcpy(p, "d1:rd2:id20:", 12);
    p += 12;
    memset(p, 0x80, 20);
    p += 20;
    struct slice query;
    assert(string_field((struct slice){data, length}, "q", &query));
    if (query.n == 3 && !memcmp(query.p, "put", 3))
        recovery_puts++;
    if (getenv("MATCH_PROOF_MISMATCH") || getenv("MATCH_STALE_LOCAL")) {
        uint8_t value[52], signable[1200], sig[64];
        pc_rank_session_initial_public(value);
        value[19] = getenv("MATCH_STALE_LOCAL") ? 0 : 1;
        size_t n =
            pc_dht_item_signable(signable, item.salt, item.salt_length, value[19], value, 52);
        pc_identity_sign(&identity, sig, signable, n);
        memcpy(p, "1:k32:", 6);
        p += 6;
        memcpy(p, identity.public_key, 32);
        p += 32;
        p += sprintf((char*)p, "3:seqi%ue3:sig64:", value[19]);
        memcpy(p, sig, 64);
        p += 64;
        memcpy(p, "1:v52:", 6);
        p += 6;
        memcpy(p, value, 52);
        p += 52;
    }
    memcpy(p, "5:token3:toke1:t8:", 18);
    p += 18;
    memcpy(p, tid.p, 8);
    p += 8;
    memcpy(p, "1:y1:re", 7);
    p += 7;
    item_response_length = p - item_response;
    item_endpoint = *ep;
    return true;
}
bool pc_net_resim(void) {
    return false;
}
const char* pc_get_net_name(void) {
    return getenv("MATCH_NAME");
}
int pc_get_net_port(void) {
    return local_port;
}
const char* pc_app_rev(void) {
    return "test-rev";
}
const char* pc_lan_disc_id(void) {
    return "test-disc";
}
char* SDL_GetPrefPath(const char* org, const char* app) {
    (void)org;
    (void)app;
    return strdup(getenv("MATCH_DIR"));
}
void SDL_free(void* p) {
    free(p);
}
uint64_t SDL_GetTicks(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000) * 20;
}
bool pc_dht_start(enum pc_dht_mode m, const char* c, int b, uint16_t p) {
    (void)m;
    (void)c;
    (void)b;
    (void)p;
    dht_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(local_port)};
    return bind(dht_fd, (void*)&a, sizeof a) == 0;
}
bool pc_dht_warm(uint16_t p) {
    (void)p;
    return dht_fd >= 0;
}
void pc_dht_idle(void) {
    pc_dht_item_cancel();
    dht_cb = NULL;
}
bool pc_dht_external_endpoint(struct pc_dht_endpoint* out) {
    (void)out;
    return false;
}
void pc_dht_set_datagram_callback(pc_dht_datagram_fn f, void* c) {
    dht_cb = f;
    dht_ctx = c;
}
intptr_t pc_dht_socket(void) {
    return dht_fd;
}
bool pc_dht_next_candidate(struct pc_dht_endpoint* out) {
    if (!candidate)
        return false;
    candidate = 0;
    out->address = htonl(INADDR_LOOPBACK);
    out->port = peer_port;
    return true;
}
void pc_dht_poll(void) {
    pc_dht_item_tick();
    if (item_response_length) {
        size_t n = item_response_length;
        item_response_length = 0;
        pc_dht_item_receive(item_response, n, item_endpoint.address, item_endpoint.port);
        if (item.best_sequence == 1)
            mismatch_verified = true;
    }
    unsigned char b[512];
    struct sockaddr_in a;
    socklen_t z = sizeof a;
    int n;
    while ((n = recvfrom(dht_fd, b, sizeof b, MSG_DONTWAIT, (void*)&a, &z)) > 0) {
        /* Idle node (after a failed attempt): no callback, like net_dht.c,
         * which only hands non-DHT datagrams to a registered callback. */
        if (!dht_cb)
            continue;
        if (n > 5 && b[5] == 'O' && !dropped_offer++) {
            continue;
        }
        struct pc_dht_endpoint e = {a.sin_addr.s_addr, ntohs(a.sin_port)};
        if (n == (int)sizeof(MatchHello) && !injected_bad++) {
            unsigned char bad[sizeof(MatchHello)];
            memcpy(bad, b, n);
            bad[n - 1] ^= 1;
            dht_cb(bad, n, &e, dht_ctx);
            assert(!have_peer);
        }
        dht_cb(b, n, &e, dht_ctx);
    }
}
intptr_t pc_dht_take_socket(void) {
    intptr_t f = dht_fd;
    dht_fd = -1;
    return f;
}
void pc_dht_stop(void) {
    pc_dht_item_cancel();
    if (dht_fd >= 0)
        close(dht_fd);
    dht_fd = -1;
}
bool pc_net_connect_socket(intptr_t f, const char* ip, uint16_t p, int pl, uint32_t s) {
    (void)ip;
    (void)p;
    (void)pl;
    (void)s;
    game_fd = f;
    return true;
}
bool pc_net_active(void) {
    return game_fd >= 0;
}
void pc_net_set_datagram_handler(PcNetDatagramHandler f) {
    game_cb = f;
}
bool pc_net_send_datagram(const void* d, size_t n, uint32_t a, uint16_t p) {
    struct sockaddr_in x = {.sin_family = AF_INET, .sin_addr.s_addr = a, .sin_port = htons(p)};
    return sendto(game_fd, d, n, 0, (void*)&x, sizeof x) == (int)n;
}
void pc_net_poll(void) {
    unsigned char b[512];
    struct sockaddr_in a;
    socklen_t z = sizeof a;
    int n;
    while ((n = recvfrom(game_fd, b, sizeof b, MSG_DONTWAIT, (void*)&a, &z)) > 0) {
        if (n >= 2 && b[0] == 'Z') {
            assert(count < 32);
            types[count] = b[1];
            lengths[count] = n - 2;
            memcpy(messages[count++], b + 2, n - 2);
        } else if (game_cb)
            game_cb(b, n, a.sin_addr.s_addr, ntohs(a.sin_port));
    }
}
bool pc_net_host_match(uint32_t s, int32_t* f) {
    (void)s;
    *f = 120;
    return true;
}
bool pc_net_guest_wait_match(uint32_t* s, int32_t* f) {
    (void)s;
    *f = 120;
    return true;
}
bool pc_net_send_reliable(uint8_t t, const void* p, int n) {
    uint8_t z[258] = {'Z', t};
    if (n)
        memcpy(z + 2, p, n);
    struct sockaddr_in x = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(peer_port)};
    return sendto(game_fd, z, n + 2, 0, (void*)&x, sizeof x) == n + 2;
}
int pc_net_recv_reliable(uint8_t* t, void* p, int n) {
    if (!count)
        return -1;
    *t = types[0];
    int size = lengths[0];
    assert(size <= n);
    memcpy(p, messages[0], size);
    --count;
    memmove(types, types + 1, count);
    memmove(lengths, lengths + 1, count * sizeof *lengths);
    memmove(messages, messages + 1, count * sizeof *messages);
    return size;
}
int pc_net_handshake_state(void) {
    return 2;
}
int32_t pc_net_frame(void) {
    return 0;
}
void pc_net_disconnect(void) {
    if (game_fd >= 0)
        close(game_fd);
    game_fd = -1;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "cancel")) {
        local_port = 0;
        assert(
            pc_net_match_start(getenv("MATCH_RANKED") ? PC_MATCH_RANKED : PC_MATCH_UNRANKED, ""));
        uint8_t key[32];
        memcpy(key, identity.public_key, 32);
        setenv("MATCH_NAME", "NEWNAME", 1);
        pc_net_match_stop();
        assert(pc_net_match_start(PC_MATCH_UNRANKED, ""));
        assert(!memcmp(key, identity.public_key, 32));
        assert(!strncmp(identity.code, "NEWNAME#", 8));
        pc_net_match_stop();
        assert(pc_net_match_state(NULL) == PC_MATCH_FAIL && !pc_net_active() && dht_fd < 0);
        return 0;
    }
    if (argc == 3) {
        local_port = atoi(argv[1]);
        peer_port = atoi(argv[2]);
        assert(
            pc_net_match_start(getenv("MATCH_RANKED") ? PC_MATCH_RANKED : PC_MATCH_UNRANKED, ""));
        for (int i = 0; i < 400 && pc_net_match_state(NULL) != PC_MATCH_READY; i++) {
            pc_net_match_poll();
            usleep(5000);
        }
        if (getenv("MATCH_PROOF_TIMEOUT") || getenv("MATCH_PROOF_MISMATCH")) {
            assert(pc_net_match_state(NULL) == PC_MATCH_FAIL);
            assert(game_fd < 0);
            if (getenv("MATCH_PROOF_MISMATCH"))
                assert(mismatch_verified);
            puts("refused unverified history");
            return 0;
        }
        assert(pc_net_match_state(NULL) == PC_MATCH_READY);
        if (getenv("MATCH_COMPLETE")) {
            for (int game = 0; game < 2; game++) {
                pc_rank_session_stage_begin();
                if (game) {
                    assert(pc_rank_session_choose_stage(0, 2));
                    assert(pc_rank_session_choose_stage(0, 3));
                    assert(pc_rank_session_choose_stage(1, 8));
                }
                assert(pc_rank_session_game(0, 2, 0, pc_rank_session_stage(), 3600));
            }
            for (int i = 0; i < 400 && pc_rank_session_state(NULL) != PC_RANK_SESSION_SAVED; i++) {
                pc_net_poll();
                pc_rank_session_poll();
                usleep(5000);
            }
            assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_SAVED);
            setenv("MATCH_PROOF_TIMEOUT", "1", 1);
            assert(pc_net_match_publish_rank());
            for (int i = 0; i < 800 && pc_net_match_publication(NULL) == 1; i++) {
                pc_net_match_poll_publication();
                usleep(5000);
            }
            assert(pc_net_match_publication(NULL) == -1);
            assert(pc_rank_session_record());
            unsetenv("MATCH_PROOF_TIMEOUT");
            if (getenv("MATCH_RECOVER")) {
                pc_net_match_stop();
                assert(pc_net_match_start(PC_MATCH_RANKED, ""));
                assert(begin_rank());
                assert(public_sequence(local_public) == 1);
                setenv("MATCH_STALE_LOCAL", "1", 1);
                recovery_puts = 0;
                for (int i = 0; i < 400 && proof_step != 2 && proof_step >= 0; i++) {
                    poll_proofs();
                    pc_dht_poll();
                    usleep(5000);
                }
                assert(proof_step == 2);
                assert(recovery_puts >= 2);
                unsetenv("MATCH_STALE_LOCAL");
                /* Equal and newer authenticated heads still fail closed. */
                PcDhtItemResult r = {.status = PC_DHT_ITEM_OK, .sequence = 1, .value_length = 52};
                memcpy(r.value, local_public, 52);
                r.value[0] ^= 1;
                proof_step = 0;
                proof_result(&r, NULL);
                assert(proof_step == -1);
                r.sequence = 2;
                proof_step = 0;
                proof_result(&r, NULL);
                assert(proof_step == -1);
                pc_net_match_stop();
                puts("recovered stale published head after restart");
                return 0;
            }
            assert(pc_net_match_publish_rank());
            for (int i = 0; i < 400 && pc_net_match_publication(NULL) == 1; i++) {
                pc_net_match_poll_publication();
                usleep(5000);
            }
            assert(pc_net_match_publication(NULL) == 2);
            assert(pc_rank_session_record());
            assert(!pc_net_active());
            assert(recovery_puts >= (host ? 2u : 1u));
            pc_net_match_stop();
            assert(pc_net_match_publication(NULL) == 0);
            assert(pc_rank_session_state(NULL) == PC_RANK_SESSION_OFF);
        }
        printf("ready %d %u\n", pc_net_match_is_host(), pc_net_match_seed());
        return 0;
    }
    char path[] = "/tmp/melee-match-XXXXXX";
    assert(mkdtemp(path));
    assert(pc_identity_load(&identity, path, "HOST"));
    identity_loaded = true;

    MatchHello h = {0};
    h.magic = htonl(MATCH_MAGIC);
    h.version = MATCH_VERSION;
    h.type = 'H';
    h.mode = PC_MATCH_UNRANKED;
    h.nonce = 0x123456789abcdef0ULL;
    memcpy(h.public_key, identity.public_key, 32);
    memcpy(h.code, identity.code, sizeof h.code);
    sign_packet(&h, sizeof h);
    assert(sizeof h < 512 && sizeof(MatchOffer) < 512 && sizeof(MatchAck) < 512);
    assert(key_code_matches(h.public_key, h.code));
    assert(signed_ok(h.public_key, h.signature, &h, sizeof h));

    h.mode = PC_MATCH_DIRECT; /* mode is inside the authenticated transcript */
    assert(!signed_ok(h.public_key, h.signature, &h, sizeof h));
    h.mode = PC_MATCH_UNRANKED;
    h.nonce++; /* stale/cross-session substitution is authenticated too */
    assert(!signed_ok(h.public_key, h.signature, &h, sizeof h));
    h.nonce--;
    h.code[strlen(h.code) - 1] ^= 1;
    assert(!key_code_matches(h.public_key, h.code));

    /* Direct connect record: bound to the host's code, its signature and
     * freshness; the slot key is derived from the code alone. */
    snprintf(target, sizeof target, "%s", identity.code);
    uint8_t record[DIRECT_RECORD_BYTES];
    memcpy(record, "MPD1", 4);
    memcpy(record + 4, identity.public_key, 32);
    uint32_t addr = htonl(0x4a2c2ff7);
    memcpy(record + 36, &addr, 4);
    put_be(record + 40, 51234, 2);
    put_be(record + 42, (uint64_t)time(NULL), 8);
    pc_identity_sign(&identity, record + DIRECT_SIGNED_BYTES, record, DIRECT_SIGNED_BYTES);
    struct pc_dht_endpoint found;
    assert(direct_record_read(record, sizeof record, &found));
    assert(found.address == addr && found.port == 51234);
    assert(!direct_record_read(record, sizeof record - 1, &found));
    record[41] ^= 1; /* endpoint is inside the signature */
    assert(!direct_record_read(record, sizeof record, &found));
    record[41] ^= 1;
    put_be(record + 42, (uint64_t)time(NULL) - DIRECT_MAX_AGE - 60, 8);
    pc_identity_sign(&identity, record + DIRECT_SIGNED_BYTES, record, DIRECT_SIGNED_BYTES);
    assert(!direct_record_read(record, sizeof record, &found)); /* stale */
    put_be(record + 42, (uint64_t)time(NULL), 8);
    pc_identity_sign(&identity, record + DIRECT_SIGNED_BYTES, record, DIRECT_SIGNED_BYTES);
    target[strlen(target) - 1] ^= 1; /* someone else's code */
    assert(!direct_record_read(record, sizeof record, &found));
    target[0] = 0;
    PcNetIdentity slot_a, slot_b;
    direct_slot_for("HOST#AAAAAAAA");
    slot_a = direct_slot;
    direct_slot_for("HOST#AAAAAAAA");
    slot_b = direct_slot;
    assert(!memcmp(slot_a.public_key, slot_b.public_key, 32));
    direct_slot_for("HOST#AAAAAAAB");
    assert(memcmp(slot_a.public_key, direct_slot.public_key, 32));

    char file[512];
    snprintf(file, sizeof file, "%s/identity.key", path);
    unlink(file);
    rmdir(path);
    puts("pairing transcript signature, nonce/mode binding, code binding, packet bounds and "
         "direct record checks passed");
    return 0;
}
