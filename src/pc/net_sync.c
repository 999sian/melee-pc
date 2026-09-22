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
#define DELAY_LEAD 120    /* frames between announcing a delay and applying it */
#define DELAY_EVERY 60    /* frames between auto-delay decisions */
#define DELAY_HOLDOFF 300 /* frames between link-driven delay changes */
/* A fight predicts and rolls back, so the frames of the trip covered here are
 * paid by the rollback window instead of by the player's hands. Menus are
 * lockstep and pay all of it. */
#define ROLLBACK_COVER 2
/* Nudge: the offset ring is a 30-sample trimmed mean, so only part of a
 * window's correction is visible in the next window's measurement; taking
 * half the excess per window damps the rest instead of ringing. */
#define NUDGE_MAX_NS 1000000 /* 1 ms: ~6% of a frame, one dropped frame in 16 */

static int32_t s_offset[OFFSET_SAMPLES];
static int s_offset_n, s_offset_i;
static int s_skip_left;
static int32_t s_skip_asked = -1;      /* frame the pending skip was raised on */
static bool s_skip_wait;               /* prefer an idle frame to take it on */
static int s_drop_left;                /* SYNC_LEGACY only: samples to discard */
static int s_sync_over;                /* +1/-1 when the last window crossed a threshold */
static int32_t s_sync_acted;           /* frame of the last skip/advance burst */
static uint32_t s_rtt_prev;            /* last RTT sample, for the jitter ring */
static uint32_t s_jit[JITTER_SAMPLES]; /* |dRTT| ring; its mean scales the thresholds */
static int s_jit_n, s_jit_i;
static uint64_t s_jit_sum;
static int64_t s_nudge_ns;    /* slow-down carried by every frame's pacing wait */
static int s_delay_base;      /* frames of delay the link needs while lockstep */
static int s_delay_seen;      /* last window's measurement, for confirm-twice */
static int32_t s_delay_acted; /* frame of the last delay announcement */
static bool s_delay_fight;    /* in_fight() when that announcement was made */

/* One phase sample (on_inputs, per input packet): the peer's frame advantage
 * against ours, in frames. Positive means we are the one running ahead.
 * Stored in microseconds so the rest of the controller keeps its units and
 * pc_net_quality/the report keep reading one number. */
void adv_note(int remote_adv, int local_adv) {
    s_offset[s_offset_i] = (int32_t)((local_adv - remote_adv) * (int64_t)FRAME_US / 2);
    s_offset_i = (s_offset_i + 1) % OFFSET_SAMPLES;
    if (s_offset_n < OFFSET_SAMPLES) {
        s_offset_n++;
    }
}

/* Trimmed mean of the phase ring: drop the top and bottom third. Each sample
 * is a difference of two integer frame counts, so the ring is here to ride
 * out the frame either side that loss and reordering move it by, not to
 * average out a noisy estimator. */
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
    return (int32_t)(sum / (s_offset_n - 2 * drop));
}

/* 30-sample mean of |dRTT|: what the delay has to cover on top of the trip. */
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
    return s_jit_n ? (uint32_t)(s_jit_sum / s_jit_n) : 0;
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

/* Host side of MELEE_NET_DELAY=auto.
 *
 * A lockstep frame cannot start until the peer's input for it has arrived, so
 * a menu needs delay * frame >= the one-way trip or BOTH peers stall every
 * frame and the pair runs at trip/delay instead of 60 Hz. A fight does not:
 * it predicts up to WINDOW frames past the remote and corrects by rolling
 * back, so ROLLBACK_COVER frames of that trip are paid by the rollback
 * window rather than by the player's hands. One delay sized for the menus
 * therefore carries one to two frames of dead input lag through the whole
 * match, which is exactly the latency rollback exists to remove.
 *
 * The link measurement is confirmed over two windows before it moves the
 * base, and a link-driven change waits out DELAY_HOLDOFF; a scene crossing
 * between menu and fight is applied at once, because that is the transition
 * the split is for. Deciding is one side's job (the peers measure different
 * ping and jitter and would otherwise pick differently); applying is driven
 * by frame number alone, so both land on the same frame. */
static void delay_auto(void) {
    delay_apply();
    /* Exactly one side announces, and it is the same side everywhere in the
     * netcode: player 1, net.local 0. That peer names the session id
     * (net.c), is the one a lobby connects as player 0 (net_lan.c,
     * net_match.c) and hosts the match handshake, direct sessions included
     * (net_handshake.c). Reading net.hs_host instead would miss the window
     * before the handshake is claimed, which in a direct session used to be
     * the whole session: the documented MELEE_NET_DELAY=auto then never
     * moved off its initial guess in direct play. */
    bool announcer = net.local == 0;
    if (!net.delay_auto || !announcer || net.delay_at != 0) {
        return;
    }
    if ((net.frame % DELAY_EVERY) != 0 || net.ping_us == 0) {
        return;
    }
    uint32_t trip = net.ping_us / 2 + jitter_us();
    int need = (int)((trip + FRAME_US - 1) / FRAME_US);
    /* Hysteresis: a trip sitting on a frame boundary would otherwise flip
     * between two delays for the rest of the match, one announcement per
     * holdoff. Coming down wants a quarter frame of room below the boundary;
     * going up is immediate, because the frames above it are the ones that
     * stall. */
    if (need < s_delay_base && trip + (uint32_t)(FRAME_US / 4) > (uint32_t)(need * FRAME_US)) {
        need = s_delay_base;
    }
    if (need == s_delay_seen) {
        s_delay_base = need; /* two windows agree: not a jitter spike */
    }
    s_delay_seen = need;
    if (s_delay_base == 0) {
        return;
    }
    bool fight = in_fight();
    int d = fight ? s_delay_base - ROLLBACK_COVER : s_delay_base;
    d = d < 1 ? 1 : d > 4 ? 4 : d;
    if (d == net.delay) {
        return;
    }
    /* The delay moves on entering or leaving a fight and nowhere else. A
     * delay that drifts with the link is worse to play against than a delay
     * that is a frame too high: NetherRealm shipped Mortal Kombat X with a
     * variable 5-20 frames, found that was the thing players complained
     * about, and replaced it with a fixed 3 (Stallone, GDC 2017). So the
     * link only gets to re-decide between matches. */
    if (fight == s_delay_fight && (fight || net.frame - s_delay_acted < DELAY_HOLDOFF)) {
        return;
    }
    DelayMsg m = {htonl((uint32_t)d), htonl((uint32_t)(net.frame + DELAY_LEAD))};
    if (!pc_net_send_reliable(REL_DELAY, &m, sizeof m)) {
        return; /* lane full: the next window announces again */
    }
    net.delay_next = d;
    net.delay_at = net.frame + DELAY_LEAD;
    s_delay_acted = net.frame;
    s_delay_fight = fight;
    pc_log_line("net: auto delay %d -> %d at frame %d (%s, ping %u ms, jitter %u ms)", net.delay, d,
        net.delay_at, fight ? "fight" : "lockstep", net.ping_us / 1000, jitter_us() / 1000);
}

/* The host's announcement (REL_DELAY, reliable lane 1). The guest never
 * decides: whatever it measures, it switches where and when it is told. */
void net_delay_rel(const void* payload, int len) {
    DelayMsg m;
    if (len != (int)sizeof m) {
        pc_log_line("net: REL_DELAY of %d bytes ignored", len);
        return;
    }
    memcpy(&m, payload, sizeof m);
    int d = (int)ntohl(m.delay);
    int32_t at = (int32_t)ntohl(m.frame);
    if (d < 1 || d >= RING / 2) {
        pc_log_line("net: REL_DELAY asked for delay %d, ignored", d);
        return;
    }
    net.delay_next = d;
    /* A late announcement (the reliable channel took DELAY_LEAD frames, two
     * seconds) is applied at once rather than never: the delay decides only
     * which local sample lands in this side's ring, and both peers feed the
     * simulation from the wire, so a window where the two run different
     * delays costs a frame of fairness and nothing else. */
    net.delay_at = at > net.frame ? at : net.frame;
    pc_log_line("net: auto delay %d -> %d at frame %d (host's pick%s)", net.delay, d, net.delay_at,
        at > net.frame ? "" : ", late");
    delay_apply();
}

/* Every SYNC_INTERVAL frames. net.offset_last is half the disagreement
 * between the two peers' frame advantages, i.e. exactly how much phase this
 * side should give back; it is spread over the frames until the next window
 * as a per-frame lengthening of the pacing wait, so the phase closes without
 * the 16.7 ms hitch a whole skipped frame costs.
 *
 * Only the peer that is AHEAD corrects, which is GGPO's rule: the two peers
 * compute the same difference from the same two numbers, so they cannot both
 * decide they are the one in front. Acting from both ends would give the
 * loop twice the gain it is tuned for, and slowing down is in any case the
 * only correction that is bounded -- a frame can always be made longer,
 * never arbitrarily shorter. The peer behind needs no correction of its own:
 * the other one slowing is what closes the gap. */
void time_sync(void) {
    delay_auto();
    net.offset_last = offset_us();
    if (net.sync_mode == SYNC_OFF) {
        s_nudge_ns = 0;
        return; /* measure the link, never act on it */
    }
    int32_t off = net.offset_last;
    /* One frame of disagreement is half a frame of correction, and that is
     * the quantisation of the measurement itself: loss or reordering moves a
     * sample by a frame either way. Correct from two frames out. */
    int32_t deadband = FRAME_US / 2;
    if (off > deadband) {
        int64_t nudge = ((int64_t)(off - deadband) * 1000) / (2 * SYNC_INTERVAL);
        s_nudge_ns = nudge > NUDGE_MAX_NS ? NUDGE_MAX_NS : nudge;
    } else {
        s_nudge_ns = 0;
    }
    /* A gap the nudge would take seconds to close still pays a whole frame,
     * at most one per SYNC_HOLDOFF. The advance (an extra tick) is kept for
     * the mirror case only: a peer this far behind is normally pulled back
     * by the other side waiting on its inputs. */
    if (net.frame - s_sync_acted >= SYNC_HOLDOFF) {
        if (off > 100000) {
            s_sync_acted = net.frame;
            s_skip_left = 1;
            s_skip_asked = net.frame;
            s_skip_wait = true;
        } else if (off < -100000) {
            s_sync_acted = net.frame;
            net.advance_left = 1;
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
        p->qread = (uint8_t)((p->qread + 1) % p->qnum);
        p->qcount--;
    }
    net.pad_reused = p->qcount == 0;
    if (net.pad_reused) {
        p->qread = (uint8_t)((p->qread + p->qnum - 1) % p->qnum);
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
        p->qwrite = (uint8_t)((p->qwrite + p->qnum - 1) % p->qnum);
        p->qcount--;
        s_drop_left--;
        net.skips++;
    }
    if (s_skip_left == 0) {
        return 0;
    }
    s_skip_left--;
    s_drop_left++;
    return (uint64_t)FRAME_US * 1000;
}

uint64_t pc_net_pace_adjust_ns(void) {
    if (!net.active) {
        return 0;
    }
    if (net.sync_mode == SYNC_LEGACY) {
        return pace_adjust_legacy();
    }
    pad_queue_pin();
    uint64_t adj = (uint64_t)s_nudge_ns; /* never negative: see time_sync */
    /* The whole-frame correction waits for a frame the player is not in the
     * middle of. It lengthens one frame by a whole frame period, so taken
     * mid-input it eats a frame of that input -- the one correction here
     * that a player can feel, and on a stick sweep or a button press the
     * one that matters most. GGPO's require_idle_input refuses to sleep for
     * the same reason. The sub-frame nudge above is unconditional: it is
     * small enough that no input is lost to it, and it is what closes the
     * phase in the normal case anyway (the delayed soak: 261 skips against
     * 225,000 frames, offset held at +0.0 ms). A skip deferred here is not
     * dropped, it waits for the next idle frame. */
    /* ...but not forever: a player who never lets go would otherwise defer
     * it indefinitely and the phase would stay open. After SYNC_HOLDOFF
     * frames of waiting, take it anyway. */
    if (s_skip_left > 0 && s_skip_asked >= 0 && net.frame - s_skip_asked >= SYNC_HOLDOFF) {
        s_skip_wait = false;
    }
    if (s_skip_left > 0 && (!s_skip_wait || net_local_idle())) {
        s_skip_left--;
        net.skips++;
        adj += (uint64_t)FRAME_US * 1000; /* the whole frame the nudge could not pay */
    }
    return adj;
}

/* Session start: empty rings, no burst pending, holdoff already elapsed. */
void sync_reset(void) {
    const char* mode = getenv("MELEE_NET_SYNC");
    net.sync_mode = mode == NULL                ? SYNC_ON :
                    strcmp(mode, "off") == 0    ? SYNC_OFF :
                    strcmp(mode, "legacy") == 0 ? SYNC_LEGACY :
                                                  SYNC_ON;
    if (net.sync_mode != SYNC_ON) {
        pc_log_line("net: time sync %s (MELEE_NET_SYNC)",
            net.sync_mode == SYNC_OFF ? "off: offset measured, never acted on" :
                                        "legacy: skips discard a queued pad sample");
    }
    s_offset_n = s_offset_i = 0;
    net.offset_last = 0;
    s_skip_left = net.advance_left = s_drop_left = s_sync_over = 0;
    s_skip_asked = -1;
    s_skip_wait = false;
    s_sync_acted = -SYNC_HOLDOFF;
    s_nudge_ns = 0;
    s_delay_base = s_delay_seen = 0;
    s_delay_acted = -DELAY_HOLDOFF;
    s_delay_fight = false;
    s_rtt_prev = 0;
    s_jit_n = s_jit_i = 0;
    s_jit_sum = 0;
    net.delay_at = 0;
    net.pad_reused = false;
    net.pad_reuse = net.pad_empty = 0;
}
