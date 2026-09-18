/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay prototype: two instances exchange one PADStatus per frame over UDP
 * and run in rollback lockstep modelled on Slippi (docs/netcode-plan.md §4):
 * a remote input that has not arrived yet is predicted as "repeat last", a
 * snapshot is taken before every predicted tick, and when the real input
 * turns out different the state is restored and the frames since re-run.
 * Both peers must boot with the same disc and no memory card; the env path
 * below needs the same MELEE_SEED too, the lobby path agrees the seed in
 * the match handshake. Sessions can also be opened at runtime through
 * pc_net_connect() (src/pc/net_lan.h).
 *
 *   MELEE_NET=host:port          peer address (enables netplay at boot)
 *   MELEE_NET_PORT=n             local UDP port (default 41000)
 *   MELEE_NET_PLAYER=0|1         which controller port the local player drives
 *   MELEE_NET_DELAY=n|auto       input delay in frames (default auto: from ping and jitter)
 *   MELEE_NET_SIM_LOSS=percent   drop that share of outgoing packets
 *   MELEE_NET_SIM_DELAY_MS=ms    hold every outgoing packet that long
 *   MELEE_NET_SIM_DELAY_RX_MS=ms hold every incoming packet that long (asymmetric links)
 *   MELEE_NET_SIM_JITTER_MS=ms   uniform +-ms on the outgoing delay (reorders when > delay)
 *   MELEE_NET_SIM_REORDER=pct    hold that share of packets behind the next one
 *   MELEE_NET_SIM_DUP=pct        send that share of packets twice
 *   MELEE_NET_SIM_BURST=n        every 5 s drop n consecutive outgoing packets
 *   (the simulator's PRNG is seeded from MELEE_NET_PORT, so runs repeat)
 *   MELEE_NET_HANDSHAKE_TEST=1   run the lobby handshake at frame 300 without a lobby
 *
 * This file is the session, the socket and the rollback loop; the wire
 * codec, link simulator, reliable channel, handshake, time sync and
 * snapshots are the net_*.c modules listed in net_internal.h. */
#include "compat.h"
#include "pc/net_internal.h"

#include <dolphin/os.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- state ------------------------------------------------------------ */

struct NetSession net = { .sock = SOCK_INVALID, .tick_frame = -1, .rb_barrier = -1,
                          .start_frame = -1 };

static WirePad s_local_ring[RING];       /* indexed by frame & (RING-1) */
static WirePad s_remote_ring[RING];      /* real input, or the prediction in use */
static int32_t s_remote_have = -1;       /* newest contiguous real remote frame */
static int32_t s_remote_newest = -1;     /* newest frame the peer reported holding */
static int32_t s_last_acked = -1;        /* newest local frame the peer holds */
static int32_t s_rb_frame = -1;          /* oldest mispredicted frame not rolled back yet */
static int32_t s_remote_ck_frame = -1;
static uint32_t s_remote_ck;
static uint32_t s_ck_ring[RING];         /* our checksum entering each frame */
static bool s_heard;                     /* any packet from the peer yet */
static PADStatus s_raw_last;             /* newest physical sample (port 0) */
static int s_status;                     /* pc_net_peer_status(); kept until the next connect */
static bool s_peer_left;                 /* BYE received or version mismatch: stop waiting */
static bool s_warn_src, s_warn_sess, s_warn_bad; /* one reject line each per session */

static unsigned s_stalls, s_advances, s_rollbacks, s_rb_lost;
static int s_rb_depth_max;
static uint64_t s_stall_ns_max;
static uint64_t s_ping_sum;
static unsigned s_ping_n;
static uint32_t s_rtt_min, s_rtt_max;    /* this stats window */
static unsigned s_rx_pkts;               /* datagrams this stats window */
static unsigned s_rx_acks;               /* every input packet earns one ack: the gap is loss */
static int s_loss_pct;                   /* round-trip loss of the last window */
static int s_rb_depth_cur, s_rb_depth_recent; /* deepest rollback this second / the last */
static int32_t s_stall_frame = -1000;    /* frame that ended a stall > 500 ms */

static int32_t s_wrote = -1;             /* newest local frame written to s_local_ring */
static uint64_t s_send_ns;               /* when the newest local frame first went out */
static int32_t s_send_frame = -1;

static SDL_ThreadID s_game_thread;

/* Outgoing side: the game thread and a 4 ms SDL timer (mid-frame resend +
 * release of held packets + reliable retransmit) share the socket under
 * net.tx_lock. net.active flips under it too, so the timer never sends on
 * a closed socket. */
static SDL_TimerID s_timer;
static Packet s_last_pkt;
static bool s_last_valid;
static uint64_t s_last_send_ns;
static Held s_rx_held[HELD_MAX];         /* incoming, game thread */

/* Sequence numbers (proto 3): every outgoing datagram carries s_tx_seq++,
 * assigned in send_packet under tx_lock. The receiver dedups exact copies
 * (RTP-style anti-replay: a 64-bit window behind the highest seq) and counts
 * reorders but still processes them (their pads may fill a gap). */
static uint16_t s_tx_seq;                /* next tx seq (tx_lock) */
static struct { uint16_t seq; uint64_t send_ns; } s_rtt_ring[64]; /* last 64 sends (tx_lock) */
static bool s_rx_seq_init;
static uint16_t s_rx_seq_top;            /* highest seq seen */
static uint64_t s_rx_seq_bits;           /* bit k: (top-k) received; bit 0 = top */
static unsigned s_rx_dups, s_rx_reorders; /* this stats window */

/* Socket errors: transient ones (EAGAIN/EINTR/ICMP unreachable) are ignored,
 * a hard one is logged once per session and counted. */
static unsigned s_sock_err;
static bool s_warn_sock;

/* Adaptive redundancy: unacked frames repeated per input packet, clamped to
 * loss and rollback depth; the value in force is reported as `red`. */
static int s_red_target = REDUNDANCY;

/* Rollback re-run budget: at most RESIM_BUDGET re-run ticks per present, the
 * rest spill to the next present (snapshot's s_resim_splits). A full pad
 * queue during a re-run forces reuse of a live sample (s_resim_eat). */
#define RESIM_BUDGET 6
static int s_resim_run;                  /* re-run ticks this present */
static unsigned s_resim_eat;             /* live-sample reuses this window */
static bool s_resim_eat_logged;          /* one log per session */
static bool s_timer_tried;               /* SDL_AddTimer attempted this session */

/* Rollback barrier: no frame <= net.rb_barrier can be rolled back to, so
 * fresh_tick predicts nothing up to it either (lockstep). Raised by
 *   - a scene change (heaps are torn down and rebuilt around it);
 *   - a game-thread disc request, to IO_QUIET frames ahead: the tick that
 *     issued it cannot be re-run, the completion lands on a worker thread
 *     some frames later, and the match's own loads trail into its first
 *     frames. aurora's in-flight counters cannot narrow that window: the
 *     audio thread streams music through the same path all match;
 *   - a lost rollback (rb_lost) or a snapshot that could not be taken, to
 *     the newest simulated frame;
 *   - INT32_MAX when snapshot memory ran out (lockstep for the session). */
static int s_scene_last = -1;            /* scene_kind() at the last fresh tick */
static bool s_rb_lost_logged;            /* rb_lost: one log per session */

static void barrier_raise(int32_t f) {
    if (f > net.rb_barrier) {
        net.rb_barrier = f;
    }
}

/* Disc requests issued by the game thread. The audio thread streams music
 * through the same path into ARAM and its own statics, none of which a
 * snapshot covers, so those do not count. */
void pc_net_note_io(void) {
    if (SDL_GetCurrentThreadID() == s_game_thread) {
        barrier_raise(net.frame + IO_QUIET);
    }
}

extern struct GameSceneInfo* gm_804D6720; /* current scene, gmscene.c */

int scene_kind(void) {
    return gm_804D6720 != NULL ? gm_804D6720->scene_kind : -1;
}

bool in_fight(void) {
    int k = scene_kind();
    return k == GS_VS || k == GS_SUDDEN_DEATH;
}

/* Newest frame ticked in the current timeline. */
static int32_t simulated_upto(void) {
    return net.resim ? net.tick_frame : net.frame - 1;
}

/* Newest frame whose checksum is final: simulated with real inputs on both
 * sides and not awaiting a rollback. */
static int32_t confirmed_frame(void) {
    int32_t f = simulated_upto();
    if (f > s_remote_have) {
        f = s_remote_have;
    }
    if (s_rb_frame >= 0 && f >= s_rb_frame) {
        f = s_rb_frame - 1;
    }
    return f;
}

/* ---- transmit --------------------------------------------------------- */

static size_t packet_len(const Packet* pk) {
    return offsetof(Packet, pads) + pk->count * sizeof(WirePad);
}

#if defined(_WIN32)
#define sock_last_err() WSAGetLastError()
#else
#define sock_last_err() errno
#endif

/* "Drained": the non-blocking socket has nothing more (recv stops). */
static bool sock_would_block(int e) {
#if defined(_WIN32)
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

/* Transient and ignorable: an interrupted call or an ICMP unreachable queued
 * against a connectionless UDP socket. Not a would-block, so recv keeps
 * draining and send just drops the datagram. */
static bool sock_transient(int e) {
#if defined(_WIN32)
    return e == WSAEINTR || e == WSAECONNRESET;
#else
    return e == EINTR || e == ECONNREFUSED;
#endif
}

/* One hard socket error logged per session, counted for the report. */
static void sock_err_note(const char* what, int e) {
    s_sock_err++;
    if (!s_warn_sock) {
        s_warn_sock = true;
        pc_log_line("net: %s error %d at frame %d", what, e, net.frame);
    }
}

/* One sendto with error translation (caller holds tx_lock). */
int net_sendto(const void* buf, size_t len) {
    int r = (int) sendto(net.sock, (const char*) buf, len, 0, (struct sockaddr*) &net.peer,
                         net.peer_len);
    if (r < 0) {
        int e = sock_last_err();
        if (!sock_would_block(e) && !sock_transient(e)) {
            sock_err_note("sendto", e);
        }
    }
    return r;
}

/* Wire copy of a host-order input packet out (caller holds tx_lock). Stamps
 * the next tx seq and remembers when it went out, so the matching ack yields
 * a clean RTT sample regardless of resends or delayed acks. */
static void send_packet(Packet* pk) {
    pk->seq = s_tx_seq++;
    int slot = pk->seq & 63;
    s_rtt_ring[slot].seq = pk->seq;
    s_rtt_ring[slot].send_ns = SDL_GetTicksNS();
    Packet w = *pk;
    wire_packet(&w);
    tx(&w, packet_len(pk));
}

/* Slippi's second send: the newest input packet goes out again mid-frame,
 * so a lost packet costs half a frame instead of a whole one. Runs on SDL's
 * timer thread; it only touches the frozen packet copy, the held queue and
 * the reliable transmit queue. Removes itself once the session is gone. */
static Uint32 SDLCALL tx_timer(void* ud, SDL_TimerID id, Uint32 interval) {
    (void) ud;
    (void) id;
    SDL_LockMutex(net.tx_lock);
    if (!net.active) {
        s_timer = 0;
        SDL_UnlockMutex(net.tx_lock);
        return 0;
    }
    tx_flush();
    rel_service();
    uint64_t now = SDL_GetTicksNS();
    if (s_last_valid && now - s_last_send_ns >= 7000000ull) {
        send_packet(&s_last_pkt);
        s_last_send_ns = now;
    }
    if (s_last_valid && now - s_last_send_ns >= 500000000ull) {
        /* Keepalive while the game thread is not ticking (a load): an empty
         * input packet keeps the peer's silence timer and NAT mapping fresh. */
        Packet ka = s_last_pkt;
        ka.count = 0;
        send_packet(&ka);
        s_last_send_ns = now;
    }
    SDL_UnlockMutex(net.tx_lock);
    return interval;
}

static void send_inputs(void) {
    Packet pk;
    memset(&pk, 0, sizeof pk);
    pk.h = hdr('M');
    /* The local sample taken at frame f is the input for frame f + delay. */
    int32_t newest = s_wrote;
    /* Everything the peer has not acked yet, oldest first so its contiguous
     * mark can always advance; the cap only bites when acks are starved. */
    int32_t first = s_last_acked + 1;
    if (first < 0) {
        first = 0;
    }
    if (first <= newest - RING) {
        first = newest - RING + 1;
    }
    /* Repeat only as many unacked frames as loss and rollback depth warrant,
     * clamped to REDUNDANCY: a clean link ships the floor, a lossy or bursty
     * one widens toward the full window. */
    int target = 4 + 2 * s_loss_pct + s_rb_depth_recent;
    if (target < 4) {
        target = 4;
    }
    if (target > REDUNDANCY) {
        target = REDUNDANCY;
    }
    s_red_target = target;
    int32_t count = newest - first + 1;
    if (count > target) {
        count = target;
    }
    if (count < 1) {
        count = 0;
        first = newest > 0 ? newest : 0; /* keep first <= newest so the peer accepts it */
    }
    for (int32_t i = 0; i < count; i++) {
        pk.pads[i] = s_local_ring[(first + i) & (RING - 1)];
    }
    pk.count = (uint8_t) count;
    pk.first = first;
    pk.newest = newest;
    pk.ck_frame = confirmed_frame();
    pk.ck = pk.ck_frame >= 0 ? s_ck_ring[pk.ck_frame & (RING - 1)] : 0;
    uint64_t now = SDL_GetTicksNS();
    SDL_LockMutex(net.tx_lock);
    tx_flush();
    rel_service();
    send_packet(&pk);
    s_last_pkt = pk;
    s_last_valid = true;
    s_last_send_ns = now;
    SDL_UnlockMutex(net.tx_lock);
    if (newest != s_send_frame) {
        s_send_frame = newest;
        s_send_ns = now;
    }
}

static void send_ack(int32_t frame, uint16_t seq) {
    Ack a = { hdr('A'), seq, frame };
    wire_ack(&a);
    SDL_LockMutex(net.tx_lock);
    tx(&a, sizeof a);
    SDL_UnlockMutex(net.tx_lock);
}

static void send_bye(uint8_t reason) {
    Bye b = { hdr('B'), reason };
    tx(&b, sizeof b);
}

/* ---- receive ---------------------------------------------------------- */

/* RTP/IPsec-style anti-replay over the per-datagram seq: SEQ_DUP is an exact
 * copy already seen (drop it), SEQ_REORDER is older but not a duplicate
 * (process it, its pads may fill a gap), SEQ_NEW advances the window. */
enum { SEQ_NEW, SEQ_DUP, SEQ_REORDER };
static int seq_check(uint16_t seq) {
    if (!s_rx_seq_init) {
        s_rx_seq_init = true;
        s_rx_seq_top = seq;
        s_rx_seq_bits = 1; /* bit 0 marks the top as received */
        return SEQ_NEW;
    }
    int16_t d = (int16_t) (seq - s_rx_seq_top);
    if (d > 0) {
        s_rx_seq_bits = d >= 64 ? 0 : s_rx_seq_bits << d;
        s_rx_seq_bits |= 1;
        s_rx_seq_top = seq;
        return SEQ_NEW;
    }
    uint32_t back = (uint32_t) (-d);
    if (back >= 64) {
        return SEQ_REORDER; /* too old to remember: process, cannot dedup */
    }
    uint64_t bit = 1ull << back;
    if (s_rx_seq_bits & bit) {
        return SEQ_DUP;
    }
    s_rx_seq_bits |= bit;
    return SEQ_REORDER;
}

static void on_inputs(const Packet* pk, int n) {
    int count = pk->count > REDUNDANCY ? REDUNDANCY : pk->count;
    /* A peer can be at most window + delays ahead of us; anything else is
     * garbage (a corrupt packet, or a stale process at the peer's address).
     * All arithmetic is in int64 so first/count/newest at INT32 extremes
     * cannot overflow the comparisons. */
    int64_t first = pk->first, newest = pk->newest, ckf = pk->ck_frame;
    int64_t last = first + count - 1;
    if (n < (int) (offsetof(Packet, pads) + (size_t) count * sizeof(WirePad)) || first < 0 ||
        newest < first || last > newest || newest > (int64_t) net.frame + RING / 2 || ckf < -1 ||
        ckf > newest) {
        if (!s_warn_bad) {
            s_warn_bad = true;
            pc_log_line("net: dropped malformed input packet (len %d first %d count %d newest %d "
                        "ck_frame %d at frame %d)",
                        n, pk->first, count, pk->newest, pk->ck_frame, net.frame);
        }
        return;
    }
    /* Only contiguous data is taken; the ack below tells the peer what to
     * resend, so a gap never has to be tracked. */
    if (count > 0 && pk->first <= s_remote_have + 1 && last > s_remote_have &&
        last < net.frame + RING / 2) {
        int32_t upto = simulated_upto();
        for (int32_t f = s_remote_have + 1; f <= last; f++) {
            WirePad pad = pk->pads[f - pk->first];
            WirePad* slot = &s_remote_ring[f & (RING - 1)];
            /* A frame already simulated holds the prediction it ran on. */
            if (f <= upto && memcmp(slot, &pad, sizeof pad) != 0 &&
                (s_rb_frame < 0 || f < s_rb_frame)) {
                s_rb_frame = f;
            }
            *slot = pad;
        }
        s_remote_have = last;
    }
    if (pk->ck_frame > s_remote_ck_frame) {
        s_remote_ck_frame = pk->ck_frame;
        s_remote_ck = pk->ck;
    }
    send_ack(s_remote_have, pk->seq);
    if (pk->newest > s_remote_newest) {
        s_remote_newest = pk->newest;
        if (s_send_frame >= 0 && s_ping_n > 0) {
            /* Clocks are not shared, so the peer's send time is estimated as
             * now - ping/2 (Slippi); the frame difference is in our units. */
            int64_t now_us = (int64_t) (SDL_GetTicksNS() / 1000);
            int64_t their_send_us = now_us - net.ping_us / 2;
            int64_t off = their_send_us - (int64_t) (s_send_ns / 1000) +
                          (int64_t) FRAME_US * (s_send_frame - pk->newest);
            offset_note((int32_t) off);
        }
    }
}

static void on_ack(const Ack* a) {
    /* The peer cannot hold a frame we never sent: a bogus ack above s_wrote
     * would leave send_inputs() with first > newest, i.e. shipping no pads at
     * all for the rest of the session (and INT32_MAX overflows first). */
    if (a->frame > s_last_acked && a->frame <= s_wrote) {
        s_last_acked = a->frame;
    }
    /* Sample RTT only when the ack echoes a seq we actually sent and have
     * not sampled yet; consume it so a delayed duplicate ack cannot skew the
     * estimate. The ring is written by both senders under tx_lock. */
    uint64_t rtt = 0;
    bool got = false;
    SDL_LockMutex(net.tx_lock);
    int slot = a->seq & 63;
    if (s_rtt_ring[slot].seq == a->seq && s_rtt_ring[slot].send_ns != 0) {
        uint64_t now = SDL_GetTicksNS();
        if (now > s_rtt_ring[slot].send_ns) {
            rtt = (now - s_rtt_ring[slot].send_ns) / 1000;
            got = true;
        }
        s_rtt_ring[slot].send_ns = 0;
    }
    SDL_UnlockMutex(net.tx_lock);
    if (got && rtt < 5000000u) {
        net.ping_us = net.ping_us == 0 ? (uint32_t) rtt : (uint32_t) ((net.ping_us * 3 + rtt) / 4);
        s_ping_sum += rtt;
        s_ping_n++;
        if (s_rtt_min == 0 || rtt < s_rtt_min) {
            s_rtt_min = (uint32_t) rtt;
        }
        if (rtt > s_rtt_max) {
            s_rtt_max = (uint32_t) rtt;
        }
        jitter_note((uint32_t) rtt);
    }
}

/* One datagram from the peer, header already in host order and checked. */
static void rx_dispatch(void* buf, int n) {
    union {
        Hdr h;
        Packet pk;
        Ack ack;
        Rel rel;
        RelAck rack;
        Bye bye;
    }* u = buf;
    switch (u->h.magic) {
    case 'M':
        if (n < (int) offsetof(Packet, pads)) {
            return;
        }
        wire_packet(&u->pk);
        switch (seq_check(u->pk.seq)) {
        case SEQ_DUP:
            s_rx_dups++; /* exact copy: the original was already processed and acked */
            break;
        case SEQ_REORDER:
            s_rx_reorders++; /* older but new: still process, its pads may fill a gap */
            on_inputs(&u->pk, n);
            break;
        default:
            on_inputs(&u->pk, n);
            break;
        }
        break;
    case 'A':
        if (n != (int) sizeof(Ack)) {
            return;
        }
        wire_ack(&u->ack);
        s_rx_acks++;
        on_ack(&u->ack);
        break;
    case 'R':
        if (n < (int) offsetof(Rel, payload)) {
            return;
        }
        wire_rel(&u->rel);
        on_rel(&u->rel, n);
        break;
    case 'K':
        if (n != (int) sizeof(RelAck)) {
            return;
        }
        on_rel_ack(&u->rack);
        break;
    case 'B':
        if (n != (int) sizeof(Bye) || s_peer_left) {
            return;
        }
        s_peer_left = true;
        s_status = u->bye.reason <= PC_NET_PEER_INCOMPATIBLE ? u->bye.reason : PC_NET_PEER_LEFT;
        pc_log_line("net: peer left (reason %d) at frame %d", s_status, net.frame);
        break;
    default:
        return;
    }
    s_heard = true;
}

/* Drain the socket. A datagram is dropped, with one log line per session
 * and reason, unless it comes from the peer's address, carries our protocol
 * version and session id (the guest learns the id from the host's first
 * packet) and names the remote player. */
void recv_inputs(void) {
    for (;;) {
        union {
            Hdr h;
            uint8_t raw[sizeof(Rel)];
        } u;
        struct sockaddr_storage from;
        socklen_t from_len = sizeof from;
        int n = (int) recvfrom(net.sock, (char*) &u, sizeof u, 0, (struct sockaddr*) &from, &from_len);
        if (n < 0) {
            int e = sock_last_err();
            if (sock_would_block(e)) {
                break; /* nothing more queued */
            }
            if (sock_transient(e)) {
                continue; /* EINTR / ICMP unreachable: more may still be queued */
            }
            sock_err_note("recvfrom", e);
            break;
        }
        if (n == 0) {
            continue; /* empty datagram: nothing to parse */
        }
        s_rx_pkts++;
        if (n < (int) sizeof(Hdr)) {
            continue;
        }
        if (!addr_eq(&from, &net.peer)) {
            if (!s_warn_src) {
                s_warn_src = true;
                pc_log_line("net: dropped a datagram from an address other than the peer's");
            }
            continue;
        }
        wire_hdr(&u.h);
        if (u.h.version != WIRE_VERSION) {
            if (!s_peer_left) {
                s_peer_left = true;
                s_status = PC_NET_PEER_INCOMPATIBLE;
                pc_log_line("net: peer speaks protocol %u, we speak %u", u.h.version, WIRE_VERSION);
            }
            continue;
        }
        if (net.session == 0 && net.local == 1 && u.h.session != 0) {
            net.session = u.h.session; /* the host picked it */
        }
        if (u.h.session != net.session || u.h.player != net.remote) {
            if (!s_warn_sess) {
                s_warn_sess = true;
                pc_log_line("net: dropped a datagram for session %08x player %u (ours %08x, peer %d)",
                            u.h.session, u.h.player, net.session, net.remote);
            }
            continue;
        }
        if (net.sim_rx_delay_ns == 0) {
            rx_dispatch(&u, n);
        } else if (held_put(s_rx_held, &u, (size_t) n, SDL_GetTicksNS() + net.sim_rx_delay_ns) < 0) {
            continue; /* simulator queue full: one more loss */
        }
    }
    if (net.sim_rx_delay_ns != 0) {
        uint64_t now = SDL_GetTicksNS();
        for (Held* h; (h = held_due(s_rx_held, now)) != NULL;) {
            h->release_ns = 0;
            rx_dispatch(h->buf, h->len);
        }
    }
}

static void check_desync(void) {
    if (net.desync_reported || net.hs == HS_PENDING || s_remote_ck_frame < net.ck_from ||
        s_remote_ck_frame > confirmed_frame() || s_remote_ck_frame <= net.frame - RING) {
        return;
    }
    uint32_t mine = s_ck_ring[s_remote_ck_frame & (RING - 1)];
    if (mine != s_remote_ck) {
        net.desync_reported = true;
        pc_log_line("net: DESYNC at frame %d (local %08x remote %08x)", s_remote_ck_frame, mine,
                    s_remote_ck);
    }
}

/* Block until the remote input for `need` is here. False on timeout or
 * when the peer announced it is gone (s_status says which). */
static bool wait_remote(int32_t need) {
    if (s_remote_have >= need) {
        return true;
    }
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t last_send = t0;
    s_stalls++;
    for (;;) {
        recv_inputs();
        if (s_remote_have >= need) {
            break;
        }
        if (s_peer_left) {
            return false;
        }
        uint64_t now = SDL_GetTicksNS();
        uint64_t limit = (s_heard ? STALL_TIMEOUT_MS : CONNECT_TIMEOUT_MS) * 1000000ull;
        if (now - t0 > limit) {
            s_status = PC_NET_PEER_TIMEOUT;
            return false;
        }
        if (now - last_send > 16000000ull) {
            send_inputs(); /* peer may be waiting on us, or lost our packets */
            last_send = now;
        }
        SDL_DelayNS(500000);
    }
    uint64_t dt = SDL_GetTicksNS() - t0;
    if (dt > s_stall_ns_max) {
        s_stall_ns_max = dt;
    }
    if (dt > 500000000ull) {
        s_stall_frame = net.frame;
    }
    return true;
}

/* ---- public ----------------------------------------------------------- */

bool pc_net_active(void) {
    return net.active;
}

int pc_net_local_player(void) {
    return net.local;
}

/* Back to frame 0 with empty rings; called with the timer parked (net.active
 * false), so only the game thread is looking. */
static bool s_seen_remote;

static void session_reset(void) {
    s_seen_remote = false;
    memset(s_local_ring, 0, sizeof s_local_ring);
    memset(s_remote_ring, 0, sizeof s_remote_ring);
    memset(s_ck_ring, 0, sizeof s_ck_ring);
    sim_reset();
    memset(s_rx_held, 0, sizeof s_rx_held);
    net.frame = 0;
    net.tick_frame = -1;
    net.resim = false;
    s_remote_have = s_remote_newest = s_last_acked = s_rb_frame = s_remote_ck_frame = -1;
    s_remote_ck = 0;
    net.desync_reported = s_heard = s_peer_left = false;
    s_warn_src = s_warn_sess = s_warn_bad = false;
    s_status = PC_NET_PEER_OK;
    s_stalls = net.skips = s_advances = s_rollbacks = s_rb_lost = 0;
    s_rb_depth_max = s_rb_depth_cur = s_rb_depth_recent = 0;
    s_stall_ns_max = 0;
    s_stall_frame = -1000;
    net.ping_us = 0;
    s_ping_sum = 0;
    s_ping_n = 0;
    s_rtt_min = s_rtt_max = 0;
    net.tx_pkts = s_rx_pkts = net.tx_inputs = s_rx_acks = 0;
    s_tx_seq = 0;
    memset(s_rtt_ring, 0, sizeof s_rtt_ring);
    s_rx_seq_init = false;
    s_rx_seq_top = 0;
    s_rx_seq_bits = 0;
    s_rx_dups = s_rx_reorders = 0;
    s_sock_err = 0;
    s_warn_sock = false;
    s_red_target = REDUNDANCY;
    s_resim_run = 0;
    s_resim_eat = 0;
    s_resim_eat_logged = false;
    s_timer_tried = false;
    s_loss_pct = 0;
    sync_reset();
    s_wrote = -1;
    s_send_ns = 0;
    s_send_frame = -1;
    net.rb_barrier = -1;
    s_scene_last = -1;
    s_rb_lost_logged = false;
    s_last_valid = false;
    rel_reset();
    net.hs = HS_IDLE;
    net.hs_host = false;
    net.seed = 0;
    net.start_frame = -1;
    net.ck_from = 0;
    snaps_free();
}

void pc_net_disconnect(void) {
    if (net.sock == SOCK_INVALID) {
        return;
    }
    /* Timer first: once removed it cannot fire again, and taking tx_lock
     * below waits out a callback already running, so nothing touches the
     * socket after this returns. */
    if (s_timer) {
        SDL_RemoveTimer(s_timer);
        s_timer = 0;
    }
    SDL_LockMutex(net.tx_lock);
    if (!s_peer_left) {
        /* Tell the peer why so it need not wait out the 7 s silence; sent
         * twice, unacked (the timeout is the fallback). */
        uint8_t why = s_status == PC_NET_PEER_OK ? PC_NET_PEER_LEFT : (uint8_t) s_status;
        net.sim_hold = false; /* straight out: the held queue dies with the socket */
        send_bye(why);
        send_bye(why);
    }
    net.active = false;
    sock_close(net.sock);
    net.sock = SOCK_INVALID;
    net.hs = HS_IDLE;
    rules_restore();
    HSD_PadLibData.qtype = 0;
    SDL_UnlockMutex(net.tx_lock);
    pc_log_line("net: disconnected at frame %d (status %d)", net.tick_frame, s_status);
    snaps_free();
}

/* MELEE_NET_EXIT_AFTER_FRAMES=n (tools/net_test.py): the instance that reaches
 * n first sends BYE, and the other one is a frame or two behind (the clocks
 * differ by the time offset), so its own check would never fire — it would sit
 * at the title until the harness killed it. A BYE this close to the target ends
 * the test as done; a BYE for a real reason still logs DESYNC etc. first. */
static long s_exit_after; /* 0 = knob unset */
static void exit_if_test_done(void) {
    if (s_exit_after > 0 && net.frame >= s_exit_after - 16) {
        pc_log_line("net: test done at frame %d", net.frame);
        pc_net_disconnect(); /* BYE goes out, the peer need not wait out the timeout */
        exit(0);
    }
}

int pc_net_peer_status(void) {
    return s_status;
}

int pc_net_quality(void) {
    if (!net.active) {
        return 0;
    }
    if (s_peer_left || net.frame - s_stall_frame < 120) {
        return 2;
    }
    if (s_loss_pct >= 5 || s_rb_depth_recent >= 4 || jitter_us() >= 8000) {
        return 1;
    }
    return 0;
}

bool pc_net_connect(const char* ip, uint16_t port, int player, uint32_t seed) {
    if (net.tx_lock == NULL) {
        net.tx_lock = SDL_CreateMutex();
        sock_startup();
    }
    pc_net_disconnect();
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(ip, portstr, &hints, &res) != 0 || res == NULL) {
        pc_log_line("net: cannot resolve %s:%u", ip, port);
        return false;
    }
    memcpy(&net.peer, res->ai_addr, res->ai_addrlen);
    net.peer_len = (socklen_t) res->ai_addrlen;
    int family = res->ai_family;
    freeaddrinfo(res);

    sock_t sock = socket(family, SOCK_DGRAM, 0);
    if (sock == SOCK_INVALID) {
        pc_log_line("net: socket() failed");
        return false;
    }
    const char* lport = getenv("MELEE_NET_PORT");
    unsigned short bind_port = (unsigned short) (lport ? atoi(lport) : 41000);
    struct sockaddr_storage local;
    memset(&local, 0, sizeof local);
    socklen_t local_len;
    if (family == AF_INET6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*) &local;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(bind_port);
        local_len = sizeof *a;
    } else {
        struct sockaddr_in* a = (struct sockaddr_in*) &local;
        a->sin_family = AF_INET;
        a->sin_port = htons(bind_port);
        local_len = sizeof *a;
    }
    if (bind(sock, (struct sockaddr*) &local, local_len) != 0) {
        pc_log_line("net: bind(%u) failed", bind_port);
        sock_close(sock);
        return false;
    }
    if (!sock_nonblock(sock)) {
        pc_log_line("net: could not put the socket in non-blocking mode");
        sock_close(sock);
        return false;
    }
    /* Ask the network for expedited forwarding (DSCP EF / 46): low-latency
     * queueing where the path honours it, harmless where it does not. */
#if defined(_WIN32)
    /* ponytail: Windows ignores IP_TOS for DSCP; real marking there needs the
     * qWAVE API (QOSAddSocketToFlow). Skipped in the prototype. */
#else
    {
        int tos = 0xB8; /* DSCP 46 << 2, ECN 0 */
        int qr = family == AF_INET6
                     ? setsockopt(sock, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos)
                     : setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
        if (qr != 0) {
            pc_log_line("net: could not set DSCP EF on the socket (non-fatal)");
        }
    }
#endif
    session_reset();
    /* A full raw queue (any stall) with qtype 0 makes the pad alarm shift
     * qread, dropping the head write_head() just filled: the tick then eats
     * a raw sample (remote port = no controller) and the peers diverge. 2
     * drops the new raw sample instead; the local input is read from the
     * head anyway. */
    HSD_PadLibData.qtype = 2;
    net.local = player ? 1 : 0;
    net.remote = 1 - net.local;
    const char* delay = getenv("MELEE_NET_DELAY");
    net.delay_auto = delay == NULL || strcmp(delay, "auto") == 0;
    net.delay = net.delay_auto ? 2 : atoi(delay);
    if (net.delay < 0 || net.delay >= RING / 2) {
        net.delay = 2;
    }
    net.delay_next = net.delay;
    s_wrote = net.delay - 1; /* frames 0..delay-1 stay neutral on both sides */
    sim_env(bind_port);
    /* The host names the session; the guest takes it from the first packet. */
    net.session = net.local == 0 ? (uint32_t) (SDL_GetPerformanceCounter() ^ (uint64_t) getpid() << 20) | 1u : 0;
    net.seed = seed;
    if (seed != 0) {
        *HSD_RandSeedPtr = seed;
    }
    SDL_LockMutex(net.tx_lock);
    net.sock = sock;
    net.active = true;
    SDL_UnlockMutex(net.tx_lock);
    pc_log_line("net: rollback with %s:%u, local port %u, player P%d, delay %s%d, window %d, "
                "seed %u, session %08x, proto %d, sim loss %d%% delay %d/%d ms jitter %d reorder %d%% "
                "dup %d%% burst %d",
                ip, port, bind_port, net.local + 1, net.delay_auto ? "auto " : "", net.delay, WINDOW, seed,
                net.session, WIRE_VERSION, net.sim_loss, (int) (net.sim_delay_ns / 1000000),
                (int) (net.sim_rx_delay_ns / 1000000), net.sim_jitter_ms, net.sim_reorder, net.sim_dup,
                net.sim_burst);
    return true;
}

void pc_net_init(void) {
    s_game_thread = SDL_GetCurrentThreadID(); /* pc_platform_init runs on it */
    const char* peer = getenv("MELEE_NET");
    if (peer == NULL || peer[0] == '\0') {
        return;
    }
    char host[256];
    const char* colon = strrchr(peer, ':');
    if (colon == NULL || (size_t) (colon - peer) >= sizeof host) {
        pc_log_line("net: MELEE_NET must be host:port");
        return;
    }
    memcpy(host, peer, (size_t) (colon - peer));
    host[colon - peer] = '\0';
    const char* player = getenv("MELEE_NET_PLAYER");
    const char* seed = getenv("MELEE_SEED");
    if (seed == NULL) {
        pc_log_line("net: MELEE_SEED not set; peers will diverge at the first random call");
    }
    pc_net_connect(host, (uint16_t) atoi(colon + 1), player && player[0] == '1',
                   seed ? (uint32_t) strtoul(seed, NULL, 0) : 0);
}

int32_t pc_net_frame(void) {
    return net.tick_frame;
}

bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks) {
    if (!net.active) {
        return false;
    }
    *ping_ms = (int) (net.ping_us / 1000);
    *delay_frames = net.delay;
    *rollbacks = s_rollbacks;
    return true;
}

bool pc_net_resim(void) {
    return net.resim || net.synctest;
}

/* ---- rollback ---------------------------------------------------------
 * One snapshot per predicted frame (Slippi cadence), taken right before the
 * tick that consumes the prediction. When the real input for such a frame
 * arrives and differs, that frame's snapshot is restored and every frame
 * since is ticked again from the input rings with the resim flag on. */

static void predict(int32_t f) {
    static const WirePad neutral;
    s_remote_ring[f & (RING - 1)] =
        s_remote_have >= 0 ? s_remote_ring[s_remote_have & (RING - 1)] : neutral;
}

/* Snapshot before a predicted frame. Behind the barrier nothing is taken
 * (the frame cannot be rolled back to anyway); when the buffer cannot be
 * grown the session drops to lockstep for good rather than mispredicting
 * into a desync. */
static void snap_predicted(int32_t f) {
    Snapshot* s = snap_slot(f);
    if (f <= net.rb_barrier) {
        s->frame = -1;
        return;
    }
    if (!snapshot_take(s, f)) {
        barrier_raise(INT32_MAX);
        pc_log_line("net: out of memory for snapshots at frame %d, lockstep from here", f);
    }
}

/* Ports 0-3 of the queue head become the synced inputs for frame f. */
static void write_head(PADStatus* head, int32_t f) {
    static const WirePad neutral;
    const WirePad* mine = f >= net.delay ? &s_local_ring[f & (RING - 1)] : &neutral;
    from_wire(&head[net.local], mine);
    const WirePad* theirs = &s_remote_ring[f & (RING - 1)];
    from_wire(&head[net.remote], theirs);
    if (!s_seen_remote && theirs->button != 0) {
        s_seen_remote = true;
        pc_log_line("net: first remote button press (%04x) at frame %d", theirs->button, f);
    }
    for (int i = 2; i < 4; i++) {
        memset(&head[i], 0, sizeof head[i]);
        head[i].err = PAD_ERR_NO_CONTROLLER;
    }
}

/* Re-expose the slot the last tick consumed so the next tick has one to
 * consume; the raw sample in it was already captured. Fails when full. */
static PADStatus* unconsume(void) {
    PadLibData* p = &HSD_PadLibData;
    if (p->qcount >= p->qnum) {
        return NULL;
    }
    p->qread = (uint8_t) ((p->qread + p->qnum - 1) % p->qnum);
    p->qcount++;
    return &p->queue->stat[p->qread * 4];
}

/* Set up the re-run of frame f: refresh its snapshot if it is still
 * predicted (the old one holds the discarded timeline), then feed it. */
static void resim_prepare(int32_t f) {
    if (f > s_remote_have) {
        predict(f);
        snap_predicted(f);
    }
    PADStatus* head = unconsume();
    if (head == NULL) {
        /* ponytail: the pad queue is full (a hitch queued several samples
         * during the rollback) and the game reads the queue head directly
         * through HSD_PadRenewMasterStatus (controller.c), not a pointer we
         * own, so a private re-run buffer would never be read. This re-run
         * therefore reuses the live head, eating one real sample; time sync
         * pays it back. Counted (resim_eat) so the report shows how often. */
        PadLibData* p = &HSD_PadLibData;
        head = &p->queue->stat[p->qread * 4];
        s_resim_eat++;
        if (!s_resim_eat_logged) {
            s_resim_eat_logged = true;
            pc_log_line("net: pad queue full during a rollback at frame %d, re-run reused a live "
                        "sample", f);
        }
    }
    write_head(head, f);
    s_ck_ring[f & (RING - 1)] = frame_checksum(head);
    net.tick_frame = f;
}

static bool rollback_to(int32_t f) {
    Snapshot* s = snap_slot(f);
    const char* why = s->buf == NULL || s->frame != f ? "no snapshot"
                      : f <= net.rb_barrier          ? "behind the barrier"
                                                     : snapshot_unusable(s);
    if (why != NULL) {
        /* The misprediction stays in the timeline: the peers have diverged
         * unless the inputs happened to match. Everything simulated so far
         * is now behind the barrier so it is not rolled back to later. */
        s_rb_lost++;
        barrier_raise(simulated_upto());
        if (!s_rb_lost_logged) {
            s_rb_lost_logged = true;
            pc_log_line("net: cannot roll back to frame %d (%s), expect a desync", f, why);
        }
        return false;
    }
    int depth = simulated_upto() - f + 1;
    /* The pad queue and its bookkeeping sit inside the snapshot but belong
     * to the present: keep the live copy so raw samples queued since the
     * snapshot survive, and the re-run ticks consume slots unconsume() hands
     * them. Interrupts stay off so the pad alarm cannot land in between. */
    bool intr = OSDisableInterrupts();
    PadLibData pad = HSD_PadLibData;
    HSD_PadData queue[8];
    int qn = pad.qnum > 8 ? 8 : pad.qnum;
    memcpy(queue, pad.queue, qn * sizeof *queue);
    snapshot_restore(s);
    memcpy(pad.queue, queue, qn * sizeof *queue);
    HSD_PadLibData = pad;
    OSRestoreInterrupts(intr);
    s_rollbacks++;
    if (depth > s_rb_depth_max) {
        s_rb_depth_max = depth;
    }
    if (depth > s_rb_depth_cur) {
        s_rb_depth_cur = depth;
    }
    net.resim = true;
    resim_prepare(f);
    return true;
}

/* ---- per-tick entry --------------------------------------------------- */

/* A fresh frame: capture the local sample, exchange inputs, predict or
 * stall, and feed the queue head. `raw` is false for the extra tick a
 * time-sync advance adds, which reuses the last physical sample. */
static void fresh_tick(PADStatus* head, bool raw) {
    if (net.active) {
        if (raw) {
            s_raw_last = head[0];
        }
        /* One slot per tick; a delay change (delay_auto) leaves a gap to fill
         * with the same sample, or already-sent frames that must not move. */
        for (int32_t w = s_wrote + 1; w <= net.frame + net.delay; w++) {
            to_wire(&s_local_ring[w & (RING - 1)], &s_raw_last);
            s_wrote = w;
        }
        send_inputs();
        recv_inputs();
        if (s_peer_left) {
            exit_if_test_done();
            pc_net_disconnect();
            return;
        }
        /* A scene change tears heaps down and rebuilds them: nothing across
         * it can be restored, and its loads trail into the next frames. */
        int scene = scene_kind();
        if (scene != s_scene_last) {
            s_scene_last = scene;
            barrier_raise(net.frame + IO_QUIET);
        }
        /* Predict at most WINDOW frames past the remote, and only in a
         * fight past the barrier (plan §10.4: menus lockstep, rollback
         * armed in GS_VS once the match's own loads have gone quiet). */
        bool lockstep = !in_fight() || net.frame <= net.rb_barrier;
        int32_t need = lockstep ? net.frame : net.frame - WINDOW;
        if (!wait_remote(need)) {
            if (!s_peer_left) {
                pc_log_line("net: peer silent for %d ms at frame %d, leaving netplay",
                            s_heard ? STALL_TIMEOUT_MS : CONNECT_TIMEOUT_MS, net.frame);
            }
            pc_net_disconnect();
            return;
        }
        if ((net.frame % SYNC_INTERVAL) == 0 && net.frame > 0) {
            time_sync();
        }
        if (s_remote_have < net.frame) {
            predict(net.frame);
            snap_predicted(net.frame);
        }
        write_head(head, net.frame); /* after anything that can run the pad alarm */
    }
    replay_feed(head);

    if (net.start_frame == net.frame && net.hs != HS_FAILED) {
        *HSD_RandSeedPtr = net.seed; /* both peers enter the match from the agreed seed */
    }
    uint32_t ck = frame_checksum(head);
    s_ck_ring[net.frame & (RING - 1)] = ck;
    if (net.active) {
        record_state(head, net.frame);
        bool before = net.desync_reported;
        check_desync();
        if (net.desync_reported && !before) {
            dump_states_around(s_remote_ck_frame);
        }
    }
    record_frame(head, ck);

    if ((net.frame % 60) == 0) {
        s_rb_depth_recent = s_rb_depth_cur; /* pc_net_quality: deepest rollback last second */
        s_rb_depth_cur = 0;
    }
    if ((net.frame % 600) == 0 && net.frame > 0 && net.active) {
        s_loss_pct = net.tx_inputs > s_rx_acks && net.tx_inputs > 0
                         ? (int) (100 * (net.tx_inputs - s_rx_acks) / net.tx_inputs)
                         : 0;
        pc_log_line("net: frame %d, rollbacks %u (max depth %d, lost %u), stalls %u (worst "
                    "%.1f ms), skips %u, advances %u, ping %u ms (avg %.0f, min %u, max %u, "
                    "jitter %.1f), loss %d%% (%u tx %u rx), offset %+.1f ms, remote behind %d, "
                    "barrier %d, quality %d, dup %u reorder %u sock_err %u resim_eat %u red %d",
                    net.frame, s_rollbacks, s_rb_depth_max, s_rb_lost, s_stalls,
                    s_stall_ns_max / 1e6, net.skips, s_advances, net.ping_us / 1000,
                    s_ping_n ? s_ping_sum / 1000.0 / s_ping_n : 0.0, s_rtt_min / 1000,
                    s_rtt_max / 1000, jitter_us() / 1000.0, s_loss_pct, net.tx_pkts, s_rx_pkts,
                    net.offset_last / 1000.0, net.frame - 1 - s_remote_have, net.rb_barrier,
                    pc_net_quality(), s_rx_dups, s_rx_reorders, s_sock_err, s_resim_eat,
                    s_red_target);
        snap_stats_report();
        s_stall_ns_max = 0;
        s_ping_sum = 0;
        s_ping_n = 0;
        s_rtt_min = s_rtt_max = 0;
        net.tx_pkts = s_rx_pkts = net.tx_inputs = s_rx_acks = 0;
        s_rx_dups = s_rx_reorders = s_sock_err = s_resim_eat = 0;
    }
    net.tick_frame = net.frame;
    net.frame++;
}

void pc_net_sync(void) {
    static bool opened;
    if (!opened) {
        opened = true;
        record_open();
        net.synctest = getenv("MELEE_NET_SYNCTEST") != NULL;
        if (net.synctest) {
            pc_log_line("net: synctest on (every tick simulated twice, sound off)");
        }
        const char* ea = getenv("MELEE_NET_EXIT_AFTER_FRAMES");
        s_exit_after = ea != NULL ? atol(ea) : 0;
    }
    if (net.active && s_exit_after > 0 && net.frame >= s_exit_after) {
        exit_if_test_done();
    }
    if (net.active) {
        SDL_LockMutex(net.tx_lock);
        if (s_timer == 0) {
            s_timer = SDL_AddTimer(4, tx_timer, NULL);
        }
        SDL_UnlockMutex(net.tx_lock);
    }
    if (net.synctest) {
        synctest_before_tick();
    }
    if (!net.active && !record_active()) {
        net.frame++;
        return;
    }
    PadLibData* p = &HSD_PadLibData;
    fresh_tick(&p->queue->stat[p->qread * 4], true);
}

bool pc_net_after_tick(void) {
    if (net.synctest) {
        return synctest_after_tick();
    }
    if (!net.active) {
        return false;
    }
    recv_inputs();
    if (s_rb_frame >= 0) {
        int32_t f = s_rb_frame;
        s_rb_frame = -1;
        if (rollback_to(f)) {
            return true;
        }
    }
    if (net.resim) {
        if (net.tick_frame + 1 < net.frame) {
            resim_prepare(net.tick_frame + 1);
            return true;
        }
        net.resim = false;
    }
    check_desync();
    handshake_test();
    if (net.advance_left > 0 && (net.frame % 5) == 0) {
        /* Behind the peer: one extra tick this present, once per 5 frames. */
        PADStatus* head = unconsume();
        if (head != NULL) {
            net.advance_left--;
            s_advances++;
            fresh_tick(head, false);
            return true;
        }
    }
    return false;
}
