/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_dht.h"
#include "net_dht_item.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_timer.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#define CLOSE closesocket
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#define CLOSE close
#endif
#include "../../extern/dht/dht.h"
#include "../../extern/dht/sha1.h"
static intptr_t fd = -1;
static uint16_t bound_port;
static unsigned char local_node_id[20];
static enum pc_dht_mode mode;
static char direct_code[32];
static int rating_band;
static uint64_t started, next_search, next_periodic;
static pc_dht_datagram_fn datagram;
static void* datagram_context;
static struct pc_dht_endpoint queue[64];
static unsigned queue_count;
struct request {
    uint32_t ip;
    uint16_t port;
    unsigned char tid[16];
    size_t len;
    uint64_t sent;
};
static struct request requests[128];
static unsigned request_cursor;
struct vote {
    uint32_t network;
    struct pc_dht_endpoint endpoint;
    uint64_t when;
};
static struct vote votes[16];
static unsigned vote_cursor;
/* Bounded bencode traversal: only top-level byte strings can supply ip/t/y.
 * In particular, binary peer/node values containing "2:ip" are never parsed. */
static bool string_value(
    const unsigned char** p, const unsigned char* end, const unsigned char** data, size_t* len) {
    size_t n = 0;
    const unsigned char* start = *p;
    while (*p < end && **p >= '0' && **p <= '9') {
        if (n > 4096)
            return false;
        n = n * 10 + *(*p)++ - '0';
    }
    if (*p == start || *p >= end || *(*p)++ != ':' || n > (size_t)(end - *p))
        return false;
    *data = *p;
    *len = n;
    *p += n;
    return true;
}
static bool skip_value(const unsigned char** p, const unsigned char* end, int depth) {
    if (*p >= end || depth > 8)
        return false;
    if (**p >= '0' && **p <= '9') {
        const unsigned char* data;
        size_t len;
        return string_value(p, end, &data, &len);
    }
    unsigned char kind = *(*p)++;
    if (kind == 'i') {
        if (*p < end && **p == '-')
            ++*p;
        const unsigned char* start = *p;
        while (*p < end && **p >= '0' && **p <= '9')
            ++*p;
        if (*p == start || *p >= end || *(*p)++ != 'e')
            return false;
        return true;
    }
    if (kind != 'd' && kind != 'l')
        return false;
    while (*p < end && **p != 'e') {
        if (kind == 'd') {
            const unsigned char* data;
            size_t len;
            if (!string_value(p, end, &data, &len))
                return false;
        }
        if (!skip_value(p, end, depth + 1))
            return false;
    }
    if (*p >= end)
        return false;
    ++*p;
    return true;
}
struct fields {
    const unsigned char *tid, *ip, *type;
    size_t tidlen, iplen, typelen;
};
static bool fields(const void* buf, size_t len, struct fields* f) {
    const unsigned char* p = buf;
    const unsigned char* end = p + len;
    memset(f, 0, sizeof(*f));
    if (!len || *p++ != 'd')
        return false;
    while (p < end && *p != 'e') {
        const unsigned char *key, *value = NULL;
        size_t n, vlen = 0;
        if (!string_value(&p, end, &key, &n))
            return false;
        if (p < end && *p >= '0' && *p <= '9') {
            if (!string_value(&p, end, &value, &vlen))
                return false;
        } else if (!skip_value(&p, end, 1))
            return false;
        if (n == 1 && *key == 't') {
            if (f->tid)
                return false;
            f->tid = value;
            f->tidlen = vlen;
        }
        if (n == 1 && *key == 'y') {
            if (f->type)
                return false;
            f->type = value;
            f->typelen = vlen;
        }
        if (n == 2 && !memcmp(key, "ip", 2)) {
            if (f->ip)
                return false;
            f->ip = value;
            f->iplen = vlen;
        }
    }
    return p < end && *p++ == 'e' && p == end;
}
static bool public_ip(uint32_t address) {
    uint32_t ip = ntohl(address);
    return (ip >> 24) != 0 && (ip >> 24) != 10 && (ip >> 24) != 127 && (ip >> 28) < 14 &&
           (ip >> 16) != 0xa9fe && (ip >> 20) != 0xac1 && (ip >> 16) != 0xc0a8 &&
           (ip >> 22) != 0x191;
}
static void observe(const void* buf, size_t len, const struct sockaddr_in* from) {
    struct fields f;
    uint64_t now = SDL_GetTicks();
    if (!public_ip(from->sin_addr.s_addr) || !fields(buf, len, &f) || !f.type || f.typelen != 1 ||
        *f.type != 'r' || !f.tid || !f.tidlen || f.iplen != 6)
        return;
    bool matched = false;
    for (unsigned i = 0; i < 128; i++) {
        struct request* r = &requests[i];
        if (r->len == f.tidlen && r->ip == from->sin_addr.s_addr && r->port == from->sin_port &&
            now - r->sent < 30000 && !memcmp(r->tid, f.tid, f.tidlen))
        {
            r->len = 0;
            matched = true;
            break;
        }
    }
    if (!matched)
        return;
    struct pc_dht_endpoint ep;
    memcpy(&ep.address, f.ip, 4);
    ep.port = (f.ip[4] << 8) | f.ip[5];
    if (!ep.port || !public_ip(ep.address))
        return;
    uint32_t network = ntohl(from->sin_addr.s_addr) & 0xffffff00;
    unsigned slot = 16;
    for (unsigned i = 0; i < 16; i++)
        if (votes[i].network == network) {
            slot = i;
            break;
        }
    if (slot == 16)
        slot = vote_cursor++ % 16;
    votes[slot] = (struct vote){network, ep, now};
}
bool pc_dht_external_endpoint(struct pc_dht_endpoint* out) {
    uint64_t now = SDL_GetTicks();
    if (!out || fd < 0)
        return false;
    for (unsigned i = 0; i < 16; i++) {
        unsigned count = 0;
        for (unsigned j = 0; j < 16; j++)
            if (votes[j].network && now - votes[j].when < 600000 &&
                votes[j].endpoint.address == votes[i].endpoint.address &&
                votes[j].endpoint.port == votes[i].endpoint.port)
                count++;
        if (count >= 3) {
            *out = votes[i].endpoint;
            return true;
        }
    }
    return false;
}
static void node_cache(bool save) {
#ifndef PC_DHT_TEST_NO_BOOTSTRAP
    char* root = SDL_GetPrefPath(NULL, "melee-pc");
    if (!root)
        return;
    char path[2048];
    int n = snprintf(path, sizeof(path), "%sdht-nodes-v1", root);
    SDL_free(root);
    if (n < 0 || n >= (int)sizeof(path))
        return;
    struct sockaddr_in nodes[64];
    int count = 64, count6 = 0;
    if (save) {
        dht_get_nodes(nodes, &count, NULL, &count6);
        if (count <= 0)
            return; /* Preserve the last useful cache after failed bootstrap. */
        FILE* f = fopen(path, "wb");
        if (!f)
            return;
        for (int i = 0; i < count; i++)
            if (public_ip(nodes[i].sin_addr.s_addr) && nodes[i].sin_port) {
                fwrite(&nodes[i].sin_addr.s_addr, 1, 4, f);
                fwrite(&nodes[i].sin_port, 1, 2, f);
            }
        fclose(f);
    } else {
        FILE* f = fopen(path, "rb");
        if (!f)
            return;
        for (int i = 0; i < 64; i++) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            if (fread(&addr.sin_addr.s_addr, 1, 4, f) != 4 || fread(&addr.sin_port, 1, 2, f) != 2)
                break;
            if (public_ip(addr.sin_addr.s_addr) && addr.sin_port)
                dht_ping_node((struct sockaddr*)&addr, sizeof(addr));
        }
        fclose(f);
    }
#else
    (void)save;
#endif
}
struct resolver {
    SDL_AtomicInt done;
    struct sockaddr_in nodes[12];
    int count;
};
static struct resolver* resolver;
void pc_dht_sha1(const void* data, size_t len, unsigned char out[20]) {
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    while (len) {
        uint32_t n = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
        SHA1Update(&ctx, data, n);
        data = (const char*)data + n;
        len -= n;
    }
    SHA1Final(out, &ctx);
}
bool pc_dht_topic(
    enum pc_dht_mode m, const char* code, int band, int64_t minute, unsigned char out[20]) {
    char topic[128];
    int n;
    if (m == PC_DHT_DIRECT) {
        if (!code || !*code || strlen(code) >= sizeof(direct_code))
            return false;
        n = snprintf(topic, sizeof(topic), "meleepc/v1/direct/%s", code);
    } else if (m == PC_DHT_UNRANKED)
        n = snprintf(topic, sizeof(topic), "meleepc/v1/unranked/%lld", (long long)minute);
    else if (m == PC_DHT_RANKED && band >= 0)
        n = snprintf(topic, sizeof(topic), "meleepc/v1/ranked/%d/%lld", band, (long long)minute);
    else
        return false;
    if (n < 0 || n >= (int)sizeof(topic))
        return false;
    pc_dht_sha1(topic, n, out);
    return true;
}
int dht_random_bytes(void* buf, size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, buf, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ?
               (int)size :
               -1;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    return n == size ? (int)n : -1;
#endif
}
void dht_hash(
    void* out, int size, const void* a, int na, const void* b, int nb, const void* c, int nc) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    SHA1Init(&ctx);
    if (na > 0)
        SHA1Update(&ctx, a, na);
    if (nb > 0)
        SHA1Update(&ctx, b, nb);
    if (nc > 0)
        SHA1Update(&ctx, c, nc);
    SHA1Final(hash, &ctx);
    memset(out, 0, size);
    memcpy(out, hash, size < 20 ? size : 20);
}
int dht_blacklisted(const struct sockaddr* sa, int len) {
    (void)sa;
    (void)len;
    return 0;
}
int dht_sendto(
    int sock, const void* buf, int len, int flags, const struct sockaddr* to, int tolen) {
    struct fields f;
    if (to->sa_family == AF_INET && len > 0 && fields(buf, (size_t)len, &f) && f.tid &&
        f.tidlen <= 16 && f.type && f.typelen == 1 && *f.type == 'q')
    {
        const struct sockaddr_in* peer = (const struct sockaddr_in*)to;
        struct request* r = &requests[request_cursor++ % 128];
        r->ip = peer->sin_addr.s_addr;
        r->port = peer->sin_port;
        r->len = f.tidlen;
        r->sent = SDL_GetTicks();
        memcpy(r->tid, f.tid, f.tidlen);
    }
    return (int)sendto(sock, buf, len, flags, to, tolen);
}
static int resolve(void* arg) {
    struct resolver* job = arg;
    const char* hosts[] = {
        "router.bittorrent.com", "dht.transmissionbt.com", "router.utorrent.com"};
    for (unsigned i = 0; i < 3; i++) {
        struct addrinfo hints = {0}, *list = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (!getaddrinfo(hosts[i], "6881", &hints, &list)) {
            for (struct addrinfo* p = list; p && job->count < 12; p = p->ai_next)
                memcpy(&job->nodes[job->count++], p->ai_addr, sizeof(struct sockaddr_in));
            freeaddrinfo(list);
        }
    }
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}
static void values(void* ctx, int event, const unsigned char* hash, const void* data, size_t len) {
    (void)ctx;
    (void)hash;
    if (event != DHT_EVENT_VALUES || len % 6)
        return;
    const unsigned char* p = data;
    for (size_t i = 0; i < len; i += 6) {
        struct pc_dht_endpoint ep;
        memcpy(&ep.address, p + i, 4);
        ep.port = (p[i + 4] << 8) | p[i + 5];
        uint32_t ip = ntohl(ep.address);
        if (!ep.port || !ip || (ip >> 24) == 127 || (ip >> 28) >= 14)
            continue;
        unsigned j;
        for (j = 0; j < queue_count; j++)
            if (queue[j].address == ep.address && queue[j].port == ep.port)
                break;
        if (j == queue_count && queue_count < 64)
            queue[queue_count++] = ep;
    }
}
bool pc_dht_start(enum pc_dht_mode m, const char* code, int band, uint16_t port) {
    unsigned char id[20], topic[20];
    if (!pc_dht_topic(m, code, band, 0, topic))
        return false;
    pc_dht_stop();
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w))
        return false;
#endif
    fd = (intptr_t)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || fd > INT_MAX) {
        if (fd >= 0)
            CLOSE(fd);
        fd = -1;
        return false;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)))
        goto fail;
#ifdef _WIN32
    u_long on = 1;
    if (ioctlsocket(fd, FIONBIO, &on))
        goto fail;
    int size = sizeof(addr);
#else
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0)
        goto fail;
    socklen_t size = sizeof(addr);
#endif
    if (getsockname(fd, (struct sockaddr*)&addr, &size))
        goto fail;
    bound_port = ntohs(addr.sin_port);
    if (dht_random_bytes(id, sizeof(id)) < 0 ||
        dht_init((int)fd, -1, id, (const unsigned char*)"MP01") < 0)
        goto fail;
    memcpy(local_node_id, id, 20);
    mode = m;
    rating_band = band;
    snprintf(direct_code, sizeof(direct_code), "%s", code ? code : "");
    started = SDL_GetTicks();
    next_search = next_periodic = 0;
    queue_count = 0;
    memset(requests, 0, sizeof(requests));
    memset(votes, 0, sizeof(votes));
    request_cursor = vote_cursor = 0;
    node_cache(false);
    /* Offline harness disables only DNS bootstrap, never the socket path. */
#ifndef PC_DHT_TEST_NO_BOOTSTRAP
    if (!resolver) {
        resolver = calloc(1, sizeof(*resolver));
        if (resolver) {
            SDL_Thread* thread = SDL_CreateThread(resolve, "DHT bootstrap", resolver);
            if (thread)
                SDL_DetachThread(thread);
            else {
                free(resolver);
                resolver = NULL;
            }
        }
    }
#endif
    return true;
fail:
    CLOSE(fd);
    fd = -1;
    bound_port = 0;
    return false;
}
bool pc_dht_ready(void) {
    int good = 0, dubious = 0;
    if (fd < 0)
        return false;
    dht_nodes(AF_INET, &good, &dubious, NULL, NULL);
    return good >= 4 && good + dubious >= 30;
}
void pc_dht_poll(void) {
    if (fd < 0)
        return;
    if (resolver && SDL_GetAtomicInt(&resolver->done)) {
        for (int i = 0; i < resolver->count; i++)
            dht_ping_node((struct sockaddr*)&resolver->nodes[i], sizeof(struct sockaddr_in));
        free(resolver);
        resolver = NULL;
    }
    uint64_t now = SDL_GetTicks();
    time_t sleep = 1;
    for (unsigned i = 0; i < 64; i++) {
        unsigned char packet[4097];
        struct sockaddr_in from;
#ifdef _WIN32
        int len = sizeof(from);
#else
        socklen_t len = sizeof(from);
#endif
        int n =
            (int)recvfrom(fd, (char*)packet, sizeof(packet) - 1, 0, (struct sockaddr*)&from, &len);
        if (n < 0)
            break;
        packet[n] = 0; /* jech/dht requires accessible NUL terminator. */
        if (n && packet[0] == 'd') {
            observe(packet, n, &from);
            bool consumed =
                pc_dht_item_receive(packet, n, from.sin_addr.s_addr, ntohs(from.sin_port));
            if (fd < 0)
                return;
            if (!consumed)
                dht_periodic(packet, n, (struct sockaddr*)&from, len, &sleep, values, NULL);
        } else if (datagram) {
            struct pc_dht_endpoint ep = {from.sin_addr.s_addr, ntohs(from.sin_port)};
            datagram(packet, n, &ep, datagram_context);
            if (fd < 0)
                return;
        }
    }
    if (now >= next_periodic) {
        dht_periodic(NULL, 0, NULL, 0, &sleep, values, NULL);
        next_periodic = now + (sleep > 0 ? (uint64_t)sleep * 1000 : 100);
    }
    pc_dht_item_tick();
    if (fd < 0)
        return;
    if (now >= next_search && pc_dht_ready()) {
        int width = mode == PC_DHT_RANKED ? (now - started >= 90000    ? 2 :
                                                now - started >= 30000 ? 1 :
                                                                         0) :
                                            0;
        int64_t minute = (int64_t)time(NULL) / 60;
        for (int offset = -width; offset <= width; offset++)
            for (int age = 0; age < (mode == PC_DHT_DIRECT ? 1 : 2); age++) {
                unsigned char hash[20];
                if (pc_dht_topic(mode, direct_code, rating_band + offset, minute - age, hash))
                    dht_search(hash, age == 0 ? bound_port : 0, AF_INET, values, NULL);
            }
        next_search = now + 15000;
    }
}
bool pc_dht_next_candidate(struct pc_dht_endpoint* out) {
    if (!out || !queue_count)
        return false;
    *out = queue[0];
    memmove(queue, queue + 1, --queue_count * sizeof(*queue));
    return true;
}
void pc_dht_set_datagram_callback(pc_dht_datagram_fn fn, void* context) {
    datagram = fn;
    datagram_context = context;
}
intptr_t pc_dht_socket(void) {
    return fd;
}
uint16_t pc_dht_port(void) {
    return bound_port;
}
intptr_t pc_dht_take_socket(void) {
    intptr_t result = fd;
    if (fd >= 0) {
        node_cache(true);
        dht_uninit();
    }
    fd = -1;
    bound_port = 0;
    queue_count = 0;
    pc_dht_item_cancel();
    return result;
}
void pc_dht_stop(void) {
    intptr_t old = pc_dht_take_socket();
    if (old >= 0)
        CLOSE(old);
}

size_t pc_dht_item_seeds(struct pc_dht_endpoint* out, size_t capacity) {
    if (fd < 0 || !out)
        return 0;
    struct sockaddr_in nodes[64];
    int count = 64, count6 = 0;
    dht_get_nodes(nodes, &count, NULL, &count6);
    size_t n = (size_t)count < capacity ? (size_t)count : capacity;
    for (size_t i = 0; i < n; i++)
        out[i] = (struct pc_dht_endpoint){nodes[i].sin_addr.s_addr, ntohs(nodes[i].sin_port)};
    return n;
}
bool pc_dht_item_send(const void* data, size_t length, const struct pc_dht_endpoint* to) {
    if (fd < 0 || !to || length > 1600)
        return false;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = to->address;
    addr.sin_port = htons(to->port);
    return dht_sendto((int)fd, data, (int)length, 0, (struct sockaddr*)&addr, sizeof(addr)) ==
           (int)length;
}
void pc_dht_item_node_id(uint8_t out[20]) {
    memcpy(out, local_node_id, 20);
}
