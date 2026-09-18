/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay time sync, copied from Slippi (SlippiNetplay.cpp CalcTimeOffsetUs):
 * positive offset = we run ahead of the peer. Skips are paid at the frame
 * boundary (pc_net_pace_adjust_ns), advances as an extra tick in
 * pc_net_after_tick; auto delay picks the input delay from ping and jitter.
 *
 * Both of those are per-side decisions taken from a per-side measurement, so
 * neither may touch the input stream: the frame loop (gm_801A4D34) runs one
 * tick per queued raw pad sample and HSD_PadRenewMasterStatus renews the
 * game's inputs only when the queue is non-empty, so anything that moves the
 * raw queue moves which inputs a tick consumes. pc_net_pace_adjust_ns
 * therefore pins the queue to exactly one sample per boundary: the sim runs
 * one tick per present, the pacing clock alone decides when, and a skip is
 * nothing but a longer wait. MELEE_NET_SYNC=legacy restores the pre-fix skip
 * (discard a queued sample) that desynced every constant-latency link. */
#include "compat.h"
#include "pc/net_internal.h"

#include <dolphin/os.h>
#include <sysdolphin/baselib/controller.h>

#include <stdlib.h>
#include <string.h>

#define OFFSET_SAMPLES 30
#define JITTER_SAMPLES 30
#define DELAY_LEAD 600           /* frames between announcing a delay and applying it */

static int32_t s_offset[OFFSET_SAMPLES];
static int s_offset_n, s_offset_i;
static int s_skip_left;
static int s_drop_left;                  /* SYNC_LEGACY only: samples to discard */
static int s_sync_over;                  /* +1/-1 when the last window crossed a threshold */
static int32_t s_sync_acted;             /* frame of the last skip/advance burst */
static uint32_t s_rtt_prev;              /* last RTT sample, for the jitter ring */
static uint32_t s_jit[JITTER_SAMPLES];   /* |dRTT| ring; its mean scales the thresholds */
static int s_jit_n, s_jit_i;
static uint64_t s_jit_sum;

/* One offset sample (on_inputs, per newest remote frame). */
void offset_note(int32_t off) {
    s_offset[s_offset_i] = off;
    s_offset_i = (s_offset_i + 1) % OFFSET_SAMPLES;
    if (s_offset_n < OFFSET_SAMPLES) {
        s_offset_n++;
    }
}

/* Trimmed mean of the offset ring: drop the top and bottom third. */
static int32_t offset_us(void) {
    if (s_offset_n == 0) {
        return 0;
    }
    int32_t b[OFFSET_SAMPLES];
    memcpy(b, s_offset, s_offset_n * sizeof b[0]);
    for (int i = 1; i < s_offset_n; i++) {
        int32_t v = b[i];
        int j = i;
        while (j > 0 && b[j - 1] > v) {
            b[j] = b[j - 1];
            j--;
        }
        b[j] = v;
    }
    int drop = s_offset_n / 3;
    int64_t sum = 0;
    for (int i = drop; i < s_offset_n - drop; i++) {
        sum += b[i];
    }
    return (int32_t) (sum / (s_offset_n - 2 * drop));
}

/* 30-sample mean of |dRTT|: the noise floor of the offset estimate. */
void jitter_note(uint32_t rtt) {
    if (s_rtt_prev != 0) {
        uint32_t d = rtt > s_rtt_prev ? rtt - s_rtt_prev : s_rtt_prev - rtt;
        if (s_jit_n == JITTER_SAMPLES) {
            s_jit_sum -= s_jit[s_jit_i];
        } else {
            s_jit_n++;
        }
        s_jit[s_jit_i] = d;
        s_jit_sum += d;
        s_jit_i = (s_jit_i + 1) % JITTER_SAMPLES;
    }
    s_rtt_prev = rtt;
}

uint32_t jitter_us(void) {
    return s_jit_n ? (uint32_t) (s_jit_sum / s_jit_n) : 0;
}

/* The delay is shared state: the frame a local sample is written for is
 * frame + delay, and pc_net_quality/the HUD report it as one number for the
 * session. Ping and jitter are per-side measurements, so both peers deciding
 * for themselves is how they ended up 2 against 4 for 600 frames. The host
 * decides alone and announces the frame it takes effect on; both peers apply
 * it there, which is a point they reach deterministically whatever the
 * message's arrival time. */
static void delay_apply(void) {
    if (net.delay_at == 0 || net.frame < net.delay_at) {
        return;
    }
    if (net.delay != net.delay_next) {
        pc_log_line("net: delay %d -> %d at frame %d", net.delay, net.delay_next, net.frame);
        net.delay = net.delay_next;
    }
    net.delay_at = 0;
}

/* Host side of MELEE_NET_DELAY=auto: delay = round((rtt/2 + jitter) / frame)
 * - 1 in 1..4, re-evaluated every 600 frames. Announced only outside a fight
 * so a running match keeps its delay; the announcement is a host-only
 * decision, so taking it from a scene that may yet be rolled back cannot
 * make the peers disagree, while applying it is frame-number driven only. */
static void delay_auto(void) {
    delay_apply();
    if (!net.delay_auto || !net.hs_host || net.delay_at != 0) {
        return;
    }
    if ((net.frame % 600) != 0 || net.ping_us == 0 || in_fight()) {
        return;
    }
    int d = (int) ((net.ping_us / 2 + jitter_us() + FRAME_US / 2) / FRAME_US) - 1;
    d = d < 1 ? 1 : d > 4 ? 4 : d;
    if (d == net.delay) {
        return;
    }
    DelayMsg m = { htonl((uint32_t) d), htonl((uint32_t) (net.frame + DELAY_LEAD)) };
    if (!pc_net_send_reliable(REL_DELAY, &m, sizeof m)) {
        return; /* lane full: the next window announces again */
    }
    net.delay_next = d;
    net.delay_at = net.frame + DELAY_LEAD;
    pc_log_line("net: auto delay %d -> %d at frame %d (ping %u ms, jitter %u ms)", net.delay, d,
                net.delay_at, net.ping_us / 1000, jitter_us() / 1000);
}

/* The host's announcement (REL_DELAY, reliable lane 1). The guest never
 * decides: whatever it measures, it switches where and when it is told. */
void net_delay_rel(const void* payload, int len) {
    DelayMsg m;
    if (len != (int) sizeof m) {
        pc_log_line("net: REL_DELAY of %d bytes ignored", len);
        return;
    }
    memcpy(&m, payload, sizeof m);
    int d = (int) ntohl(m.delay);
    int32_t at = (int32_t) ntohl(m.frame);
    if (d < 1 || d >= RING / 2) {
        pc_log_line("net: REL_DELAY asked for delay %d, ignored", d);
        return;
    }
    net.delay_next = d;
    /* A late announcement (only reachable if the reliable channel took
     * DELAY_LEAD frames, i.e. ten seconds, which the stall timeout would
     * have killed first) is applied at once rather than never. */
    net.delay_at = at > net.frame ? at : net.frame;
    pc_log_line("net: auto delay %d -> %d at frame %d (host's pick%s)", net.delay, d, net.delay_at,
                at > net.frame ? "" : ", late");
    delay_apply();
}

/* Every SYNC_INTERVAL frames. Acts only when two consecutive windows cross
 * the same threshold and at most once per SYNC_HOLDOFF frames; the
 * thresholds grow with the jitter so a noisy link cannot trigger skip/advance
 * ping-pong between the peers. */
void time_sync(void) {
    delay_auto();
    net.offset_last = offset_us();
    int32_t skip_at = 10000 + (int32_t) jitter_us();
    int32_t advance_at = FRAME_US + skip_at;
    int over = net.offset_last > skip_at ? 1 : net.offset_last < -advance_at ? -1 : 0;
    bool confirmed = over != 0 && over == s_sync_over;
    s_sync_over = over;
    if (!confirmed || net.frame - s_sync_acted < SYNC_HOLDOFF) {
        return;
    }
    s_sync_acted = net.frame;
    s_sync_over = 0;
    if (net.sync_mode == SYNC_OFF) {
        return; /* measure the link, never act on it */
    }
    if (over > 0) {
        s_skip_left = (net.offset_last - skip_at) / FRAME_US + 1;
        if (s_skip_left > 5) {
            s_skip_left = 5;
        }
    } else {
        net.advance_left = -net.offset_last / FRAME_US;
        if (net.advance_left > 3) {
            net.advance_left = 3;
        }
    }
}

/* The frame loop runs one tick per queued raw sample (lb_80019894), and a
 * tick renews the game's inputs only if the queue is non-empty, so the raw
 * queue is a clock as well as an input source. Netplay does not want it as a
 * clock: pin it to exactly one sample per boundary and the loop runs exactly
 * one tick per present, paced by vi.c.
 *
 * Surplus samples (a long present let the pad alarm catch up) are dropped
 * from the read end, so the newest physical sample is the one that survives.
 * An empty queue is refilled with the slot the last tick consumed; net.c
 * writes the frame's synced inputs over it and does not read a local sample
 * back out of it (pad_reused), so the local input simply repeats, which is
 * what both peers see because it goes on the wire. */
static void pad_queue_pin(void) {
    PadLibData* p = &HSD_PadLibData;
    if (p->qnum == 0) {
        return;
    }
    bool intr = OSDisableInterrupts();
    while (p->qcount > 1) {
        p->qread = (uint8_t) ((p->qread + 1) % p->qnum);
        p->qcount--;
    }
    net.pad_reused = p->qcount == 0;
    if (net.pad_reused) {
        p->qread = (uint8_t) ((p->qread + p->qnum - 1) % p->qnum);
        p->qcount = 1;
        net.pad_reuse++;
    }
    OSRestoreInterrupts(intr);
}

/* SYNC_LEGACY: the skip as it was before the pin. The longer wait lets the
 * pad alarm queue one sample more than usual and that sample is discarded
 * here, which is a change to the raw queue -- and therefore to how many
 * ticks the frame loop runs and to whether a tick renews its inputs at all.
 * Kept only so the desync can be reproduced on demand. */
static uint64_t pace_adjust_legacy(void) {
    PadLibData* p = &HSD_PadLibData;
    while (s_drop_left > 0 && p->qcount > 1) {
        p->qwrite = (uint8_t) ((p->qwrite + p->qnum - 1) % p->qnum);
        p->qcount--;
        s_drop_left--;
        net.skips++;
    }
    if (s_skip_left == 0) {
        return 0;
    }
    s_skip_left--;
    s_drop_left++;
    return (uint64_t) FRAME_US * 1000;
}

uint64_t pc_net_pace_adjust_ns(void) {
    if (!net.active) {
        return 0;
    }
    if (net.sync_mode == SYNC_LEGACY) {
        return pace_adjust_legacy();
    }
    pad_queue_pin();
    if (s_skip_left == 0) {
        return 0;
    }
    /* Ahead of the peer: wait one frame longer before the next present. No
     * tick is added or removed and no sample is touched; the sim simply
     * falls one frame further behind the wall clock. */
    s_skip_left--;
    net.skips++;
    return (uint64_t) FRAME_US * 1000;
}

/* Session start: empty rings, no burst pending, holdoff already elapsed. */
void sync_reset(void) {
    const char* mode = getenv("MELEE_NET_SYNC");
    net.sync_mode = mode == NULL                  ? SYNC_ON
                    : strcmp(mode, "off") == 0    ? SYNC_OFF
                    : strcmp(mode, "legacy") == 0 ? SYNC_LEGACY
                                                  : SYNC_ON;
    if (net.sync_mode != SYNC_ON) {
        pc_log_line("net: time sync %s (MELEE_NET_SYNC)",
                    net.sync_mode == SYNC_OFF ? "off: offset measured, never acted on"
                                              : "legacy: skips discard a queued pad sample");
    }
    s_offset_n = s_offset_i = 0;
    net.offset_last = 0;
    s_skip_left = net.advance_left = s_drop_left = s_sync_over = 0;
    s_sync_acted = -SYNC_HOLDOFF;
    s_rtt_prev = 0;
    s_jit_n = s_jit_i = 0;
    s_jit_sum = 0;
    net.delay_at = 0;
    net.pad_reused = false;
    net.pad_reuse = net.pad_empty = 0;
}
