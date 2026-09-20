/* Deterministic fake DHT transport: no sockets, DNS, or public network. */
#include "../src/pc/net_dht_item.c"
#include "../extern/dht/sha1.h"
#include "../extern/monocypher/monocypher-ed25519.h"
#include <assert.h>
static uint8_t sent[128][1600];
static size_t sent_length[128];
static struct pc_dht_endpoint sent_to[128];
static unsigned sent_count;
static int completions;
static PcDhtItemResult result;
static const uint8_t seed_node_id[20] = {0x80};
void pc_dht_sha1(const void* data, size_t len, uint8_t out[20]) {
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, data, len);
    SHA1Final(out, &ctx);
}
intptr_t pc_dht_socket(void) {
    return 123;
}
size_t pc_dht_item_seeds(struct pc_dht_endpoint* out, size_t capacity) {
    assert(capacity);
    out[0] = (struct pc_dht_endpoint){htonl(0x08080808), 6881};
    return 1;
}
void pc_dht_item_node_id(uint8_t out[20]) {
    memset(out, 0x42, 20);
}
bool pc_dht_item_send(const void* data, size_t len, const struct pc_dht_endpoint* to) {
    assert(sent_count < 128 && len <= 1600);
    memcpy(sent[sent_count], data, len);
    sent_length[sent_count] = len;
    sent_to[sent_count++] = *to;
    return true;
}
static void done(const PcDhtItemResult* r, void* context) {
    assert(context == &result);
    result = *r;
    completions++;
}
static void reset(void) {
    pc_dht_item_cancel();
    sent_count = 0;
    completions = 0;
    memset(&result, 0, sizeof(result));
}
static size_t response(uint8_t out[1600], unsigned request, const PcNetIdentity* identity,
    int64_t sequence, const char* value, bool bad_signature, bool add_closer) {
    struct slice query = {sent[request], sent_length[request]}, tid;
    assert(string_field(query, "t", &tid));
    uint8_t* p = out;
    memcpy(p, "d1:rd2:id20:", 12);
    p += 12;
    if (request == 0)
        memcpy(p, seed_node_id, 20);
    else
        memcpy(p, item.target, 20);
    p += 20;
    if (identity) {
        memcpy(p, "1:k32:", 6);
        p += 6;
        memcpy(p, identity->public_key, 32);
        p += 32;
    }
    if (add_closer) {
        memcpy(p, "5:nodes26:", 10);
        p += 10;
        memcpy(p, item.target, 20);
        p += 20;
        *p++ = 9;
        *p++ = 9;
        *p++ = 9;
        *p++ = 9;
        *p++ = 0x1a;
        *p++ = 0xe1;
    }
    if (identity) {
        uint8_t payload[1200], signature[64];
        size_t n = pc_dht_item_signable(
            payload, item.salt, item.salt_length, sequence, value, strlen(value));
        pc_identity_sign(identity, signature, payload, n);
        if (bad_signature)
            signature[0] ^= 1;
        p += sprintf((char*)p, "3:seqi%llde3:sig64:", (long long)sequence);
        memcpy(p, signature, 64);
        p += 64;
    }
    memcpy(p, "5:token3:tok", 12);
    p += 12;
    if (identity || (item.immutable && value)) {
        memcpy(p, "1:v", 3);
        p += 3;
        p += append_string(p, value, strlen(value));
    }
    memcpy(p, "e1:t8:", 6);
    p += 6;
    memcpy(p, tid.p, 8);
    p += 8;
    memcpy(p, "1:y1:re", 7);
    p += 7;
    return p - out;
}
static void deliver(unsigned request, const PcNetIdentity* id, int64_t seq, const char* value,
    bool invalid, bool closer) {
    uint8_t packet[1600];
    size_t len = response(packet, request, id, seq, value, invalid, closer);
    assert(pc_dht_item_receive(packet, len, sent_to[request].address, sent_to[request].port));
}
static void unhex(uint8_t* out, const char* text) {
    for (size_t i = 0; i < strlen(text) / 2; i++) {
        unsigned x;
        assert(sscanf(text + 2 * i, "%2x", &x) == 1);
        out[i] = (uint8_t)x;
    }
}
int main(void) {
    PcNetIdentity identity = {0};
    uint8_t seed[32] = {1};
    crypto_ed25519_key_pair(identity.secret_key, identity.public_key, seed);
    /* Official BEP44 vectors, independent of our signing implementation. */
    uint8_t official_key[32], official_sig[64], canonical[1200];
    unhex(official_key, "77ff84905a91936367c01360803104f92432fcd904a43511876df5cdf3e7e548");
    unhex(official_sig, "305ac8aeb6c9c151fa120f120ea2cfb923564e11552d06a5d856091e5e853cff1260d3f39e"
                        "4999684aa92eb73ffd136e6f4f3ecbfda0ce53a1608ecd7ae21f01");
    size_t n = pc_dht_item_signable(canonical, NULL, 0, 1, "Hello World!", 12);
    assert(pc_identity_verify(official_key, official_sig, canonical, n));
    unhex(official_sig, "6834284b6b24c3204eb2fea824d82f88883a3d95e8b4a21b8c0ded553d17d17ddf9a8a7104"
                        "b1258f30bed3787e6cb896fca78c58f8e03b5f18f14951a87d9a08");
    n = pc_dht_item_signable(canonical, "foobar", 6, 1, "Hello World!", 12);
    assert(pc_identity_verify(official_key, official_sig, canonical, n));
    n = pc_dht_item_signable(canonical, NULL, 0, 1, "Hello World!", 12);
    assert(n == strlen("3:seqi1e1:v12:Hello World!") &&
           !memcmp(canonical, "3:seqi1e1:v12:Hello World!", n));
    n = pc_dht_item_signable(canonical, "foobar", 6, 2, "Hello World!", 12);
    assert(n == strlen("4:salt6:foobar3:seqi2e1:v12:Hello World!") &&
           !memcmp(canonical, "4:salt6:foobar3:seqi2e1:v12:Hello World!", n));
    assert(!pc_dht_item_signable(canonical, NULL, 0, -1, "a", 1));
    assert(!pc_dht_item_signable(canonical, "x", 65, 1, "a", 1));
    assert(!pc_dht_item_signable(canonical, NULL, 0, 1, "x", 997));
    int64_t decoded;
    assert(!integer((struct slice){(const uint8_t*)"i9223372036854775808e", 21}, &decoded));
    assert(!integer((struct slice){(const uint8_t*)"i01e", 4}, &decoded));
    assert(pc_dht_item_get(identity.public_key, "rating", 6, 0, done, &result));
    assert(!pc_dht_item_get(identity.public_key, NULL, 0, 0, done, &result));
    pc_dht_item_tick();
    assert(sent_count == 1);
    uint8_t packet[1600];
    size_t len = response(packet, 0, &identity, 3, "record", false, false);
    assert(!pc_dht_item_receive(packet, len, htonl(0x09090909), 6881));
    assert(!completions && item.nodes[0].pending);
    assert(pc_dht_item_receive(packet, len, sent_to[0].address, 6881));
    pc_dht_item_tick();
    assert(completions == 1 && result.status == PC_DHT_ITEM_OK && result.sequence == 3 &&
           result.value_length == 6 && !memcmp(result.value, "record", 6));
    reset();
    assert(pc_dht_item_get(identity.public_key, NULL, 0, 0, done, &result));
    pc_dht_item_tick();
    deliver(0, &identity, 3, "bad", true, false);
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_NOT_FOUND);
    reset();
    assert(pc_dht_item_get(identity.public_key, NULL, 0, 4, done, &result));
    pc_dht_item_tick();
    deliver(0, &identity, 3, "old", false, false);
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_NOT_FOUND);
    reset();
    assert(pc_dht_item_get(identity.public_key, NULL, 0, 0, done, &result));
    pc_dht_item_tick();
    deliver(0, NULL, 0, "", false, true);
    pc_dht_item_tick();
    assert(sent_count == 2 && sent_to[1].address == htonl(0x09090909));
    deliver(1, &identity, 8, "nearest", false, false);
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_OK && result.sequence == 8);
    /* Two independently signed replies at the same sequence must agree.
     * Different lengths and equal-length conflicting bytes both fail closed;
     * duplicate identical values and older signed records remain harmless. */
    const char* second_values[] = {"different", "other", "first", "older"};
    for (unsigned i = 0; i < 4; i++) {
        reset();
        assert(pc_dht_item_get(identity.public_key, NULL, 0, 0, done, &result));
        pc_dht_item_tick();
        deliver(0, &identity, 8, "first", false, true);
        pc_dht_item_tick();
        assert(sent_count == 2);
        deliver(1, &identity, i == 3 ? 7 : 8, second_values[i], false, false);
        pc_dht_item_tick();
        assert(completions == 1 && !pc_dht_item_busy());
        assert(result.status == (i < 2 ? PC_DHT_ITEM_CONFLICT : PC_DHT_ITEM_OK));
        if (i >= 2)
            assert(result.sequence == 8 && result.value_length == 5 &&
                   !memcmp(result.value, "first", 5));
    }
    reset();
    assert(pc_dht_item_put(&identity, "rating", 6, 4, "new", 3, done, &result));
    pc_dht_item_tick();
    deliver(0, &identity, 3, "old", false, false);
    pc_dht_item_tick();
    assert(sent_count == 2);
    struct slice wire = {sent[1], sent_length[1]}, args, token, sig, key, seq, cas, value, payload;
    int64_t parsed;
    assert(field(wire, "a", &args) && string_field(args, "token", &token) && token.n == 3 &&
           !memcmp(token.p, "tok", 3));
    assert(string_field(args, "sig", &sig) && sig.n == 64 && string_field(args, "k", &key) &&
           key.n == 32);
    assert(field(args, "cas", &cas) && integer(cas, &parsed) && parsed == 3);
    assert(field(args, "seq", &seq) && integer(seq, &parsed) && parsed == 4);
    assert(field(args, "v", &value) && bytes(value, &payload) && payload.n == 3);
    n = pc_dht_item_signable(canonical, "rating", 6, 4, payload.p, payload.n);
    assert(pc_identity_verify(key.p, sig.p, canonical, n));
    /* Reply id is the queried server's ID, not the lookup target. */
    len = response(packet, 1, NULL, 0, "", false, false);
    memcpy(packet + 12, seed_node_id, 20);
    assert(pc_dht_item_receive(packet, len, sent_to[1].address, sent_to[1].port));
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_OK && result.acknowledgements == 1 &&
           result.sequence == 4 && result.value_length == 3);
    reset();
    assert(pc_dht_item_put(&identity, NULL, 0, 2, "new", 3, done, &result));
    pc_dht_item_tick();
    deliver(0, &identity, 3, "old", false, false);
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_CONFLICT && sent_count == 1);
    reset();
    assert(pc_dht_item_put(&identity, NULL, 0, 3, "new", 3, done, &result));
    pc_dht_item_tick();
    deliver(0, &identity, 3, "old", false, false);
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_CONFLICT);
    /* Official BEP44 immutable vector and authenticated-by-target retrieval. */
    uint8_t immutable_target[20], expected_target[20];
    assert(pc_dht_item_immutable_target("Hello World!", 12, immutable_target));
    unhex(expected_target, "e5f96f6f38320f0f33959cb4d3d656452117aadb");
    assert(!memcmp(immutable_target, expected_target, 20));
    assert(!pc_dht_item_immutable_target("x", 997, immutable_target));
    reset();
    assert(pc_dht_item_get_immutable(expected_target, done, &result));
    pc_dht_item_tick();
    deliver(0, NULL, 0, "Hello World!", false, false);
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_OK && result.sequence == 0 && result.value_length == 12 &&
           !memcmp(result.value, "Hello World!", 12));
    reset();
    assert(pc_dht_item_get_immutable(expected_target, done, &result));
    pc_dht_item_tick();
    deliver(0, NULL, 0, "forged value", false, false);
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_NOT_FOUND);
    reset();
    assert(pc_dht_item_put_immutable("Hello World!", 12, done, &result));
    pc_dht_item_tick();
    deliver(0, NULL, 0, NULL, false, false);
    pc_dht_item_tick();
    assert(sent_count == 2);
    wire = (struct slice){sent[1], sent_length[1]};
    assert(field(wire, "a", &args) && string_field(args, "token", &token) && token.n == 3);
    const char* absent[] = {"k", "sig", "seq", "salt", "cas"};
    for (unsigned i = 0; i < 5; i++) {
        struct slice found;
        assert(field(args, absent[i], &found) && !found.p);
    }
    assert(string_field(args, "v", &payload) && payload.n == 12 &&
           !memcmp(payload.p, "Hello World!", 12));
    len = response(packet, 1, NULL, 0, NULL, false, false);
    memcpy(packet + 12, seed_node_id, 20);
    assert(pc_dht_item_receive(packet, len, sent_to[1].address, sent_to[1].port));
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_OK && result.acknowledgements == 1 &&
           result.value_length == 12 && result.sequence == 0);
    reset();
    assert(pc_dht_item_get(identity.public_key, NULL, 0, 0, done, &result));
    item.deadline = 0;
    pc_dht_item_tick();
    assert(result.status == PC_DHT_ITEM_TIMEOUT);
    reset();
    assert(pc_dht_item_get(identity.public_key, NULL, 0, 0, done, &result));
    pc_dht_item_cancel();
    assert(result.status == PC_DHT_ITEM_CANCELLED && !pc_dht_item_busy());
    puts("BEP44 mutable/immutable vectors, signing, tokens, CAS, lookup, spoof rejection, "
         "equivocation and lifecycle passed");
}
