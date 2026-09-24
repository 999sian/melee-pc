/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay wire codec: byte order, headers, pad conversion, and the
 * per-datagram authentication tag (net_internal.h). */
#include "compat.h"
#include "monocypher.h"
#include "pc/net_internal.h"

#include <string.h>

/* ---- helpers ---------------------------------------------------------- */

static int8_t at_rest(int8_t v) {
    /* Slippi's clamp: idle stick noise must not look like a new input. */
    return v >= -2 && v <= 2 ? 0 : v;
}

void to_wire(WirePad* w, const PADStatus* p) {
    w->button = p->button;
    w->stickX = at_rest(p->stickX);
    w->stickY = at_rest(p->stickY);
    w->substickX = at_rest(p->substickX);
    w->substickY = at_rest(p->substickY);
    w->triggerLeft = p->triggerLeft;
    w->triggerRight = p->triggerRight;
}

void from_wire(PADStatus* p, const WirePad* w) {
    memset(p, 0, sizeof *p);
    p->button = w->button;
    p->stickX = w->stickX;
    p->stickY = w->stickY;
    p->substickX = w->substickX;
    p->substickY = w->substickY;
    p->triggerLeft = w->triggerLeft;
    p->triggerRight = w->triggerRight;
    p->err = PAD_ERR_NONE;
}

uint32_t fnv1a(uint32_t h, const void* data, size_t n) {
    const uint8_t* b = data;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* ---- wire codec -------------------------------------------------------
 * be*() rewrite one field between host and big-endian order in place: read
 * the bytes as big-endian, store the value in host order. That is its own
 * inverse, so each wire_*() below serves both send and receive. */
static uint16_t get16(const uint8_t* p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)get16(p) << 16 | get16(p + 2);
}

static void be16(void* p) {
    uint16_t v = get16(p);
    memcpy(p, &v, sizeof v);
}

static void be32(void* p) {
    uint32_t v = get32(p);
    memcpy(p, &v, sizeof v);
}

static void be64(void* p) {
    uint64_t v = (uint64_t)get32(p) << 32 | get32((const uint8_t*)p + 4);
    memcpy(p, &v, sizeof v);
}

void wire_hdr(Hdr* h) {
    be32(&h->session);
}

/* Bodies only: the header is converted once by recv_inputs / the senders. */
void wire_packet(Packet* pk) {
    be16(&pk->seq);
    be32(&pk->newest);
    be32(&pk->first);
    be32(&pk->ck_frame);
    be32(&pk->ck);
    for (int i = 0; i < pk->count && i < REDUNDANCY; i++) {
        be16(&pk->pads[i].button);
    }
}

/* Pads on the wire, after wire_packet: each one XORed against the pad before
 * it (the first against zero), sent as a mask byte whose bit j says byte j of
 * that XOR is nonzero, then those bytes in order. Melee pads repeat frame to
 * frame, so a held pad costs one byte instead of eight and a full 32-frame
 * window of a player holding shield is 40 bytes, not 256. GGRS does the same
 * with XOR plus RLE, GGPO with a list of changed bits; this form keeps every
 * pad byte-aligned and its worst case (all eight bytes changing every frame)
 * one byte a pad over raw. */
size_t pads_encode(const WirePad* pads, int count, uint8_t* out) {
    uint8_t prev[sizeof(WirePad)] = {0};
    size_t n = 0;
    for (int i = 0; i < count; i++) {
        uint8_t cur[sizeof(WirePad)];
        memcpy(cur, &pads[i], sizeof cur);
        uint8_t* mask = &out[n++];
        *mask = 0;
        for (int j = 0; j < (int)sizeof cur; j++) {
            uint8_t x = cur[j] ^ prev[j];
            if (x != 0) {
                *mask |= (uint8_t)(1u << j);
                out[n++] = x;
            }
        }
        memcpy(prev, cur, sizeof prev);
    }
    return n;
}

/* Bytes the encoded pads take in `in`, or -1 when `len` cannot hold them. */
int pads_wire_len(const uint8_t* in, int len, int count) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (n >= len) {
            return -1;
        }
        n += 1 + __builtin_popcount(in[n]);
    }
    return n <= len ? n : -1;
}

/* pads_encode's inverse; false unless the pads fill exactly `len` bytes. */
bool pads_decode(const uint8_t* in, int len, int count, WirePad* pads) {
    if (pads_wire_len(in, len, count) != len) {
        return false;
    }
    uint8_t prev[sizeof(WirePad)] = {0};
    int n = 0;
    for (int i = 0; i < count; i++) {
        uint8_t mask = in[n++];
        for (int j = 0; j < (int)sizeof prev; j++) {
            if (mask & (1u << j)) {
                prev[j] ^= in[n++];
            }
        }
        memcpy(&pads[i], prev, sizeof prev);
    }
    return true;
}

void wire_ack(Ack* a) {
    be16(&a->seq);
    be32(&a->frame);
}

void wire_rel(Rel* r) {
    be16(&r->len);
}

void wire_rules(Rules* ru) {
    be32(&ru->seed);
    be32(&ru->start_frame);
    be64(&ru->nonce);
    be32(&ru->game.unk_14);
    be64(&ru->item_mask);
    be32(&ru->stage_mask);
    be32(&ru->unlock_hash);
    be32(&ru->hash);
}

void wire_ready(Ready* rd) {
    be64(&rd->nonce);
    be64(&rd->echo);
    be32(&rd->unlock_hash);
    be32(&rd->start_frame);
    be32(&rd->hash);
}

/* Hash of a handshake payload's wire image (everything before .hash) with
 * the session id folded in after it, big-endian like every wire field. The
 * session binding is what stops a captured RULES/READY from validating in a
 * later session; the nonces ride in the images themselves (Rules.nonce,
 * Ready.nonce/.echo) and are checked by net_handshake.c. */
static uint32_t hs_hash(const void* image, size_t n, uint32_t session) {
    uint8_t be[4] = {(uint8_t)(session >> 24), (uint8_t)(session >> 16), (uint8_t)(session >> 8),
        (uint8_t)session};
    return fnv1a(fnv1a(2166136261u, image, n), be, sizeof be);
}

uint32_t rules_hash(Rules ru, uint32_t session) {
    wire_rules(&ru);
    return hs_hash(&ru, offsetof(Rules, hash), session);
}

uint32_t ready_hash(Ready rd, uint32_t session) {
    wire_ready(&rd);
    return hs_hash(&rd, offsetof(Ready, hash), session);
}

/* ---- datagram authentication ------------------------------------------
 * The key is a BLAKE2b of the session id and both handshake nonces, and the
 * tag is a keyed BLAKE2b of the whole datagram truncated to NET_MAC_LEN. It
 * covers the header as well as the body, so neither a forged input packet
 * nor a bit flipped inside a reliable message -- which is a delay change or
 * a scene exit, with no integrity of its own -- survives the trip.
 *
 * Binding the session id is not what makes a captured datagram useless in
 * the next session; the nonces are. The host draws a fresh one per session
 * and the guest answers with its own, so two runs between the same peers
 * derive unrelated keys even in the (2^-32) case where the session id
 * repeats, and a recording of one session verifies under no other key. */
static const char KEY_LABEL[] = "melee-pc netplay session key v1";
static const char KEY_LABEL_DIRECT[] = "melee-pc netplay direct key v1";
static uint8_t s_key[32];
static bool s_key_on;     /* s_key holds a key for the session in progress */
static bool s_key_pinned; /* from MELEE_NET_KEY: the handshake must not rekey */

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

void net_key_session(uint64_t host_nonce, uint64_t guest_nonce) {
    if (s_key_pinned) {
        return;
    }
    /* Host first, guest second whichever side is deriving: the two peers
     * must feed the same bytes in the same order, and local/remote is the
     * one ordering they disagree about. */
    uint8_t m[sizeof KEY_LABEL - 1 + 20], *p = m + sizeof KEY_LABEL - 1;
    memcpy(m, KEY_LABEL, sizeof KEY_LABEL - 1);
    put32(p, net.session);
    put64(p + 4, host_nonce);
    put64(p + 12, guest_nonce);
    crypto_blake2b(s_key, sizeof s_key, m, sizeof m);
    crypto_wipe(m, sizeof m);
    s_key_on = true;
}

void net_key_direct(const char* secret) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, sizeof s_key);
    crypto_blake2b_update(&ctx, (const uint8_t*)KEY_LABEL_DIRECT, sizeof KEY_LABEL_DIRECT - 1);
    crypto_blake2b_update(&ctx, (const uint8_t*)secret, strlen(secret));
    crypto_blake2b_final(&ctx, s_key);
    crypto_wipe(&ctx, sizeof ctx);
    s_key_on = s_key_pinned = true;
}

void net_key_clear(void) {
    crypto_wipe(s_key, sizeof s_key);
    s_key_on = s_key_pinned = false;
}

bool net_key_ready(void) {
    return s_key_on;
}

bool net_key_pinned(void) {
    return s_key_pinned;
}

void net_mac_stamp(void* buf, size_t len) {
    uint8_t* mac = (uint8_t*)buf + len;
    if (!s_key_on) {
        memset(mac, 0, NET_MAC_LEN);
        return;
    }
    crypto_blake2b_keyed(mac, NET_MAC_LEN, s_key, sizeof s_key, buf, len);
}

bool net_mac_ok(const void* buf, size_t len) {
    if (!s_key_on) {
        return false;
    }
    uint8_t want[NET_MAC_LEN];
    crypto_blake2b_keyed(want, NET_MAC_LEN, s_key, sizeof s_key, buf, len);
    const uint8_t* got = (const uint8_t*)buf + len;
    /* Compared without an early exit: a tag is rejected in the same time
     * whichever byte of it is wrong, so resends cannot be timed into a
     * byte-at-a-time search for one. */
    unsigned diff = 0;
    for (size_t i = 0; i < NET_MAC_LEN; i++) {
        diff |= (unsigned)(want[i] ^ got[i]);
    }
    crypto_wipe(want, sizeof want);
    return diff == 0;
}

/* A header in wire order, ready to send. */
Hdr hdr(uint8_t magic) {
    Hdr h = {magic, WIRE_VERSION, net.session, (uint8_t)net.local};
    wire_hdr(&h);
    return h;
}

bool addr_eq(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (a->ss_family != b->ss_family) {
        return false;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6*)a,
                                  *y = (const struct sockaddr_in6*)b;
        return x->sin6_port == y->sin6_port &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr) == 0;
    }
    const struct sockaddr_in *x = (const struct sockaddr_in*)a, *y = (const struct sockaddr_in*)b;
    return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
}
