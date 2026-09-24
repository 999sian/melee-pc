/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded BEP44 client. Iterative XOR-nearest lookup, four requests in flight,
 * fresh random transaction IDs, 32-node shortlist, at most 64 get requests,
 * 8 closest token-bearing replicas, source+transaction reply matching. */
#include "net_dht_item.h"
#include "net_dht.h"
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#define NODE_COUNT 32
#define REPLICAS 8
#define GET_LIMIT 64
struct node {
    uint8_t id[20], transaction[8], token[128];
    struct pc_dht_endpoint endpoint;
    size_t token_length;
    uint64_t sent;
    int64_t sequence;
    bool known, queried, pending, put_sent, responded;
};
static struct {
    bool active, publishing, putting, immutable, first;
    uint8_t key[32], target[20], salt[64], value[1000], signature[64], best_value[1000];
    size_t salt_length, value_length, best_length;
    int64_t sequence, best_sequence;
    uint64_t deadline;
    unsigned count, queries, acknowledgements, responses;
    struct node nodes[NODE_COUNT];
    pc_dht_item_callback callback;
    void* context;
} item;
/* Provided by the discovery module; no second UDP socket. */
extern size_t pc_dht_item_seeds(struct pc_dht_endpoint* out, size_t capacity);
extern bool pc_dht_item_send(const void* data, size_t length, const struct pc_dht_endpoint* to);
extern void pc_dht_item_node_id(uint8_t out[20]);

struct slice {
    const uint8_t* p;
    size_t n;
};
static bool read_string(const uint8_t** p, const uint8_t* end, struct slice* out) {
    const uint8_t* start = *p;
    size_t n = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        if (n > 4096 || (*p > start && *start == '0'))
            return false;
        n = n * 10 + *(*p)++ - '0';
    }
    if (*p == start || *p >= end || *(*p)++ != ':' || n > (size_t)(end - *p))
        return false;
    *out = (struct slice){*p, n};
    *p += n;
    return true;
}
static bool integer(struct slice raw, int64_t* out) {
    if (raw.n < 3 || raw.p[0] != 'i' || raw.p[raw.n - 1] != 'e')
        return false;
    uint64_t n = 0;
    for (size_t i = 1; i < raw.n - 1; i++) {
        if (raw.p[i] < '0' || raw.p[i] > '9' || (i > 1 && raw.p[1] == '0'))
            return false;
        unsigned digit = raw.p[i] - '0';
        if (n > ((uint64_t)INT64_MAX - digit) / 10)
            return false;
        n = n * 10 + digit;
    }
    *out = (int64_t)n;
    return true;
}
static bool skip(const uint8_t** p, const uint8_t* end, unsigned depth) {
    if (*p >= end || depth > 8)
        return false;
    if (**p >= '0' && **p <= '9') {
        struct slice s;
        return read_string(p, end, &s);
    }
    const uint8_t* start = *p;
    uint8_t kind = *(*p)++;
    if (kind == 'i') {
        if (*p < end && **p == '-')
            ++*p;
        const uint8_t* digits = *p;
        while (*p < end && **p >= '0' && **p <= '9')
            ++*p;
        if (*p == digits || *p >= end || *(*p)++ != 'e')
            return false;
        /* Negative integers are allowed in unrelated error payloads. */
        return *p - start <= 22;
    }
    if (kind != 'l' && kind != 'd')
        return false;
    while (*p < end && **p != 'e') {
        if (kind == 'd') {
            struct slice key;
            if (!read_string(p, end, &key))
                return false;
        }
        if (!skip(p, end, depth + 1))
            return false;
    }
    if (*p >= end)
        return false;
    ++*p;
    return true;
}
/* Returns raw bencode for a unique dictionary key; malformed/duplicate keys
 * reject the entire lookup. Values are not searched recursively. */
static bool field(struct slice dictionary, const char* key, struct slice* out) {
    const uint8_t* p = dictionary.p;
    const uint8_t* end = p + dictionary.n;
    *out = (struct slice){0};
    if (!dictionary.n || *p++ != 'd')
        return false;
    while (p < end && *p != 'e') {
        struct slice name;
        if (!read_string(&p, end, &name))
            return false;
        const uint8_t* start = p;
        if (!skip(&p, end, 1))
            return false;
        if (name.n == strlen(key) && !memcmp(name.p, key, name.n)) {
            if (out->p)
                return false;
            *out = (struct slice){start, (size_t)(p - start)};
        }
    }
    return p < end && *p++ == 'e' && p == end;
}
static bool bytes(struct slice raw, struct slice* out) {
    if (!raw.p)
        return false;
    const uint8_t* p = raw.p;
    return read_string(&p, raw.p + raw.n, out) && p == raw.p + raw.n;
}
static bool string_field(struct slice dictionary, const char* key, struct slice* out) {
    struct slice raw;
    return field(dictionary, key, &raw) && bytes(raw, out);
}
static size_t append_string(uint8_t* dst, const void* data, size_t length) {
    int n = sprintf((char*)dst, "%zu:", length);
    if (length)
        memcpy(dst + n, data, length);
    return n + length;
}
size_t pc_dht_item_signable(void* output, const void* salt, size_t salt_length, int64_t sequence,
    const void* value, size_t value_length) {
    if (!output || salt_length > 64 || (salt_length && !salt) || sequence < 0 ||
        value_length > PC_DHT_ITEM_MAX_VALUE || (value_length && !value))
        return 0;
    uint8_t* p = output;
    if (salt_length) {
        memcpy(p, "4:salt", 6);
        p += 6;
        p += append_string(p, salt, salt_length);
    }
    p += sprintf((char*)p, "3:seqi%llde1:v", (long long)sequence);
    p += append_string(p, value, value_length);
    return (size_t)(p - (uint8_t*)output);
}
static int distance(const uint8_t a[20], const uint8_t b[20]) {
    for (int i = 0; i < 20; i++) {
        unsigned x = a[i] ^ item.target[i], y = b[i] ^ item.target[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}
static bool valid_endpoint(struct pc_dht_endpoint ep) {
    uint32_t ip = ntohl(ep.address);
    return ep.port && (ip >> 24) != 0 && (ip >> 24) != 10 && (ip >> 24) != 127 && (ip >> 28) < 14 &&
           (ip >> 16) != 0xa9fe && (ip >> 20) != 0xac1 && (ip >> 16) != 0xc0a8 &&
           (ip >> 22) != 0x191;
}
static void add_node(const uint8_t* id, struct pc_dht_endpoint endpoint) {
    if (!valid_endpoint(endpoint))
        return;
    for (unsigned i = 0; i < item.count; i++) {
        struct node* n = &item.nodes[i];
        if (n->endpoint.address == endpoint.address && n->endpoint.port == endpoint.port) {
            if (id && !n->known) {
                memcpy(n->id, id, 20);
                n->known = true;
            }
            return;
        }
        if (id && n->known && !memcmp(n->id, id, 20))
            return;
    }
    unsigned slot = item.count;
    if (slot == NODE_COUNT) {
        if (!id)
            return;
        slot = NODE_COUNT;
        for (unsigned i = 0; i < NODE_COUNT; i++) {
            struct node* n = &item.nodes[i];
            if (n->pending)
                continue;
            if (slot == NODE_COUNT || !n->known ||
                (item.nodes[slot].known && distance(n->id, item.nodes[slot].id) > 0))
                slot = i;
        }
        if (slot == NODE_COUNT ||
            (item.nodes[slot].known && distance(id, item.nodes[slot].id) >= 0))
            return;
    } else
        item.count++;
    struct node* n = &item.nodes[slot];
    memset(n, 0, sizeof(*n));
    n->endpoint = endpoint;
    n->sequence = -1;
    if (id) {
        memcpy(n->id, id, 20);
        n->known = true;
    }
}
static void finish(enum pc_dht_item_status status) {
    PcDhtItemResult result = {0};
    result.status = status;
    result.sequence = item.best_sequence;
    result.acknowledgements = item.acknowledgements;
    result.value_length = item.best_length;
    memcpy(result.value, item.best_value, item.best_length);
    if (item.publishing && status == PC_DHT_ITEM_OK) {
        result.sequence = item.sequence;
        result.value_length = item.value_length;
        memcpy(result.value, item.value, item.value_length);
    }
    pc_dht_item_callback callback = item.callback;
    void* context = item.context;
    memset(&item, 0, sizeof(item));
    if (callback)
        callback(&result, context);
}
bool pc_dht_item_busy(void) {
    return item.active;
}
void pc_dht_item_cancel(void) {
    if (item.active)
        finish(PC_DHT_ITEM_CANCELLED);
}
static bool begin(const uint8_t key[32], const void* salt, size_t salt_length, int64_t sequence,
    pc_dht_item_callback callback, void* context) {
    if (item.active || pc_dht_socket() < 0 || !key || salt_length > 64 || (salt_length && !salt) ||
        sequence < 0)
        return false;
    memset(&item, 0, sizeof(item));
    item.active = true;
    item.sequence = sequence;
    item.best_sequence = -1;
    memcpy(item.key, key, 32);
    if (salt_length)
        memcpy(item.salt, salt, salt_length);
    item.salt_length = salt_length;
    uint8_t target[96];
    memcpy(target, key, 32);
    if (salt_length)
        memcpy(target + 32, salt, salt_length);
    pc_dht_sha1(target, 32 + salt_length, item.target);
    item.callback = callback;
    item.context = context;
    item.deadline = SDL_GetTicks() + 20000;
    struct pc_dht_endpoint seeds[NODE_COUNT];
    size_t count = pc_dht_item_seeds(seeds, NODE_COUNT);
    for (size_t i = 0; i < count; i++)
        add_node(NULL, seeds[i]);
    return true;
}
bool pc_dht_item_get(const uint8_t key[32], const void* salt, size_t salt_length, int64_t minimum,
    pc_dht_item_callback callback, void* context) {
    return begin(key, salt, salt_length, minimum, callback, context);
}
bool pc_dht_item_get_first(const uint8_t key[32], const void* salt, size_t salt_length,
    int64_t minimum, pc_dht_item_callback callback, void* context) {
    if (!begin(key, salt, salt_length, minimum, callback, context))
        return false;
    item.first = true;
    return true;
}
bool pc_dht_item_put(const PcNetIdentity* identity, const void* salt, size_t salt_length,
    int64_t sequence, const void* value, size_t length, pc_dht_item_callback callback,
    void* context) {
    uint8_t signable[1200];
    size_t n = pc_dht_item_signable(signable, salt, salt_length, sequence, value, length);
    if (!identity || !n ||
        !begin(identity->public_key, salt, salt_length, sequence, callback, context))
        return false;
    item.publishing = true;
    item.value_length = length;
    if (length)
        memcpy(item.value, value, length);
    pc_identity_sign(identity, item.signature, signable, n);
    return true;
}
bool pc_dht_item_immutable_target(const void* value, size_t length, uint8_t out[20]) {
    if (!out || length > PC_DHT_ITEM_MAX_VALUE || (length && !value))
        return false;
    uint8_t encoded[1000];
    size_t n = append_string(encoded, value, length);
    pc_dht_sha1(encoded, n, out);
    return true;
}
bool pc_dht_item_get_immutable(
    const uint8_t target[20], pc_dht_item_callback callback, void* context) {
    const uint8_t unused_key[32] = {0};
    if (!target || !begin(unused_key, NULL, 0, 0, callback, context))
        return false;
    item.immutable = true;
    memcpy(item.target, target, 20);
    return true;
}
bool pc_dht_item_put_immutable(
    const void* value, size_t length, pc_dht_item_callback callback, void* context) {
    uint8_t target[20];
    if (!pc_dht_item_immutable_target(value, length, target) ||
        !pc_dht_item_get_immutable(target, callback, context))
        return false;
    item.publishing = true;
    item.value_length = length;
    if (length)
        memcpy(item.value, value, length);
    return true;
}
static bool send_query(struct node* node, bool put) {
    uint8_t packet[1600], id[20];
    uint8_t* p = packet;
    pc_dht_item_node_id(id);
    if (!pc_identity_random(node->transaction, 8))
        return false;
    memcpy(p, "d1:ad2:id20:", 12);
    p += 12;
    memcpy(p, id, 20);
    p += 20;
    if (put) {
        /* Dictionary keys must be lexicographically sorted: cas precedes id.
         * Rebuild prefix for CAS rather than emitting a noncanonical dict. */
        if (!item.immutable && node->sequence >= 0) {
            p = packet;
            memcpy(p, "d1:ad", 5);
            p += 5;
            p += sprintf((char*)p, "3:casi%llde2:id20:", (long long)node->sequence);
            memcpy(p, id, 20);
            p += 20;
        }
        if (!item.immutable) {
            memcpy(p, "1:k32:", 6);
            p += 6;
            memcpy(p, item.key, 32);
            p += 32;
            if (item.salt_length) {
                memcpy(p, "4:salt", 6);
                p += 6;
                p += append_string(p, item.salt, item.salt_length);
            }
            p += sprintf((char*)p, "3:seqi%llde3:sig64:", (long long)item.sequence);
            memcpy(p, item.signature, 64);
            p += 64;
        }
        memcpy(p, "5:token", 7);
        p += 7;
        p += append_string(p, node->token, node->token_length);
        memcpy(p, "1:v", 3);
        p += 3;
        p += append_string(p, item.value, item.value_length);
        memcpy(p, "e1:q3:put1:t8:", 14);
        p += 14;
    } else {
        memcpy(p, "6:target20:", 11);
        p += 11;
        memcpy(p, item.target, 20);
        p += 20;
        memcpy(p, "e1:q3:get1:t8:", 14);
        p += 14;
    }
    memcpy(p, node->transaction, 8);
    p += 8;
    memcpy(p, "1:y1:qe", 7);
    p += 7;
    if (!pc_dht_item_send(packet, (size_t)(p - packet), &node->endpoint))
        return false;
    node->pending = true;
    node->sent = SDL_GetTicks();
    if (put)
        node->put_sent = true;
    else {
        node->queried = true;
        item.queries++;
    }
    return true;
}
static int closest_unqueried(void) {
    int best = -1;
    for (unsigned i = 0; i < item.count; i++)
        if (!item.nodes[i].queried) {
            if (best < 0 ||
                (item.nodes[i].known && (!item.nodes[best].known ||
                                            distance(item.nodes[i].id, item.nodes[best].id) < 0)))
                best = (int)i;
        }
    return best;
}
static void start_puts(void) {
    if (item.best_sequence > item.sequence ||
        (item.best_sequence == item.sequence &&
            (item.best_length != item.value_length ||
                memcmp(item.best_value, item.value, item.value_length))))
    {
        finish(PC_DHT_ITEM_CONFLICT);
        return;
    }
    item.putting = true;
    item.deadline = SDL_GetTicks() + 5000;
    /* Choose the closest eight respondents that supplied valid tokens. */
    for (unsigned replica = 0; replica < REPLICAS; replica++) {
        int best = -1;
        for (unsigned i = 0; i < item.count; i++) {
            struct node* n = &item.nodes[i];
            if (!n->responded || !n->token_length || n->put_sent)
                continue;
            if (best < 0 || distance(n->id, item.nodes[best].id) < 0)
                best = (int)i;
        }
        if (best < 0)
            break;
        if (!send_query(&item.nodes[best], true))
            item.nodes[best].put_sent = true;
    }
}
void pc_dht_item_tick(void) {
    if (!item.active)
        return;
    uint64_t now = SDL_GetTicks();
    unsigned pending = 0;
    for (unsigned i = 0; i < item.count; i++) {
        struct node* n = &item.nodes[i];
        if (n->pending && now - n->sent >= 2500)
            n->pending = false;
        if (n->pending)
            pending++;
    }
    if (item.putting) {
        if (!pending || now >= item.deadline)
            finish(item.acknowledgements ? PC_DHT_ITEM_OK : PC_DHT_ITEM_TIMEOUT);
        return;
    }
    if (!item.count) { /* Allow bootstrap to provide seeds without blocking. */
        struct pc_dht_endpoint seeds[NODE_COUNT];
        size_t count = pc_dht_item_seeds(seeds, NODE_COUNT);
        for (size_t i = 0; i < count; i++)
            add_node(NULL, seeds[i]);
    }
    while (pending < 4 && item.queries < GET_LIMIT && now < item.deadline) {
        int next = closest_unqueried();
        if (next < 0)
            break;
        if (send_query(&item.nodes[next], false))
            pending++;
        else
            item.nodes[next].queried = true;
    }
    if (now >= item.deadline ||
        (!pending && item.count && (closest_unqueried() < 0 || item.queries >= GET_LIMIT)))
    {
        if (item.publishing)
            start_puts();
        else
            finish(item.best_sequence >= item.sequence ? PC_DHT_ITEM_OK :
                   item.responses                      ? PC_DHT_ITEM_NOT_FOUND :
                                                         PC_DHT_ITEM_TIMEOUT);
    }
}
static bool remember_value(struct node* node, int64_t sequence, struct slice payload) {
    node->sequence = sequence;
    if (sequence == item.best_sequence &&
        (payload.n != item.best_length || memcmp(payload.p, item.best_value, payload.n)))
    {
        finish(PC_DHT_ITEM_CONFLICT);
        return false;
    }
    if (sequence > item.best_sequence) {
        item.best_sequence = sequence;
        item.best_length = payload.n;
        memcpy(item.best_value, payload.p, payload.n);
    }
    return true;
}
bool pc_dht_item_receive(const void* data, size_t length, uint32_t ip, uint16_t port) {
    if (!item.active || !length || length > 4096)
        return false;
    struct slice root = {data, length}, tid, type, response;
    if (!string_field(root, "t", &tid) || tid.n != 8 || !string_field(root, "y", &type) ||
        type.n != 1)
        return false;
    struct node* node = NULL;
    for (unsigned i = 0; i < item.count; i++) {
        struct node* n = &item.nodes[i];
        if (n->pending && n->endpoint.address == ip && n->endpoint.port == port &&
            !memcmp(n->transaction, tid.p, 8))
        {
            node = n;
            break;
        }
    }
    if (!node)
        return false;
    if (*type.p == 'e') {
        node->pending = false;
        return true;
    }
    if (*type.p != 'r' || !field(root, "r", &response) || !response.p)
        return true;
    struct slice id;
    if (!string_field(response, "id", &id) || id.n != 20)
        return true;
    if (node->known && memcmp(node->id, id.p, 20))
        return true;
    memcpy(node->id, id.p, 20);
    node->known = true;
    node->pending = false;
    if (item.putting) {
        item.acknowledgements++;
        return true;
    }
    item.responses++;
    node->responded = true;
    struct slice token;
    if (string_field(response, "token", &token) && token.n && token.n <= 128) {
        memcpy(node->token, token.p, token.n);
        node->token_length = token.n;
    }
    struct slice value, key, signature, seq;
    if (field(response, "v", &value) && value.p) {
        struct slice payload;
        int64_t sequence;
        if (item.immutable && bytes(value, &payload) && payload.n <= PC_DHT_ITEM_MAX_VALUE) {
            uint8_t target[20];
            if (pc_dht_item_immutable_target(payload.p, payload.n, target) &&
                !memcmp(target, item.target, 20) && !remember_value(node, 0, payload))
                return true;
        } else if (!item.immutable && bytes(value, &payload) &&
                   payload.n <= PC_DHT_ITEM_MAX_VALUE && string_field(response, "k", &key) &&
                   key.n == 32 && !memcmp(key.p, item.key, 32) &&
                   string_field(response, "sig", &signature) && signature.n == 64 &&
                   field(response, "seq", &seq) && integer(seq, &sequence))
        {
            uint8_t signable[1200];
            size_t n = pc_dht_item_signable(
                signable, item.salt, item.salt_length, sequence, payload.p, payload.n);
            if (n && pc_identity_verify(key.p, signature.p, signable, n)) {
                if (!remember_value(node, sequence, payload))
                    return true;
                if (item.first && item.best_sequence >= item.sequence) {
                    finish(PC_DHT_ITEM_OK);
                    return true;
                }
            }
        }
    }
    struct slice nodes;
    if (string_field(response, "nodes", &nodes) && nodes.n % 26 == 0 && nodes.n <= 26 * 32) {
        for (size_t i = 0; i < nodes.n; i += 26) {
            struct pc_dht_endpoint ep;
            memcpy(&ep.address, nodes.p + i + 20, 4);
            ep.port = ((unsigned)nodes.p[i + 24] << 8) | nodes.p[i + 25];
            add_node(nodes.p + i, ep);
        }
    }
    return true;
}
