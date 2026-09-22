/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay link simulator (replaces `tc netem`, which needs root): every
 * outgoing datagram passes tx(), which drops, holds, jitters, reorders or
 * duplicates it per the MELEE_NET_SIM_* knobs (net.c header). Held packets
 * are released by tx_flush() from the game thread and the 4 ms timer. */
#include "compat.h"
#include "pc/net_internal.h"

#include <SDL3/SDL_timer.h>
#include <stdlib.h>
#include <string.h>

static int s_sim_burst_left;
static uint64_t s_sim_burst_ns; /* when the next burst starts */
static int s_sim_swap = -1;     /* held slot waiting to go out behind the next packet */
static uint32_t s_sim_rng;      /* xorshift32, seeded from the port */
static Held s_held[HELD_MAX];   /* outgoing, under tx_lock */

static unsigned sim_rand(unsigned n) {
    s_sim_rng ^= s_sim_rng << 13;
    s_sim_rng ^= s_sim_rng >> 17;
    s_sim_rng ^= s_sim_rng << 5;
    return s_sim_rng % n;
}

/* Park a datagram until release_ns; -1 when every slot is taken (one more loss). */
int held_put(Held* held, const void* buf, size_t len, uint64_t release_ns) {
    for (int i = 0; i < HELD_MAX; i++) {
        if (held[i].release_ns == 0) {
            held[i].release_ns = release_ns;
            held[i].len = (uint16_t)len;
            memcpy(held[i].buf, buf, len);
            return i;
        }
    }
    return -1;
}

/* The due datagram with the earliest release, or NULL. */
Held* held_due(Held* held, uint64_t now) {
    Held* due = NULL;
    for (int i = 0; i < HELD_MAX; i++) {
        if (held[i].release_ns != 0 && held[i].release_ns <= now &&
            (due == NULL || held[i].release_ns < due->release_ns))
        {
            due = &held[i];
        }
    }
    return due;
}

/* Every outgoing datagram passes here (caller holds tx_lock). */
void tx(const void* buf, size_t len) {
    uint64_t now = SDL_GetTicksNS();
    net.tx_pkts++;
    if (*(const uint8_t*)buf == 'M') {
        net.tx_inputs++;
    }
    if (net.sim_burst > 0 && now >= s_sim_burst_ns) {
        s_sim_burst_ns = now + 5000000000ull;
        s_sim_burst_left = net.sim_burst;
        pc_log_line("net: sim burst: dropping %d packets", net.sim_burst);
    }
    if (s_sim_burst_left > 0) {
        s_sim_burst_left--;
        return;
    }
    if (net.sim_loss > 0 && (int)sim_rand(100) < net.sim_loss) {
        return;
    }
    if (!net.sim_hold) {
        net_sendto(buf, len);
        return;
    }
    int copies = net.sim_dup > 0 && (int)sim_rand(100) < net.sim_dup ? 2 : 1;
    for (; copies > 0; copies--) {
        int64_t release = (int64_t)(now + net.sim_delay_ns);
        if (net.sim_jitter_ms > 0) {
            release += ((int64_t)sim_rand(2 * net.sim_jitter_ms + 1) - net.sim_jitter_ms) * 1000000;
        }
        if (release < (int64_t)now) {
            release = (int64_t)now;
        }
        int slot = held_put(s_held, buf, len, (uint64_t)release);
        if (slot < 0) {
            return;
        }
        if (s_sim_swap >= 0) {
            s_held[s_sim_swap].release_ns = (uint64_t)release + 1; /* right behind this one */
            s_sim_swap = -1;
        } else if (net.sim_reorder > 0 && (int)sim_rand(100) < net.sim_reorder) {
            /* Hold it so the next packet queued overtakes it, but only until a packet
             * interval has passed: a successor that never comes because it was dropped,
             * eaten by a burst or lost to a full queue, or because the session ended,
             * must not strand this datagram. Real reordering delays packets, it does not
             * lose them, and a simulator that invents a failure mode the link cannot have
             * sends whoever reads the log chasing a bug that is not there. */
            s_held[slot].release_ns = (uint64_t)release + pc_sim_period_ns();
            s_sim_swap = slot;
        }
    }
}

/* Release held packets whose simulated delay has passed (caller holds tx_lock). */
void tx_flush(void) {
    uint64_t now = SDL_GetTicksNS();
    for (Held* h; (h = held_due(s_held, now)) != NULL;) {
        if (s_sim_swap >= 0 && h == &s_held[s_sim_swap]) {
            s_sim_swap = -1; /* its deadline won; the slot is about to be free for reuse */
        }
        net_sendto(h->buf, h->len);
        h->release_ns = 0;
    }
}

/* MELEE_NET_SIM_* knobs for the session; the PRNG is seeded from the local
 * port so runs repeat. */
void sim_env(uint16_t bind_port) {
    static const struct {
        const char* env;
        int* out;
    } knobs[] = {
        {"MELEE_NET_SIM_LOSS", &net.sim_loss},
        {"MELEE_NET_SIM_JITTER_MS", &net.sim_jitter_ms},
        {"MELEE_NET_SIM_REORDER", &net.sim_reorder},
        {"MELEE_NET_SIM_DUP", &net.sim_dup},
        {"MELEE_NET_SIM_BURST", &net.sim_burst},
    };
    for (size_t i = 0; i < sizeof knobs / sizeof knobs[0]; i++) {
        const char* v = getenv(knobs[i].env);
        *knobs[i].out = v ? atoi(v) : 0;
    }
    const char* v = getenv("MELEE_NET_SIM_DELAY_MS");
    net.sim_delay_ns = v ? (uint64_t)atoi(v) * 1000000ull : 0;
    v = getenv("MELEE_NET_SIM_DELAY_RX_MS");
    net.sim_rx_delay_ns = v ? (uint64_t)atoi(v) * 1000000ull : 0;
    net.sim_hold = net.sim_delay_ns || net.sim_jitter_ms || net.sim_reorder || net.sim_dup;
    s_sim_rng = (uint32_t)bind_port * 2654435761u | 1u;
}

/* Session start: empty held queue, no burst or swap pending (timer parked). */
void sim_reset(void) {
    memset(s_held, 0, sizeof s_held);
    s_sim_swap = -1;
    s_sim_burst_left = 0;
    s_sim_burst_ns = 0;
}
