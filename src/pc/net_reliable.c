/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay reliable channel: two independent stop-and-wait lanes so a full
 * or slow caller lane never holds up RULES/READY. Lane 0 carries the match
 * handshake (types < 0x10, consumed inline by net_handshake.c), lane 1 the
 * caller's messages (types >= 0x10, queued for pc_net_recv_reliable) except
 * REL_RESUME and REL_DELAY, which net.c and net_sync.c consume inline the
 * same way (REL_RESUME arrives while the game thread is parked in a stall
 * and REL_DELAY mid-match, where no caller is draining). Each lane has one
 * 'R' in flight, resent every 250 ms until its 'K' arrives,
 * 4 queued behind it and its own sequence space.
 *
 * Wire: Rel.seq / RelAck.seq bit 7 is the lane, bits 0-6 the lane's 7-bit
 * sequence number, so the receiver demultiplexes without a new field.
 * Sequence arithmetic is modulo 128: the next expected seq is processed
 * and acked, one of the 8 before it is re-acked (the ack was lost), any
 * other is dropped. Transmit side under tx_lock, receive side game thread
 * only. */
#include "compat.h"
#include "pc/net_internal.h"

#include <SDL3/SDL_timer.h>
#include <string.h>

#define REL_QUEUE 4
#define REL_RESEND_NS 250000000ull
#define REL_LANES 2
#define REL_LANE_BIT 0x80
#define REL_SEQ_MASK 0x7f
#define REL_REACK 8 /* already-accepted seqs still re-acked */

typedef struct RelMsg {
    uint8_t type;
    uint16_t len;
    uint8_t payload[REL_MAX];
} RelMsg;

typedef struct RelTx {
    RelMsg q[REL_QUEUE]; /* [head] is the one in flight */
    int head, n;
    uint8_t seq;      /* lane seq of the message in flight */
    uint64_t sent_ns; /* 0: not sent yet */
    int resends;
} RelTx;

static RelTx s_rel_tx[REL_LANES];  /* under tx_lock */
static RelMsg s_rel_rx[REL_QUEUE]; /* lane 1 messages waiting for the caller */
static int s_rel_rx_head, s_rel_rx_n;
static uint8_t s_rel_expect[REL_LANES]; /* next lane seq accepted */
static bool s_rel_unexpected_logged;

static int lane_of(uint8_t type) {
    return type < 0x10 ? 0 : 1;
}

/* Signed distance a - b in the 7-bit lane sequence space. */
static int seq_diff(uint8_t a, uint8_t b) {
    int d = (a - b) & REL_SEQ_MASK;
    return d < 64 ? d : d - 128;
}

/* Resend #1, #4, #16, #64...: a bad link logs a few lines, not one per 250 ms. */
static bool log_resend(unsigned n) {
    return (n & (n - 1)) == 0 && (n & 0x55555555u) != 0;
}

/* Send or resend each lane's message in flight (caller holds tx_lock). */
void rel_service(void) {
    if (s_rel_tx[0].n == 0 && s_rel_tx[1].n == 0) {
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    for (int lane = 0; lane < REL_LANES; lane++) {
        RelTx* t = &s_rel_tx[lane];
        if (t->n == 0 || (t->sent_ns != 0 && now - t->sent_ns < REL_RESEND_NS)) {
            continue;
        }
        const RelMsg* m = &t->q[t->head];
        Rel r = {hdr('R'), (uint8_t)(lane << 7 | t->seq), m->type, m->len, {0}};
        memcpy(r.payload, m->payload, m->len);
        wire_rel(&r);
        tx(&r, offsetof(Rel, payload) + m->len);
        if (t->sent_ns != 0) {
            t->resends++;
            if (log_resend((unsigned)t->resends)) {
                pc_log_line("net: reliable seq %u type %02x len %u resend #%d", r.seq, m->type,
                    m->len, t->resends);
            }
        }
        t->sent_ns = now;
    }
}

bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    if (!net.active || len < 0 || len > REL_MAX || (len > 0 && payload == NULL)) {
        return false;
    }
    RelTx* t = &s_rel_tx[lane_of(type)];
    SDL_LockMutex(net.tx_lock);
    bool ok = t->n < REL_QUEUE;
    if (ok) {
        RelMsg* m = &t->q[(t->head + t->n++) % REL_QUEUE];
        m->type = type;
        m->len = (uint16_t)len;
        if (len > 0) {
            memcpy(m->payload, payload, (size_t)len);
        }
        rel_service();
    }
    SDL_UnlockMutex(net.tx_lock);
    return ok;
}

int pc_net_recv_reliable(uint8_t* type, void* payload, int max) {
    if (s_rel_rx_n == 0 || type == NULL || payload == NULL || max < 0) {
        return -1;
    }
    RelMsg* m = &s_rel_rx[s_rel_rx_head];
    int n = m->len > max ? max : m->len;
    *type = m->type;
    memcpy(payload, m->payload, (size_t)n);
    s_rel_rx_head = (s_rel_rx_head + 1) % REL_QUEUE;
    s_rel_rx_n--;
    return n;
}

void on_rel(const Rel* r, int n) {
    if (r->len > REL_MAX || n < (int)(offsetof(Rel, payload) + r->len)) {
        return;
    }
    int lane = r->seq >> 7;
    int d = seq_diff(r->seq & REL_SEQ_MASK, s_rel_expect[lane]);
    if (d == 0) {
        if (lane == 0) {
            handshake_msg(r->type, r->payload, r->len);
        } else if (r->type == REL_RESUME) {
            net_resume_rel(r->payload, r->len);
        } else if (r->type == REL_DELAY) {
            net_delay_rel(r->payload, r->len);
        } else if (r->type == REL_SCENE) {
            net_scene_rel(r->payload, r->len);
        } else if (s_rel_rx_n < REL_QUEUE) {
            RelMsg* m = &s_rel_rx[(s_rel_rx_head + s_rel_rx_n++) % REL_QUEUE];
            m->type = r->type;
            m->len = r->len;
            memcpy(m->payload, r->payload, r->len);
        } else {
            return; /* caller is not draining: no ack, the peer resends later */
        }
        s_rel_expect[lane] = (s_rel_expect[lane] + 1) & REL_SEQ_MASK;
    } else if (d < -REL_REACK || d > 0) {
        if (!s_rel_unexpected_logged) {
            s_rel_unexpected_logged = true;
            pc_log_line("net: reliable seq %u unexpected (expect %u)", r->seq,
                lane << 7 | s_rel_expect[lane]);
        }
        return;
    }
    RelAck k = {hdr('K'), r->seq};
    SDL_LockMutex(net.tx_lock);
    tx(&k, sizeof k);
    SDL_UnlockMutex(net.tx_lock);
}

void on_rel_ack(const RelAck* k) {
    RelTx* t = &s_rel_tx[k->seq >> 7];
    SDL_LockMutex(net.tx_lock);
    if (t->n > 0 && (k->seq & REL_SEQ_MASK) == t->seq) {
        t->head = (t->head + 1) % REL_QUEUE;
        t->n--;
        t->seq = (t->seq + 1) & REL_SEQ_MASK;
        t->sent_ns = 0;
        t->resends = 0;
        rel_service(); /* next queued message goes out at once */
    }
    SDL_UnlockMutex(net.tx_lock);
}

/* Session start: every queue empty, sequence numbers from 0 (timer parked). */
void rel_reset(void) {
    for (int lane = 0; lane < REL_LANES; lane++) {
        s_rel_tx[lane].head = s_rel_tx[lane].n = s_rel_tx[lane].resends = 0;
        s_rel_tx[lane].seq = s_rel_expect[lane] = 0;
        s_rel_tx[lane].sent_ns = 0;
    }
    s_rel_rx_head = s_rel_rx_n = 0;
    s_rel_unexpected_logged = false;
}
