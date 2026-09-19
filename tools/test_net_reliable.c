/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for src/pc/net_reliable.c: one process plays both ends (the
 * transmit and receive state are independent); tx() captures the datagram
 * and the test hands it back to on_rel/on_rel_ack. From the repo root:
 *   cc -DTARGET_PC=1 -I src -I extern/aurora/include tools/test_net_reliable.c \
 *      -o /tmp/test_net_reliable && /tmp/test_net_reliable */
#include "../src/pc/net_reliable.c"

#include <stdio.h>
#include <stdlib.h>

/* not <assert.h>: the decomp's debug.h owns __assert, and -DNDEBUG must not blind this */
#define assert(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

struct NetSession net;
static uint64_t s_now = 1;
static uint8_t s_out[sizeof(Rel)]; /* last datagram, [0] is its magic */
static size_t s_out_len;
static Rel s_last;        /* last 'R' handed to on_rel */
static size_t s_last_len; /* its wire length (s_out_len is later reused by 'K') */
static int s_hs_msgs, s_unexpected, s_resend_logs;

Uint64 SDL_GetTicksNS(void) {
    return s_now;
}
void SDL_LockMutex(SDL_Mutex* m) {
    (void)m;
}
void SDL_UnlockMutex(SDL_Mutex* m) {
    (void)m;
}
Hdr hdr(uint8_t magic) {
    Hdr h = {magic, WIRE_VERSION, 0, 0};
    return h;
}
void wire_rel(Rel* r) {
    (void)r;
}
void tx(const void* buf, size_t len) {
    memcpy(s_out, buf, len);
    s_out_len = len;
}
void handshake_msg(uint8_t type, const uint8_t* p, int len) {
    (void)type;
    (void)p;
    (void)len;
    s_hs_msgs++;
}
static int s_resume_msgs, s_resume_len;
void net_resume_rel(const void* payload, int len) {
    s_resume_msgs++;
    s_resume_len = len;
    (void)payload;
}
static int s_delay_msgs;
/* Dispatched like REL_DELAY; this harness only exercises the channel. */
void net_scene_rel(const void* payload, int len) {
    (void)payload;
    (void)len;
}
void net_delay_rel(const void* payload, int len) {
    s_delay_msgs++;
    (void)payload;
    (void)len;
}
void pc_log_line(const char* fmt, ...) {
    s_unexpected += strstr(fmt, "unexpected") != NULL;
    s_resend_logs += strstr(fmt, "resend #") != NULL;
}

static uint8_t out_seq(void) {
    assert(s_out[0] == 'R');
    return s_out[offsetof(Rel, seq)];
}

/* Hand the captured 'R' to the receiver; true when it answered with a 'K'. */
static bool deliver(void) {
    assert(s_out[0] == 'R');
    memcpy(&s_last, s_out, s_out_len);
    s_last_len = s_out_len;
    s_out[0] = 0;
    on_rel(&s_last, (int)s_last_len);
    return s_out[0] == 'K';
}

/* Hand the captured 'K' to the sender (which then transmits the next queued 'R'). */
static void ack(void) {
    assert(s_out[0] == 'K');
    RelAck k;
    memcpy(&k, s_out, sizeof k);
    s_out[0] = 0;
    on_rel_ack(&k);
}

static int recv_user(void) {
    uint8_t type;
    char buf[REL_MAX];
    int n = pc_net_recv_reliable(&type, buf, sizeof buf);
    assert(n < 0 || type == 0x10);
    return n;
}

static void round_trip(uint8_t type, int i) {
    assert(pc_net_send_reliable(type, "ping", 4));
    assert(out_seq() == ((type < 0x10 ? 0 : 0x80) | (i & 0x7f)));
    assert(deliver());
    if (type >= 0x10) {
        assert(recv_user() == 4);
    }
    ack();
    assert(s_out[0] == 0); /* nothing queued behind it */
}

int main(void) {
    net.active = true;
    rel_reset();

    /* seq wrap at 127 -> 0 in both lanes, every message delivered once */
    for (int i = 0; i < 300; i++) {
        round_trip(0x10, i);
        round_trip(0x01, i);
    }
    assert(s_hs_msgs == 300 && s_rel_expect[0] == 300 % 128 && s_rel_expect[1] == 300 % 128);
    assert(s_rel_tx[0].n == 0 && s_rel_tx[1].n == 0 && recv_user() < 0);

    /* duplicate of the last one (its 'K' was lost): re-acked, not re-processed */
    Rel dup = s_last;
    on_rel(&dup, (int)s_last_len);
    assert(s_out[0] == 'K' && s_out[offsetof(RelAck, seq)] == dup.seq && s_hs_msgs == 300);
    s_out[0] = 0;
    dup.seq = (uint8_t)((s_rel_expect[0] - REL_REACK) & REL_SEQ_MASK); /* oldest re-acked */
    on_rel(&dup, (int)s_last_len);
    assert(s_out[0] == 'K' && s_hs_msgs == 300 && s_unexpected == 0);
    s_out[0] = 0;

    /* out of window: 9 back and 1 ahead are dropped, logged once per session */
    dup.seq = (uint8_t)((s_rel_expect[0] - REL_REACK - 1) & REL_SEQ_MASK);
    on_rel(&dup, (int)s_last_len);
    dup.seq = (uint8_t)((s_rel_expect[0] + 1) & REL_SEQ_MASK);
    on_rel(&dup, (int)s_last_len);
    dup.seq = (uint8_t)(0x80 | ((s_rel_expect[1] + 1) & REL_SEQ_MASK));
    on_rel(&dup, (int)s_last_len);
    assert(s_out[0] == 0 && s_hs_msgs == 300 && recv_user() < 0 && s_unexpected == 1);
    rel_reset();
    assert(s_rel_expect[0] == 0 && s_rel_tx[1].seq == 0 && s_rel_tx[1].sent_ns == 0);
    dup.seq = 0x85;
    on_rel(&dup, (int)s_last_len);
    assert(s_unexpected == 2); /* the once-flag was cleared */

    /* receive side: the caller stops draining, RULES/READY still flow */
    for (int i = 0; i < REL_QUEUE; i++) {
        assert(pc_net_send_reliable(0x10, "ping", 4) && deliver());
        ack();
    }
    assert(pc_net_send_reliable(0x10, "full", 4) && !deliver()); /* no ack: queue full */
    assert(pc_net_send_reliable(0x01, NULL, 0) && out_seq() == 0 && deliver());
    assert(s_hs_msgs == 301);
    ack();
    assert(recv_user() == 4);
    s_now += REL_RESEND_NS;
    rel_service(); /* lane 1 resends "full" now that there is room */
    assert(out_seq() == (0x80 | REL_QUEUE) && deliver() && s_resend_logs == 1);
    ack();
    for (int i = 0; i < REL_QUEUE; i++) {
        assert(recv_user() == 4);
    }
    assert(recv_user() < 0);

    /* transmit side: a full user lane does not block the handshake lane */
    for (int i = 0; i < REL_QUEUE; i++) {
        assert(pc_net_send_reliable(0x10, "ping", 4)); /* never acked */
    }
    assert(!pc_net_send_reliable(0x10, "ping", 4));
    assert(pc_net_send_reliable(0x02, NULL, 0) && out_seq() == 1 && deliver());
    assert(s_hs_msgs == 302);
    ack(); /* clear the handshake lane so only the user lane resends below */

    /* resend log only on #1, #4, #16, #64 (user lane alone is in flight) */
    for (int i = 0; i < 70; i++) {
        s_now += REL_RESEND_NS;
        rel_service();
    }
    assert(s_rel_tx[1].resends == 70 && s_resend_logs == 1 + 4);

    /* REL_RESUME is net.c's own lane-1 type: dispatched inline and acked,
     * never queued for the caller, and the lane's sequence keeps running so
     * the caller's next message still arrives. */
    rel_reset();
    assert(pc_net_send_reliable(REL_RESUME, "resume", 6) && deliver());
    assert(s_resume_msgs == 1 && s_resume_len == 6);
    assert(recv_user() < 0);
    ack();
    assert(pc_net_send_reliable(0x10, "ping", 4) && out_seq() == 0x81 && deliver());
    assert(recv_user() == 4 && s_resume_msgs == 1);
    ack();
    puts("test_net_reliable: ok");
    return 0;
}
