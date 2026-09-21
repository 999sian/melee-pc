/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_match.h"
#include "net_dht.h"
#include "net_dht_item.h"
#include "net.h"
#include "net_lan.h"
#include "net_rank_session.h"
#include "pc.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET MatchSocket;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int MatchSocket;
#endif

extern const char* pc_get_net_name(void);
extern int pc_get_net_port(void);
extern const char* pc_lan_disc_id(void);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void pc_log_line(const char* fmt, ...) {
    (void)fmt;
}
#endif

#define MATCH_MAGIC 0x4d504d31u /* MPM1 */
#define MATCH_VERSION 2
#define RETRY_MS 250
#define TIMEOUT_MS 8000

#pragma pack(push, 1)
typedef struct MatchHello {
    uint32_t magic;
    uint8_t version, type, mode, reserved;
    uint64_t nonce;
    uint8_t public_key[32], compatibility[20], topic[20];
    char code[14];
    uint8_t signature[64];
} MatchHello;
typedef struct MatchOffer {
    uint32_t magic;
    uint8_t version, type, mode, reserved;
    uint64_t host_nonce, guest_nonce;
    uint8_t host_key[32], guest_key[32], compatibility[20], topic[20];
    uint32_t seed;
    uint8_t signature[64];
} MatchOffer;
typedef struct MatchAck {
    uint32_t magic;
    uint8_t version, type, mode, reserved;
    uint64_t host_nonce, guest_nonce;
    uint8_t host_key[32], guest_key[32], offer_hash[20];
    uint8_t signature[64];
} MatchAck;
#pragma pack(pop)

static PcNetIdentity identity;
static enum PcNetMatchMode mode;
static int state = PC_MATCH_FAIL;
static const char* failure = "not started";
static char target[14], opponent[14];
static uint8_t peer_key[32], compatibility[20], topic[20], offer_hash[20];
static uint64_t local_nonce, peer_nonce, deadline, next_send;
static struct pc_dht_endpoint peer;
static bool have_peer, host;
static uint32_t seed;
static int32_t start_frame = -1;
static MatchHello hello;
static MatchOffer offer;
static MatchAck ack;
static bool identity_loaded;
static const char* profile_error;
static char profile_directory[4096];
static bool handshake_done, barrier_sent, barrier_received;
static bool rank_begun, proofs_ready, ack_received, offer_received;
static int proof_step; /* local GET, local PUT, peer GET, ancestry GET, complete */
static bool proof_pending;
static bool recovery_immutable;
static uint8_t recovery_wire[PC_RANK_RECORD_BYTES];
static uint8_t local_public[PC_RANK_PUBLIC_BYTES];
static int publication; /* 0 idle, 1 waiting, 2 acknowledged, -1 failed */
static uint64_t publication_deadline;
static const char* publication_reason;
static bool publication_record;
static uint8_t publication_wire[PC_RANK_RECORD_BYTES];
static int64_t public_sequence(const uint8_t* p) {
    return (int64_t)((uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19]);
}
static void proof_result(const PcDhtItemResult* r, void* context) {
    (void)context;
    proof_pending = false;
    uint8_t genesis[PC_RANK_PUBLIC_BYTES];
    pc_rank_session_initial_public(genesis);
    unsigned player = host ? 0 : 1;
    if (proof_step == 3) {
        if (r->status != PC_DHT_ITEM_OK || !pc_rank_session_chain_record(r->value, r->value_length))
        {
            proof_step = -1;
            return;
        }
        if (!pc_rank_session_chain_target(NULL)) {
            proof_step = 4;
            proofs_ready = true;
        }
        return;
    }
    if (proof_step == 1) {
        if (r->status != PC_DHT_ITEM_OK || !r->acknowledgements) {
            proof_step = -1;
            return;
        }
        if (recovery_immutable) {
            recovery_immutable = false;
            return;
        }
        if (!pc_rank_session_proof(
                player, local_public, sizeof local_public, public_sequence(local_public)))
        {
            proof_step = -1;
            return;
        }
        proof_step = 2;
        return;
    }
    if (r->status != PC_DHT_ITEM_OK && r->status != PC_DHT_ITEM_NOT_FOUND) {
        proof_step = -1;
        return;
    }
    const void* value = r->status == PC_DHT_ITEM_NOT_FOUND ? genesis : r->value;
    size_t length = r->status == PC_DHT_ITEM_NOT_FOUND ? sizeof genesis : r->value_length;
    int64_t sequence = r->status == PC_DHT_ITEM_NOT_FOUND ? 0 : r->sequence;
    /* A failed prior publication may leave an older verified head in DHT.
     * Durable local history is authoritative for our own strictly newer
     * sequence; never overwrite an equal-sequence conflict or newer head. */
    if (proof_step == 0 &&
        ((r->status == PC_DHT_ITEM_NOT_FOUND && memcmp(local_public, genesis, sizeof genesis)) ||
            (r->status == PC_DHT_ITEM_OK && sequence >= 0 &&
                sequence < public_sequence(local_public))))
    {
        if (!pc_rank_session_latest_record(recovery_wire)) {
            proof_step = -1;
            return;
        }
        recovery_immutable = true;
        proof_step = 1;
        return;
    }
    if (proof_step == 0 &&
        (length != sizeof local_public || memcmp(value, local_public, sizeof local_public)))
    {
        proof_step = -1;
        return;
    }
    if (proof_step == 0 && public_sequence(local_public) > 0) {
        if (!pc_rank_session_latest_record(recovery_wire)) {
            proof_step = -1;
            return;
        }
        recovery_immutable = true;
        proof_step = 1;
        return;
    }
    if (!pc_rank_session_proof(proof_step == 0 ? player : 1 - player, value, length, sequence)) {
        proof_step = -1;
        return;
    }
    proof_step = proof_step == 0 ? 2 : pc_rank_session_chain_target(NULL) ? 3 : 4;
    proofs_ready = proof_step == 4;
}
static bool begin_rank(void) {
    if (mode != PC_MATCH_RANKED || rank_begun)
        return true;
    rank_begun = true;
    return pc_rank_session_begin(profile_directory, &identity, peer_key, host ? 0 : 1, seed) &&
           pc_rank_session_public_state(local_public);
}
static void poll_proofs(void) {
    if (!rank_begun || proofs_ready || proof_pending || proof_step < 0 || !pc_dht_ready())
        return;
    const char* salt = PC_RANK_BEP44_SALT;
    if (proof_step == 3) {
        uint8_t locator[20];
        if (!pc_rank_session_chain_target(locator)) {
            proof_step = -1;
            return;
        }
        proof_pending = pc_dht_item_get_immutable(locator, proof_result, NULL);
    } else if (proof_step == 1 && recovery_immutable)
        proof_pending =
            pc_dht_item_put_immutable(recovery_wire, sizeof recovery_wire, proof_result, NULL);
    else if (proof_step == 1)
        proof_pending = pc_dht_item_put(&identity, salt, strlen(salt),
            public_sequence(local_public), local_public, sizeof local_public, proof_result, NULL);
    else
        proof_pending = pc_dht_item_get(proof_step == 0 ? identity.public_key : peer_key, salt,
            strlen(salt), 0, proof_result, NULL);
}
static void publication_result(const PcDhtItemResult* r, void* context) {
    (void)context;
    if (publication_record && r->status == PC_DHT_ITEM_OK && r->acknowledgements) {
        publication_record = false;
        proof_pending = false;
        return;
    }
    publication = r->status == PC_DHT_ITEM_OK && r->acknowledgements ? 2 : -1;
    publication_reason = publication == 2 ?
                             "rank saved and published" :
                             "rank saved locally; publication failed, retry available";
}

static bool load_identity(void) {
    if (identity_loaded)
        return true;
    char* dir = SDL_GetPrefPath(NULL, "melee-pc");
    const char* name = pc_get_net_name();
    bool ok = dir && pc_identity_load(&identity, dir, name && *name ? name : "PLAYER");
    if (dir && ok)
        snprintf(profile_directory, sizeof profile_directory, "%s", dir);
    SDL_free(dir);
    identity_loaded = ok;
    profile_error = ok ? NULL : "identity unavailable";
    return ok;
}

static void digest(void) {
    char text[256];
    int n = snprintf(text, sizeof text, "%s\n%s", pc_app_rev(), pc_lan_disc_id());
    pc_dht_sha1(text, n > 0 ? (size_t)n : 0, compatibility);
}
static bool send_packet(const void* p, size_t n, const struct pc_dht_endpoint* ep) {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ep->address;
    to.sin_port = htons(ep->port);
    return sendto((MatchSocket)pc_dht_socket(), p, (int)n, 0, (struct sockaddr*)&to, sizeof to) ==
           (int)n;
}
static bool key_code_matches(const uint8_t key[32], const char* code) {
    if (!pc_identity_code_valid(code))
        return false;
    uint8_t h[20];
    pc_dht_sha1(key, 32, h);
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    unsigned bits = (unsigned)h[0] << 12 | (unsigned)h[1] << 4 | h[2] >> 4;
    size_t n = strlen(code);
    for (int i = 0; i < 4; i++)
        if (code[n - 4 + i] != a[(bits >> (15 - 5 * i)) & 31])
            return false;
    return true;
}
static bool signed_ok(const uint8_t key[32], const uint8_t sig[64], const void* p, size_t n) {
    return pc_identity_verify(key, sig, p, n - 64);
}
static void sign_packet(void* p, size_t n) {
    pc_identity_sign(&identity, (uint8_t*)p + n - 64, p, n - 64);
}
static void fail(const char* why) {
    state = PC_MATCH_FAIL;
    failure = why;
    pc_dht_stop();
    if (pc_net_active())
        pc_net_disconnect();
    if (mode == PC_MATCH_RANKED && pc_rank_session_state(NULL) != PC_RANK_SESSION_SAVED)
        pc_rank_session_abort(why);
}

static void pairing_topic(enum PcNetMatchMode m, const char* direct, uint8_t out[20]) {
    char text[64];
    int n = m == PC_MATCH_DIRECT ?
                snprintf(text, sizeof text, "meleepc/match/v1/direct/%s", direct) :
                snprintf(text, sizeof text,
                    m == PC_MATCH_RANKED ? "meleepc/match/v1/ranked" : "meleepc/match/v1/unranked");
    pc_dht_sha1(text, n > 0 ? (size_t)n : 0, out);
}

static bool accept_ack(const MatchAck* a) {
    if (ntohl(a->magic) != MATCH_MAGIC || a->version != MATCH_VERSION || a->type != 'A' ||
        a->mode != (uint8_t)mode || a->host_nonce != local_nonce || a->guest_nonce != peer_nonce ||
        memcmp(a->host_key, identity.public_key, 32) || memcmp(a->guest_key, peer_key, 32) ||
        memcmp(a->offer_hash, offer_hash, 20) || !signed_ok(peer_key, a->signature, a, sizeof *a))
        return false;
    ack = *a;
    ack_received = true;
    if (mode == PC_MATCH_RANKED && !proofs_ready)
        return true;
    intptr_t fd = pc_dht_take_socket();
    char ip[INET_ADDRSTRLEN];
    struct in_addr addr = {peer.address};
    inet_ntop(AF_INET, &addr, ip, sizeof ip);
    pc_log_line(
        "match: accept_ack -> connecting socket as host to %s:%u (seed=%u)", ip, peer.port, seed);
    if (!pc_net_connect_socket(fd, ip, peer.port, 0, seed)) {
#ifdef _WIN32
        closesocket((SOCKET)fd);
#else
        close((int)fd);
#endif
        fail("connection failed");
        return true;
    }
    state = PC_MATCH_CONNECT;
    pc_net_set_datagram_handler(NULL);
    return true;
}
static bool after_handoff(const void* data, size_t n, uint32_t address, uint16_t port) {
    if (!host && address == peer.address && port == peer.port && n == sizeof(MatchOffer)) {
        const MatchOffer* o = data;
        uint8_t hash[20];
        pc_dht_sha1(o, sizeof *o, hash);
        if (ntohl(o->magic) == MATCH_MAGIC && o->version == MATCH_VERSION && o->type == 'O' &&
            o->mode == (uint8_t)mode && o->host_nonce == peer_nonce &&
            o->guest_nonce == local_nonce && !memcmp(o->host_key, peer_key, 32) &&
            !memcmp(o->guest_key, identity.public_key, 32) && !memcmp(hash, offer_hash, 20) &&
            signed_ok(peer_key, o->signature, o, sizeof *o))
        {
            for (int p = 0; p < 3; p++) {
                pc_net_send_datagram(&ack, sizeof ack, address, port);
            }
            return true;
        }
    }
    return false;
}
static void receive(const void* data, size_t n, const struct pc_dht_endpoint* ep, void* unused) {
    (void)unused;
    if (n == sizeof(MatchHello)) {
        const MatchHello* h = data;
        const char* terminator = memchr(h->code, '\0', sizeof h->code);
        if (ntohl(h->magic) != MATCH_MAGIC || h->version != MATCH_VERSION || h->type != 'H' ||
            h->mode != (uint8_t)mode || h->nonce == local_nonce ||
            memcmp(h->compatibility, compatibility, 20) || memcmp(h->topic, topic, 20) ||
            !terminator || !key_code_matches(h->public_key, h->code) ||
            (mode == PC_MATCH_DIRECT && target[0] && strcmp(h->code, target)) ||
            !signed_ok(h->public_key, h->signature, h, sizeof *h))
            return;
        bool fresh = !have_peer;
        if (have_peer && (h->nonce != peer_nonce || memcmp(h->public_key, peer_key, 32) ||
                             ep->address != peer.address || ep->port != peer.port))
            return;
        memcpy(peer_key, h->public_key, 32);
        peer_nonce = h->nonce;
        peer = *ep;
        have_peer = true;
        failure = NULL;
        memcpy(opponent, h->code, sizeof opponent);
        host = memcmp(identity.public_key, peer_key, 32) < 0;
        uint32_t rip = ntohl(ep->address);
        pc_log_line("match: recv valid MatchHello from %u.%u.%u.%u:%u (peer=%s host=%d fresh=%d)",
            rip >> 24, (rip >> 16) & 0xFF, (rip >> 8) & 0xFF, rip & 0xFF, ep->port, h->code, host,
            fresh);
        if (fresh) {
            for (int p = 0; p < 3; p++) {
                send_packet(&hello, sizeof hello, ep); /* answer one-sided discovery burst */
            }
        }
        if (fresh)
            deadline = SDL_GetTicks() + (mode == PC_MATCH_RANKED ? 90000 : TIMEOUT_MS);
        if (host) {
            memset(&offer, 0, sizeof offer);
            offer.magic = htonl(MATCH_MAGIC);
            offer.version = MATCH_VERSION;
            offer.type = 'O';
            offer.mode = (uint8_t)mode;
            offer.host_nonce = local_nonce;
            offer.guest_nonce = peer_nonce;
            memcpy(offer.host_key, identity.public_key, 32);
            memcpy(offer.guest_key, peer_key, 32);
            memcpy(offer.compatibility, compatibility, 20);
            memcpy(offer.topic, topic, 20);
            if (!seed && !pc_identity_random(&seed, sizeof seed)) {
                fail("random source failed");
                return;
            }
            offer.seed = htonl(seed);
            sign_packet(&offer, sizeof offer);
            pc_dht_sha1(&offer, sizeof offer, offer_hash);
            if (!begin_rank()) {
                fail("rank history unavailable");
                return;
            }
            pc_log_line("match: sending MatchOffer to peer (seed=%u)", seed);
            send_packet(&offer, sizeof offer, &peer);
            next_send = SDL_GetTicks() + RETRY_MS;
        }
    } else if (n == sizeof(MatchOffer)) {
        const MatchOffer* o = data;
        if (ntohl(o->magic) != MATCH_MAGIC || o->version != MATCH_VERSION || o->type != 'O' ||
            !have_peer || ep->address != peer.address || ep->port != peer.port ||
            o->mode != (uint8_t)mode || o->guest_nonce != local_nonce ||
            o->host_nonce != peer_nonce || memcmp(o->guest_key, identity.public_key, 32) ||
            memcmp(o->host_key, peer_key, 32) || memcmp(o->compatibility, compatibility, 20) ||
            memcmp(o->topic, topic, 20) || !signed_ok(peer_key, o->signature, o, sizeof *o))
            return;
        if (host || (offer_received && memcmp(&offer, o, sizeof offer)))
            return;
        offer_received = true;
        seed = ntohl(o->seed);
        offer = *o;
        pc_dht_sha1(o, sizeof *o, offer_hash);
        memset(&ack, 0, sizeof ack);
        ack.magic = htonl(MATCH_MAGIC);
        ack.version = MATCH_VERSION;
        ack.type = 'A';
        ack.mode = (uint8_t)mode;
        ack.host_nonce = peer_nonce;
        ack.guest_nonce = local_nonce;
        memcpy(ack.host_key, peer_key, 32);
        memcpy(ack.guest_key, identity.public_key, 32);
        memcpy(ack.offer_hash, offer_hash, 20);
        sign_packet(&ack, sizeof ack);
        if (!begin_rank()) {
            fail("rank history unavailable");
            return;
        }
        if (mode == PC_MATCH_RANKED && !proofs_ready)
            return;
        for (int p = 0; p < 3; p++) {
            send_packet(&ack, sizeof ack, ep);
        }
        intptr_t fd = pc_dht_take_socket();
        char ip[INET_ADDRSTRLEN];
        struct in_addr addr = {ep->address};
        inet_ntop(AF_INET, &addr, ip, sizeof ip);
        pc_log_line("match: recv valid MatchOffer -> sending MatchAck and connecting socket as "
                    "guest to %s:%u (seed=%u)",
            ip, ep->port, seed);
        if (!pc_net_connect_socket(fd, ip, ep->port, 1, seed)) {
#ifdef _WIN32
            closesocket((SOCKET)fd);
#else
            close((int)fd);
#endif
            fail("connection failed");
            return;
        }
        host = false;
        state = PC_MATCH_CONNECT;
        pc_net_set_datagram_handler(after_handoff);
    } else if (host && n == sizeof(MatchAck) && ep->address == peer.address &&
               ep->port == peer.port)
        accept_ack(data);
}

bool pc_net_match_start(enum PcNetMatchMode m, const char* code) {
    pc_net_match_stop();
    failure = NULL;
    mode = m;
    start_frame = -1;
    seed = 0;
    opponent[0] = 0;
    have_peer = false;
    handshake_done = barrier_sent = barrier_received = false;
    publication = 0;
    publication_reason = NULL;
    rank_begun = proofs_ready = ack_received = offer_received = proof_pending = false;
    recovery_immutable = false;
    proof_step = 0;
    identity_loaded = false; /* Reload display code from the same persistent key. */
    if (code && *code && (!pc_identity_code_valid(code) || strlen(code) >= sizeof target)) {
        fail("invalid connect code");
        return false;
    }
    snprintf(target, sizeof target, "%s", code ? code : "");
    if (!load_identity() || !pc_identity_random(&local_nonce, sizeof local_nonce)) {
        fail("identity unavailable");
        return false;
    }
    digest();
    const char* direct = target[0] ? target : identity.code;
    pairing_topic(m, direct, topic);
    if (!pc_dht_start((enum pc_dht_mode)m, direct, 0, (uint16_t)pc_get_net_port())) {
        fail("DHT unavailable");
        return false;
    }
    memset(&hello, 0, sizeof hello);
    hello.magic = htonl(MATCH_MAGIC);
    hello.version = MATCH_VERSION;
    hello.type = 'H';
    hello.mode = (uint8_t)m;
    hello.nonce = local_nonce;
    memcpy(hello.public_key, identity.public_key, 32);
    memcpy(hello.compatibility, compatibility, 20);
    memcpy(hello.topic, topic, 20);
    memcpy(hello.code, identity.code, sizeof hello.code);
    sign_packet(&hello, sizeof hello);
    pc_dht_set_datagram_callback(receive, NULL);
    state = PC_MATCH_SEARCH;
    deadline = 0;
    next_send = 0;
    return true;
}
void pc_net_match_poll(void) {
    uint64_t now = SDL_GetTicks();
    if (state == PC_MATCH_SEARCH) {
        pc_dht_poll();
        if (state != PC_MATCH_SEARCH)
            return;
        if (mode == PC_MATCH_RANKED) {
            poll_proofs();
            if (proof_step < 0) {
                fail("rank public history verification failed");
                return;
            }
            if (proofs_ready) {
                if (host && ack_received) {
                    accept_ack(&ack);
                    if (state != PC_MATCH_SEARCH)
                        return;
                } else if (!host && offer_received) {
                    receive(&offer, sizeof offer, &peer, NULL);
                    if (state != PC_MATCH_SEARCH)
                        return;
                }
            }
        }
        struct pc_dht_endpoint ep;
        while (pc_dht_next_candidate(&ep)) {
            uint32_t cip = ntohl(ep.address);
            pc_log_line("match: sending MatchHello to %u.%u.%u.%u:%u (target=%s)", cip >> 24,
                (cip >> 16) & 0xFF, (cip >> 8) & 0xFF, cip & 0xFF, ep.port, target);
            for (int p = 0; p < 3; p++) {
                send_packet(&hello, sizeof hello, &ep);
            }
        }
        if (have_peer && now >= next_send) {
            send_packet(
                host ? (void*)&offer : (void*)&hello, host ? sizeof offer : sizeof hello, &peer);
            next_send = now + RETRY_MS;
        }
        if (have_peer && now >= deadline) {
            if (mode == PC_MATCH_RANKED) {
                fail("rank proof pairing timed out");
                return;
            }
            have_peer = false;
            peer_nonce = 0;
            memset(peer_key, 0, sizeof peer_key);
            opponent[0] = 0;
            seed = 0;
            next_send = 0; /* peer attempt expired: remain queued in DHT */
            failure = "Could not connect to opponent. Searching again...";
        }
    } else if (state == PC_MATCH_CONNECT) {
        pc_net_poll();
        if (!handshake_done)
            handshake_done = host ? pc_net_host_match(seed, &start_frame) :
                                    pc_net_guest_wait_match(&seed, &start_frame);
        if (handshake_done && !barrier_sent) {
            if (!pc_net_send_reliable(0x11, NULL, 0)) {
                state = PC_MATCH_FAIL;
                failure = "ready barrier not sent";
            } else
                barrier_sent = true;
        }
        if (barrier_sent && !barrier_received) {
            uint8_t type, buf[256];
            int n;
            while ((n = pc_net_recv_reliable(&type, buf, sizeof buf)) >= 0) {
                if (type == 0x11 && n == 0)
                    barrier_received = true;
                else if (mode == PC_MATCH_RANKED)
                    pc_rank_session_receive(type, buf, n);
            }
        }
        if (barrier_received) {
            if (mode == PC_MATCH_RANKED) {
                pc_rank_session_poll();
                const char* why = NULL;
                int rs = pc_rank_session_state(&why);
                if (rs == PC_RANK_SESSION_FAILED) {
                    fail(why);
                    return;
                }
                if (rs == PC_RANK_SESSION_PLAY) {
                    if (pc_net_frame() > start_frame) {
                        fail("late ranked ready barrier");
                        return;
                    }
                    state = PC_MATCH_READY;
                    pc_net_set_datagram_handler(NULL);
                }
            } else if (pc_net_frame() > start_frame) {
                state = PC_MATCH_FAIL;
                failure = "late ready barrier";
            } else {
                state = PC_MATCH_READY;
                pc_net_set_datagram_handler(NULL);
            }
        }
        if (pc_net_handshake_state() == 3) {
            state = PC_MATCH_FAIL;
            failure = "match handshake failed";
        }
    }
}
void pc_net_match_stop(void) {
    pc_dht_stop();
    publication = 0;
    publication_reason = NULL;
    proof_pending = false;
    pc_rank_session_stop();
    pc_net_set_datagram_handler(NULL);
    if (pc_net_active())
        pc_net_disconnect();
    state = PC_MATCH_FAIL;
    failure = "cancelled";
}
int pc_net_match_state(const char** why) {
    if (why)
        *why = failure;
    return state;
}
bool pc_net_match_is_host(void) {
    return host;
}
int32_t pc_net_match_start_frame(void) {
    return start_frame;
}
uint32_t pc_net_match_seed(void) {
    return seed;
}
const char* pc_net_match_local_code(void) {
    return load_identity() ? identity.code : "";
}
const char* pc_net_match_profile_error(void) {
    load_identity();
    return profile_error;
}
const char* pc_net_match_profile_directory(void) {
    return load_identity() ? profile_directory : "";
}
enum PcNetMatchMode pc_net_match_mode(void) {
    return mode;
}
const char* pc_net_match_opponent_code(void) {
    return opponent;
}
const PcNetIdentity* pc_net_match_identity(void) {
    return &identity;
}
const uint8_t* pc_net_match_peer_key(void) {
    return peer_key;
}

/* Durable append is retained even when publication fails. Retry after leaving
 * the game; this deliberately disconnects before starting another DHT socket. */
bool pc_net_match_publish_rank(void) {
    if (publication == 1 || !pc_rank_session_record() ||
        !pc_rank_session_public_state(local_public))
        return false;
    if (pc_net_active())
        pc_net_disconnect();
    pc_dht_stop();
    pc_net_set_datagram_handler(NULL);
    const PcNetRankRecord* record = pc_rank_session_record();
    PcNetRankSet set;
    if (!pc_rank_set(record->games, record->game_count, &set) ||
        !pc_rank_encode(record, publication_wire))
        return false;
    publication_record = set.winner == (host ? 0 : 1);
    publication = 1;
    proof_pending = false;
    publication_deadline = SDL_GetTicks() + 60000;
    publication_reason = "rank saved locally; publishing";
    if (!pc_dht_start(PC_DHT_RANKED, identity.code, 0, (uint16_t)pc_get_net_port())) {
        publication = -1;
        publication_reason = "rank saved locally; DHT unavailable, retry available";
        return false;
    }
    return true;
}
void pc_net_match_poll_publication(void) {
    if (publication != 1)
        return;
    pc_dht_poll();
    if (publication == 1 && SDL_GetTicks() >= publication_deadline) {
        publication = -1;
        publication_reason = "rank saved locally; publication timed out, retry available";
    }
    if (publication != 1) {
        pc_dht_stop();
        return;
    }
    if (!proof_pending && pc_dht_ready()) {
        const char* salt = PC_RANK_BEP44_SALT;
        if (publication_record)
            proof_pending = pc_dht_item_put_immutable(
                publication_wire, sizeof publication_wire, publication_result, NULL);
        else
            proof_pending =
                pc_dht_item_put(&identity, salt, strlen(salt), public_sequence(local_public),
                    local_public, sizeof local_public, publication_result, NULL);
    }
}
int pc_net_match_publication(const char** reason) {
    if (reason)
        *reason = publication_reason;
    return publication;
}
