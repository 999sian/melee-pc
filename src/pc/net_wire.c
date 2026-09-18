/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay wire codec: byte order, headers, pad conversion (net_internal.h). */
#include "compat.h"
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
    return (uint16_t) (p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t) get16(p) << 16 | get16(p + 2);
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
    uint64_t v = (uint64_t) get32(p) << 32 | get32((const uint8_t*) p + 4);
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
    be32(&ru->hash);
}

void wire_ready(Ready* rd) {
    be64(&rd->nonce);
    be64(&rd->echo);
    be32(&rd->hash);
}

/* Hash of a handshake payload's wire image (everything before .hash) with
 * the session id folded in after it, big-endian like every wire field. The
 * session binding is what stops a captured RULES/READY from validating in a
 * later session; the nonces ride in the images themselves (Rules.nonce,
 * Ready.nonce/.echo) and are checked by net_handshake.c. */
static uint32_t hs_hash(const void* image, size_t n, uint32_t session) {
    uint8_t be[4] = { (uint8_t) (session >> 24), (uint8_t) (session >> 16),
                      (uint8_t) (session >> 8), (uint8_t) session };
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

/* A header in wire order, ready to send. */
Hdr hdr(uint8_t magic) {
    Hdr h = { magic, WIRE_VERSION, net.session, (uint8_t) net.local };
    wire_hdr(&h);
    return h;
}

bool addr_eq(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (a->ss_family != b->ss_family) {
        return false;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6*) a, *y = (const struct sockaddr_in6*) b;
        return x->sin6_port == y->sin6_port &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr) == 0;
    }
    const struct sockaddr_in *x = (const struct sockaddr_in*) a, *y = (const struct sockaddr_in*) b;
    return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
}
