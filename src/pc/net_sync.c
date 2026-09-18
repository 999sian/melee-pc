/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay time sync, copied from Slippi (SlippiNetplay.cpp CalcTimeOffsetUs):
 * positive offset = we run ahead of the peer. Skips are paid at the frame
 * boundary (pc_net_pace_adjust_ns), advances as an extra tick in
 * pc_net_after_tick; auto delay picks the input delay from ping and jitter. */
#include "compat.h"
#include "pc/net_internal.h"

#include <sysdolphin/baselib/controller.h>

#include <string.h>

#define OFFSET_SAMPLES 30
#define JITTER_SAMPLES 30

static int32_t s_offset[OFFSET_SAMPLES];
static int s_offset_n, s_offset_i;
static int s_skip_left;
static int s_drop_left;                  /* pad samples to discard after paid skips */
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

/* MELEE_NET_DELAY=auto: delay = round((rtt/2 + jitter) / frame) - 1 in 1..4,
 * re-evaluated every 600 frames. A running match keeps its delay; the new
 * one is applied at the first sync point outside a fight (lobby, results,
 * title), so both matches of a set can differ but no match changes mid-way.
 * Both peers may pick different delays: frames are absolute, and the offset
 * equilibrium just shifts by the difference. */
static void delay_auto(void) {
    if (!net.delay_auto) {
        return;
    }
    if ((net.frame % 600) == 0 && net.ping_us > 0) {
        int d = (int) ((net.ping_us / 2 + jitter_us() + FRAME_US / 2) / FRAME_US) - 1;
        d = d < 1 ? 1 : d > 4 ? 4 : d;
        if (d != net.delay_next) {
            pc_log_line("net: auto delay %d -> %d (ping %u ms, jitter %u ms)%s", net.delay_next, d,
                        net.ping_us / 1000, jitter_us() / 1000,
                        in_fight() ? ", after this match" : "");
            net.delay_next = d;
        }
    }
    if (net.delay_next != net.delay && !in_fight()) {
        pc_log_line("net: delay %d -> %d at frame %d", net.delay, net.delay_next, net.frame);
        net.delay = net.delay_next;
    }
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

/* A skip is paid at the frame boundary: the pacing wait grows by one frame,
 * the wall-clock pad alarm then queues one sample more than usual, and that
 * extra sample is discarded here so the sim really falls a frame behind
 * instead of ticking twice to catch up. Nothing sleeps inside a tick. */
uint64_t pc_net_pace_adjust_ns(void) {
    if (!net.active) {
        return 0;
    }
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

/* Session start: empty rings, no burst pending, holdoff already elapsed. */
void sync_reset(void) {
    s_offset_n = s_offset_i = 0;
    net.offset_last = 0;
    s_skip_left = net.advance_left = s_drop_left = s_sync_over = 0;
    s_sync_acted = -SYNC_HOLDOFF;
    s_rtt_prev = 0;
    s_jit_n = s_jit_i = 0;
    s_jit_sum = 0;
}
