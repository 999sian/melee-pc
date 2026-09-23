/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay prototype: two instances exchange one PADStatus per frame over UDP
 * and run in rollback lockstep modelled on Slippi (docs/netcode-plan.md §4):
 * a remote input that has not arrived yet is predicted as "repeat last", a
 * snapshot is taken before every predicted tick, and when the real input
 * turns out different the state is restored and the frames since re-run.
 * Both peers must boot with the same disc and no memory card; every session
 * agrees the rest -- seed, rules and unlock state -- in the match handshake,
 * the env path below included (net_handshake.c's handshake_direct). Sessions
 * can also be opened at runtime through pc_net_connect() (src/pc/net_lan.h).
 *
 *   MELEE_NET=host:port          peer address (enables netplay at boot)
 *   MELEE_NET_PORT=n             local UDP port (default 41000)
 *   MELEE_NET_PLAYER=0|1         which controller port the local player drives
 *   MELEE_NET_DELAY=n|auto       input delay in frames (default auto: from ping and jitter)
 *   MELEE_NET_RECONNECT_MS=ms    resume an interrupted session for this long (default 3000, 0 off)
 *   MELEE_NET_SIM_LOSS=percent   drop that share of outgoing packets
 *   MELEE_NET_SIM_DELAY_MS=ms    hold every outgoing packet that long
 *   MELEE_NET_SIM_DELAY_RX_MS=ms hold every incoming packet that long (asymmetric links)
 *   MELEE_NET_SIM_JITTER_MS=ms   uniform +-ms on the outgoing delay (reorders when > delay)
 *   MELEE_NET_SIM_REORDER=pct    hold that share of packets behind the next one
 *   MELEE_NET_SIM_DUP=pct        send that share of packets twice
 *   MELEE_NET_SIM_BURST=n        every 5 s drop n consecutive outgoing packets
 *   (the simulator's PRNG is seeded from MELEE_NET_PORT, so runs repeat)
 *
 * This file is the session, the socket and the rollback loop; the wire
 * codec, link simulator, reliable channel, handshake, time sync and
 * snapshots are the net_*.c modules listed in net_internal.h. */
#include "compat.h"
#include "pc/net_internal.h"

#include <dolphin/ar.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/lb/lb_0195.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <poll.h>
#endif

/* ---- state ------------------------------------------------------------ */

struct NetSession net = {
    .sock = SOCK_INVALID, .tick_frame = -1, .rb_barrier = -1, .start_frame = -1};

static WirePad s_local_ring[RING];   /* indexed by frame & (RING-1) */
static WirePad s_remote_ring[RING];  /* real input, or the prediction in use */
static int32_t s_remote_have = -1;   /* newest contiguous real remote frame */
static int32_t s_remote_newest = -1; /* newest frame the peer reported holding */
static int32_t s_last_acked = -1;    /* newest local frame the peer holds */
static int32_t s_rb_frame = -1;      /* oldest mispredicted frame not rolled back yet */
static int32_t s_remote_ck_frame = -1;
static uint32_t s_remote_ck;
static uint32_t s_ck_ring[RING]; /* our checksum entering each frame */
static bool s_heard;             /* any packet from the peer yet */
static uint64_t s_last_rx_ns;    /* when the last accepted datagram arrived */
/* When the contiguous remote mark last moved, 0 before the first time. The
 * no-progress bound is measured from here, not from the start of a wait:
 * wait_remote's own clock restarts on every call, so a peer that advanced one
 * frame just under the bound each time kept the game frozen for ever. */
static uint64_t s_progress_ns;
static bool s_no_progress; /* the wait ended on a talking peer that never advanced */
/* Session-local seed history must not survive a reconnect. */
static uint32_t s_seed_after_tick;
static bool s_seed_have;
static bool s_lockstep;      /* snapshot failure: stop predicting, retain older corrections */
static PADStatus s_raw_last; /* newest physical sample (port 0) */
static int s_status;         /* pc_net_peer_status(); kept until the next connect */
static bool s_peer_left;     /* BYE received or version mismatch: stop waiting */
static bool s_warn_src, s_warn_sess, s_warn_bad, s_warn_mac; /* one reject line each per session */
/* One authenticated datagram has arrived, so the peer holds the session key
 * and nothing unauthenticated is accepted from here on (recv_inputs). Until
 * then s_mac_quiet counts what was let through for want of one, so a pair
 * that never agrees on a key says so instead of looking protected. */
static bool s_mac_seen;
static int s_mac_quiet;
#define MAC_QUIET_MAX 120 /* ~2 s of a peer's traffic at 60 Hz */

static unsigned s_stalls, s_advances, s_rollbacks, s_rb_lost;
static int s_rb_depth_max;
static uint64_t s_stall_ns_max;
static uint64_t s_ping_sum;
static unsigned s_ping_n;
static uint32_t s_rtt_min, s_rtt_max; /* this stats window */
static unsigned s_rx_pkts;            /* datagrams this stats window */
static unsigned s_rx_acks;            /* every input packet earns one ack: the gap is loss */
static int s_loss_pct;                /* round-trip loss, one-second window */
static unsigned s_loss_tx_mark, s_loss_ack_mark; /* counters at that window's start */
static int s_rb_depth_cur, s_rb_depth_recent;    /* deepest rollback this second / the last */
static int32_t s_stall_frame = -1000;            /* frame that ended a stall > 500 ms */

/* Reconnect phase; the machine and the numbers are down in "resume after an
 * interruption", these live here because send_inputs and fresh_tick read
 * them. MELEE_NET_RECONNECT_MS overrides the window at connect. */
#define RECONNECT_MS 3000
/* RSM_, not RC_: mingw-w64's wingdi.h defines RC_NONE as an empty macro
 * (raster capabilities), so RC_NONE broke the whole enum on Windows. */
enum { RSM_NONE, RSM_ACTIVE, RSM_FAILED };
static int s_rc;         /* reconnect phase */
static uint64_t s_rc_ns; /* when the phase opened */
static bool s_rc_sent;   /* our RESUME is out for this phase */
static int32_t s_rc_window_ms = RECONNECT_MS;

static int32_t s_wrote = -1; /* newest local frame written to s_local_ring */
static uint64_t s_send_ns;   /* when the newest local frame first went out */
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

/* Receive side: rx_main, a thread of its own, drains the socket, runs the
 * authentication gate, acks every input packet and times the round trip of
 * every ack the moment the datagram lands; recv_inputs applies what it
 * queued on the game thread (the "receive" section says why). s_rx_lock
 * guards the queue and everything the gate owns: s_heard, s_last_rx_ns, the
 * MAC grace state, the seq window, s_rx_have, s_rx_held, the sticky
 * "peer gone" pair and the rx counters. It is taken before net.tx_lock,
 * never while holding it. */
static SDL_Thread* s_rx_thread; /* NULL: recv_inputs drains the socket itself */
static atomic_bool s_rx_run;
static SDL_Mutex* s_rx_lock;
static atomic_int s_frame_pub;   /* net.frame as the receive thread may read it */
static int32_t s_rx_have = -1;   /* newest contiguous remote frame queued (acked) */
static bool s_rx_left;           /* BYE or protocol mismatch seen: s_peer_left to be */
static int s_rx_why;             /* the s_status that goes with it */
static unsigned s_rx_full;       /* datagrams dropped on a full queue, this window */
static Held s_rx_held[HELD_MAX]; /* incoming, receive thread */

/* Sequence numbers (proto 3): every outgoing datagram carries s_tx_seq++,
 * assigned in send_packet under tx_lock. The receiver dedups exact copies
 * (RTP-style anti-replay: a 64-bit window behind the highest seq) and counts
 * reorders but still processes them (their pads may fill a gap). */
static uint16_t s_tx_seq; /* next tx seq (tx_lock) */
static struct {
    uint16_t seq;
    uint64_t send_ns;
} s_rtt_ring[64]; /* last 64 sends (tx_lock) */
static bool s_rx_seq_init;
static uint16_t s_rx_seq_top;             /* highest seq seen */
static uint64_t s_rx_seq_bits;            /* bit k: (top-k) received; bit 0 = top */
static unsigned s_rx_dups, s_rx_reorders; /* this stats window */

/* Socket errors: transient ones (EAGAIN/EINTR/ICMP unreachable) are ignored,
 * a hard one is logged once per session and counted. */
static unsigned s_sock_err;
static bool s_warn_sock;
static unsigned s_rx_malformed, s_rx_bad_src, s_rx_bad_sess, s_rx_bad_player, s_tx_would_block;
static unsigned s_rx_bad_mac; /* datagrams refused by the authentication gate */

/* Adaptive redundancy: unacked frames repeated per input packet, clamped to
 * loss and rollback depth; the value in force is reported as `red`. The
 * floor is normally REDUNDANCY_FLOOR and rises to the full window while a
 * reconnect refills a gap, so send_inputs pays a load rather than a test. */
#define REDUNDANCY_FLOOR 4
static int s_red_target = REDUNDANCY;
static int s_red_floor = REDUNDANCY_FLOOR;

/* Rollback re-run cost: every re-run tick of a rollback happens inside the
 * one present that started it, so a deep correction lands as a long frame.
 * The count is reported (snap_stats_report's "resim/present max") so that
 * cost is visible rather than assumed. A full pad queue during a re-run
 * forces reuse of a live sample (s_resim_eat). */
static int s_resim_run;         /* re-run ticks in the present now running */
static unsigned s_resim_eat;    /* live-sample reuses this window */
static bool s_resim_eat_logged; /* one log per session */
static bool s_timer_tried;      /* SDL_AddTimer attempted this session */

/* Rollback barrier: no frame <= net.rb_barrier can be rolled back to, so
 * fresh_tick predicts nothing up to it either (lockstep). Raised by
 *   - a scene change (heaps are torn down and rebuilt around it);
 *   - a game-thread disc request, to IO_QUIET frames ahead: the tick that
 *     issued it cannot be re-run, the completion lands on a worker thread
 *     some frames later, and the match's own loads trail into its first
 *     frames. aurora's in-flight counters cannot narrow that window: the
 *     audio thread streams music through the same path all match;
 *   - a lost rollback (rb_lost), to the newest simulated frame.
 *
 * Snapshot failure uses s_lockstep instead: earlier predicted frames must
 * remain eligible for correction. */
static int s_scene_last = -1; /* scene_kind() at the last fresh tick */
static bool s_rb_lost_logged; /* rb_lost: one log per session */

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
        int lead = in_fight() ? 2 : IO_QUIET;
        barrier_raise(net.frame + lead);
    }
}

extern struct GameSceneInfo* gm_804D6720; /* current scene, gmscene.c */

int scene_kind(void) {
    return gm_804D6720 != NULL ? gm_804D6720->scene_kind : -1;
}

bool pc_net_chat_available(void) {
    int scene = scene_kind();
    return net.active && (scene == GS_CSS || scene == GS_RESULTS || scene == GS_ONLINE_LOBBY);
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

/* Block until the socket has a datagram or ms pass. select() is the wait
 * Winsock has; a POSIX descriptor at or above FD_SETSIZE would overflow its
 * fd_set, so poll() there. */
static void sock_wait(sock_t s, int ms) {
#if defined(_WIN32)
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    struct timeval tv = {0, ms * 1000};
    select(0, &r, NULL, NULL, &tv);
#else
    struct pollfd p = {s, POLLIN, 0};
    poll(&p, 1, ms);
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
    int r =
        (int)sendto(net.sock, (const char*)buf, len, 0, (struct sockaddr*)&net.peer, net.peer_len);
    if (r < 0) {
        int e = sock_last_err();
        if (sock_would_block(e)) {
            s_tx_would_block++;
        } else if (!sock_transient(e)) {
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
    (void)ud;
    (void)id;
    SDL_LockMutex(net.tx_lock);
    if (!net.active) {
        s_timer = 0;
        SDL_UnlockMutex(net.tx_lock);
        return 0;
    }
    tx_flush();
    rel_service();
    uint64_t now = SDL_GetTicksNS();
    /* Mid-frame resend pacing:
     * On clean links (low loss, low jitter, low rollback depth), disable redundant
     * mid-frame retransmissions to prevent bufferbloat and network congestion.
     * Re-enable immediately when packet loss, jitter spikes, or rollbacks occur.
     * If the game thread is stalled in an asset load or DVD read (now - s_last_send_ns >= 16 ms),
     * keep sending keepalives every 7 ms so neither peer times out. */
    bool game_thread_quiet = (now - s_last_send_ns >= 16000000ull);
    bool need_resend = (s_loss_pct >= 2 || jitter_us() >= 4000 || s_rb_depth_recent >= 2);
    if (s_last_valid && (now - s_last_send_ns >= 7000000ull) && (game_thread_quiet || need_resend))
    {
        send_packet(&s_last_pkt);
        s_last_send_ns = now;
    }
    int32_t seen = net.frame;
    SDL_UnlockMutex(net.tx_lock);
    /* Outside the lock: the log write must not hold up the sender, and the
     * stuck thread it signals may itself be waiting on this mutex. */
    net_watchdog_tick(seen);
    return interval;
}

static void send_inputs(void) {
    /* The local sample taken at frame f is the input for frame f + delay. */
    int32_t newest = s_wrote;
    if (newest < 0) {
        return; /* do not send input packets until the first local frame exists */
    }
    Packet pk;
    memset(&pk, 0, sizeof pk);
    pk.h = hdr('M');
    /* Everything the peer has not acked yet, oldest first so its contiguous
     * mark can always advance; the cap only bites when acks are starved.
     *
     * Computed in 64 bits. The values are all frame numbers, and while a
     * session is bounded well below the point where a frame number could
     * overflow (see SESSION_MAX_FRAMES), `s_last_acked + 1` and
     * `newest - first + 1` are the expressions that would do it first, and
     * signed overflow is undefined rather than merely wrong. Widening costs
     * nothing here and removes the class. */
    int64_t newest64 = newest;
    int64_t first = (int64_t)s_last_acked + 1;
    if (first < 0) {
        first = 0;
    }
    if (first <= newest64 - RING) {
        first = newest64 - RING + 1;
    }
    /* Repeat only as many unacked frames as loss and rollback depth warrant,
     * clamped to REDUNDANCY: a clean link ships the floor, a lossy or bursty
     * one widens toward the full window. A reconnect refills a gap of up to
     * a whole ring, and every packet of it costs a round trip, so it raises
     * the floor to the top of the window for the duration. */
    int target = s_red_floor + 2 * s_loss_pct + s_rb_depth_recent;
    if (target < s_red_floor) {
        target = s_red_floor;
    }
    if (target > REDUNDANCY) {
        target = REDUNDANCY;
    }
    s_red_target = target;
    int64_t count = newest64 - first + 1;
    if (count > target) {
        count = target;
    }
    if (count < 1) {
        count = 0;
        first = newest64;
    }
    for (int32_t i = 0; i < count; i++) {
        pk.pads[i] = s_local_ring[(first + i) & (RING - 1)];
    }
    pk.count = (uint8_t)count;
    pk.first = (int32_t)first;
    pk.newest = newest;
    pk.ck_frame = confirmed_frame();
    pk.ck = pk.ck_frame >= 0 ? s_ck_ring[pk.ck_frame & (RING - 1)] : 0;
    /* How far this simulation has run past the newest input the peer has
     * given us. The peer compares it with its own and only the side that is
     * further ahead gives a frame back (net_sync.c time_sync). */
    int32_t adv = net.frame - 1 - s_remote_have;
    pk.adv = (int8_t)(adv < -127 ? -127 : adv > 127 ? 127 : adv);
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
    Ack a = {hdr('A'), seq, frame};
    wire_ack(&a);
    SDL_LockMutex(net.tx_lock);
    tx(&a, sizeof a);
    SDL_UnlockMutex(net.tx_lock);
}

static void send_bye(uint8_t reason) {
    Bye b = {hdr('B'), reason};
    tx(&b, sizeof b);
}

/* ---- receive -----------------------------------------------------------
 * Reception runs on a thread of its own (rx_main below), not on the game
 * thread. On the game thread a datagram waited in the socket until the frame
 * loop next drained it, so every freeze of that thread -- a load, a shader
 * compile, a phone descheduling it -- was measured as network latency by
 * both peers: the frozen side acked late, and timed the acks it received
 * late. Phone<->PC on one Wi-Fi network (~20 ms ping) logged ping max
 * 150-450 ms per window, matching the phone's frame freezes exactly, and
 * auto delay and the jitter mean (net_sync.c) consume that number. The
 * frozen side also stopped acking, so its peer counted its own packets as
 * lost and widened their redundancy.
 *
 * So the receive thread does the part that is about the link the moment a
 * datagram lands: drain the socket, authenticate, ack every input packet
 * and time every ack of ours. What is about the game goes through a queue
 * (rx_push) that recv_inputs applies on the game thread: the input rings,
 * s_remote_have, frame advantage, s_last_acked, the RTT statistics, the
 * reliable lanes and through them the handshake, resume, delay and scene
 * hand-off. A BYE or a protocol mismatch is a sticky flag instead, so a full
 * queue cannot lose it.
 *
 * Measured with tools/net_test.py --delay 10 --load-stall 0.3 (20 ms round
 * trip, one peer's game thread parked 300 ms): the other peer's ping max for
 * that window went from 339 ms to 27 ms, jitter from 7.1 to 0.9 ms. Its
 * steady ping went from 40-47 ms to 23-24 ms too, because the old figure also
 * carried most of a frame of drain delay on each end.
 *
 * The ack carries s_rx_have, the receive thread's contiguous mark over what
 * it has queued. on_inputs applies the same packets in the same order under
 * the same rule, so the next drain brings s_remote_have to that mark or past
 * it: an ack never claims a frame the game thread will not get, and the peer
 * drops a frame from its redundancy window (which starts at s_last_acked + 1)
 * only once it is queued here. A packet the full queue refuses is still
 * acked, with the unchanged mark, so the peer's RTT and loss stay honest.
 *
 * A thread rather than tx_timer: the timer polls every 4 ms, which would
 * put up to 4 ms of quantisation back into every sample on each side (the
 * error this removes, only smaller), and SDL runs every timer on one shared
 * thread. A thread parked in poll() wakes when the datagram lands. */

/* One datagram's worth of game-thread work, host order. */
typedef struct RxMsg {
    int len;    /* message bytes */
    int rtt_us; /* 'A': round trip timed as it landed, -1 for none */
    union {
        Hdr h;
        Packet pk;
        Ack ack;
        Rel rel;
        RelAck rack;
    } u;
} RxMsg;
/* While this side is frozen the peer keeps sending: its input packet every
 * 16 ms from wait_remote(), every 8 ms once its tx_timer resends as well,
 * and an ack for each resend our own tx_timer makes (16 ms apart, 8 when the
 * link wants resends) -- 120 to 250 datagrams a second. 256 is a second of
 * that, past every freeze the phone sessions logged (<= 450 ms); a 2 s
 * --load-stall on loopback dropped none (rx_full 0). A longer freeze
 * drops the surplus and counts it (rx_full): input packets and reliable
 * messages are resent, and a later ack supersedes a lost one. */
#define RXQ 256
static RxMsg s_rxq[RXQ]; /* under s_rx_lock */
static int s_rxq_head, s_rxq_n;

/* Queue a message for the game thread (receive thread, s_rx_lock held). */
static bool rx_push(const void* body, int len, int rtt_us) {
    if (s_rxq_n == RXQ) {
        s_rx_full++;
        return false;
    }
    RxMsg* m = &s_rxq[(s_rxq_head + s_rxq_n++) % RXQ];
    m->len = len;
    m->rtt_us = rtt_us;
    memcpy(&m->u, body, (size_t)len);
    return true;
}

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
    int16_t d = (int16_t)(seq - s_rx_seq_top);
    if (d > 0) {
        s_rx_seq_bits = d >= 64 ? 0 : s_rx_seq_bits << d;
        s_rx_seq_bits |= 1;
        s_rx_seq_top = seq;
        return SEQ_NEW;
    }
    uint32_t back = (uint32_t)(-d);
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

/* An input packet off the wire (receive thread, s_rx_lock held): checked,
 * queued for on_inputs and acked. The ack goes out here rather than after
 * the game thread applies the packet because its seq is what the peer times
 * its round trip by; its frame is s_rx_have, final once the packet is queued
 * (see the top of this section). */
static void rx_input(const Packet* pk, int n) {
    int32_t frame = atomic_load(&s_frame_pub);
    int count = pk->count > REDUNDANCY ? REDUNDANCY : pk->count;
    /* A peer can be at most window + delays ahead of us; anything else is
     * garbage (a corrupt packet, or a stale process at the peer's address).
     * All arithmetic is in int64 so first/count/newest at INT32 extremes
     * cannot overflow the comparisons. */
    int64_t first = pk->first, newest = pk->newest, ckf = pk->ck_frame;
    int64_t last = first + count - 1;

    /* A keepalive packet sent before local frame 0 exists has count 0 and newest -1. */
    bool is_keepalive = (count == 0 && newest == -1 && first == 0 && ckf == -1);

    if (!is_keepalive && (n < (int)(offsetof(Packet, pads) + (size_t)count * sizeof(WirePad)) ||
                             first < 0 || newest < first || last > newest ||
                             newest > (int64_t)frame + RING / 2 || ckf < -1 || ckf > newest))
    {
        s_rx_malformed++;
        if (!s_warn_bad) {
            s_warn_bad = true;
            pc_log_line("net: dropped malformed input packet (len %d first %d count %d newest %d "
                        "ck_frame %d at frame %d)",
                n, pk->first, count, pk->newest, pk->ck_frame, frame);
        }
        return;
    }
    if (is_keepalive) {
        return;
    }
    /* Only contiguous data is taken; the ack below tells the peer what to
     * resend, so a gap never has to be tracked. */
    int64_t have = s_rx_have;
    if (count > 0 && first <= have + 1 && last > have && last < (int64_t)frame + RING / 2) {
        have = last;
    }
    if (rx_push(pk, n, -1)) {
        s_rx_have = (int32_t)have;
    }
    send_ack(s_rx_have, pk->seq);
}

/* The game thread's half of a packet rx_input queued. rx_input's rule over
 * the same packets in the same order, against a net.frame never behind the
 * one it checked, so a drain leaves s_remote_have at or above s_rx_have. */
static void on_inputs(const Packet* pk) {
    int count = pk->count > REDUNDANCY ? REDUNDANCY : pk->count;
    int64_t last = (int64_t)pk->first + count - 1;
    if (count > 0 && pk->first <= s_remote_have + 1 && last > s_remote_have &&
        last < (int64_t)net.frame + RING / 2)
    {
        int32_t upto = simulated_upto();
        for (int32_t f = s_remote_have + 1; f <= last; f++) {
            WirePad pad = pk->pads[f - pk->first];
            WirePad* slot = &s_remote_ring[f & (RING - 1)];
            /* A frame already simulated holds the prediction it ran on. */
            if (f <= upto && memcmp(slot, &pad, sizeof pad) != 0 &&
                (s_rb_frame < 0 || f < s_rb_frame))
            {
                s_rb_frame = f;
            }
            *slot = pad;
        }
        s_remote_have = last;
        s_progress_ns = SDL_GetTicksNS();
    }
    if (pk->ck_frame > s_remote_ck_frame) {
        s_remote_ck_frame = pk->ck_frame;
        s_remote_ck = pk->ck;
    }
    /* The peer's frame advantage against ours. Both are measured the same
     * way against data that actually arrived, so their DIFFERENCE is the
     * phase error with nothing estimated in it -- no shared clock, no round
     * trip, and no assumption that the two directions are equally fast.
     *
     * The estimator this replaces asked "what time was it when the peer
     * sent this", which needs the one-way trip, and the only handles on that
     * are round trips measured through a socket this thread drains once per
     * frame. That quantisation inflates every sample by most of a frame on
     * BOTH peers, so the error is common-mode: it survives the trimmed mean
     * and moves both peers the same way. Measured here, an averaged trip put
     * both peers 7 ms behind each other (nobody corrects, and the pair keeps
     * whatever phase it started with -- 6 rollbacks on one side against 724
     * on the other), and a minimum-filtered trip put both 16 ms ahead of
     * each other (both slow down, and the pair runs at 59.4 Hz). */
    adv_note(pk->adv, net.frame - 1 - s_remote_have);
    if (pk->newest > s_remote_newest) {
        s_remote_newest = pk->newest;
    }
}

/* The round trip of our input packet `seq`, timed as its ack lands (receive
 * thread); -1 unless the ack echoes a seq we actually sent and have not
 * sampled yet. Consumed, so a delayed duplicate ack cannot skew the
 * estimate. The ring is written by both senders under tx_lock. */
static int rtt_sample(uint16_t seq) {
    int rtt = -1;
    SDL_LockMutex(net.tx_lock);
    int slot = seq & 63;
    if (s_rtt_ring[slot].seq == seq && s_rtt_ring[slot].send_ns != 0) {
        uint64_t now = SDL_GetTicksNS();
        if (now > s_rtt_ring[slot].send_ns && now - s_rtt_ring[slot].send_ns < 5000000000ull) {
            rtt = (int)((now - s_rtt_ring[slot].send_ns) / 1000);
        }
        s_rtt_ring[slot].send_ns = 0;
    }
    SDL_UnlockMutex(net.tx_lock);
    return rtt;
}

/* The game thread's half of an ack: rtt is rtt_sample()'s. */
static void on_ack(const Ack* a, int rtt) {
    /* The peer cannot hold a frame we never sent: a bogus ack above s_wrote
     * would leave send_inputs() with first > newest, i.e. shipping no pads at
     * all for the rest of the session (and INT32_MAX overflows first). */
    if (a->frame > s_last_acked && a->frame <= s_wrote) {
        s_last_acked = a->frame;
    }
    if (rtt >= 0) {
        net.ping_us = net.ping_us == 0 ? (uint32_t)rtt : (uint32_t)((net.ping_us * 3 + rtt) / 4);
        s_ping_sum += (uint64_t)rtt;
        s_ping_n++;
        if (s_rtt_min == 0 || (uint32_t)rtt < s_rtt_min) {
            s_rtt_min = (uint32_t)rtt;
        }
        if ((uint32_t)rtt > s_rtt_max) {
            s_rtt_max = (uint32_t)rtt;
        }
        jitter_note((uint32_t)rtt);
    }
}

/* One datagram from the peer, header already in host order and checked
 * (receive thread, s_rx_lock held). */
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
        if (n < (int)offsetof(Packet, pads)) {
            return;
        }
        wire_packet(&u->pk);
        switch (seq_check(u->pk.seq)) {
        case SEQ_DUP:
            /* An exact copy means the peer has not seen an ack for it. The
             * original was processed, but the ack itself may be what went
             * missing, and dropping the duplicate silently leaves the peer
             * resending until the redundancy in a later packet happens to
             * cover the gap. Re-ack it: the ack carries our contiguous mark,
             * so it is correct whenever it arrives and costs one datagram. */
            s_rx_dups++;
            send_ack(s_rx_have, u->pk.seq);
            break;
        case SEQ_REORDER:
            s_rx_reorders++; /* older but new: still process, its pads may fill a gap */
            rx_input(&u->pk, n);
            break;
        default:
            rx_input(&u->pk, n);
            break;
        }
        break;
    case 'A':
        if (n != (int)sizeof(Ack)) {
            return;
        }
        wire_ack(&u->ack);
        s_rx_acks++;
        rx_push(&u->ack, n, rtt_sample(u->ack.seq));
        break;
    case 'R':
        if (n < (int)offsetof(Rel, payload)) {
            return;
        }
        wire_rel(&u->rel);
        rx_push(&u->rel, n, -1);
        break;
    case 'K':
        if (n != (int)sizeof(RelAck)) {
            return;
        }
        rx_push(&u->rack, n, -1);
        break;
    case 'B':
        /* Only a peer we have already heard from can end the session. Before
         * pinning, the source address is not a usable filter (a dual-stack
         * peer answers from either family), so without this any host that
         * knows the published port could kill a connecting session with one
         * forged 8-byte datagram. */
        if (n != (int)sizeof(Bye) || s_rx_left || !s_heard) {
            return;
        }
        s_rx_left = true;
        s_rx_why = u->bye.reason <= PC_NET_PEER_RESUME ? u->bye.reason : PC_NET_PEER_LEFT;
        pc_log_line("net: peer left (reason %d) at frame %d", s_rx_why, atomic_load(&s_frame_pub));
        break;
    default:
        return;
    }
    s_heard = true;
    s_last_rx_ns = SDL_GetTicksNS();
}

/* Reject unknown/truncated datagrams before they can select a peer or fail
 * compatibility. Body fields remain in wire order here. */
static bool packet_shape(const void* buf, int n) {
    const Hdr* h = buf;
    switch (h->magic) {
    case 'M': {
        const Packet* p = buf;
        return n >= (int)offsetof(Packet, pads) && p->count <= REDUNDANCY &&
               n == (int)(offsetof(Packet, pads) + p->count * sizeof(WirePad));
    }
    case 'A':
        return n == (int)sizeof(Ack);
    case 'R': {
        const Rel* r = buf;
        return n >= (int)offsetof(Rel, payload) && ntohs(r->len) <= REL_MAX &&
               n == (int)(offsetof(Rel, payload) + ntohs(r->len));
    }
    case 'K':
        return n == (int)sizeof(RelAck);
    case 'B':
        return n == (int)sizeof(Bye);
    default:
        return false;
    }
}

/* Hardware output is deliberately outside game snapshots. Compare against
 * the last command actually emitted, not the interpreter's rewound status. */
void pc_net_rumble_command(unsigned port, unsigned command) {
    static int motor[4] = {-1, -1, -1, -1};
    if (port >= 4 || net.resim || motor[port] == (int)command)
        return;
    PADControlMotor(port, command);
    motor[port] = (int)command;
}

static PcNetDatagramHandler s_datagram_handler;
/* The handler runs on the receive thread, inside s_rx_lock; taking the lock
 * here means that once this returns no call into the old one is running. */
void pc_net_set_datagram_handler(PcNetDatagramHandler handler) {
    SDL_LockMutex(s_rx_lock);
    s_datagram_handler = handler;
    SDL_UnlockMutex(s_rx_lock);
}

bool pc_net_send_datagram(const void* data, size_t size, uint32_t address, uint16_t port) {
    if (!net.tx_lock || !data || size > 512)
        return false;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = address;
    to.sin_port = htons(port);
    SDL_LockMutex(net.tx_lock);
    int n = net.active ? (int)sendto(net.sock, (const char*)data, (int)size, 0,
                             (struct sockaddr*)&to, sizeof to) :
                         -1;
    SDL_UnlockMutex(net.tx_lock);
    return n == (int)size;
}

/* One call's worth of datagrams (rx_pump) or of queued messages
 * (recv_inputs). The peer sends an input packet per frame, a mid-frame
 * resend, an ack per packet received and the odd reliable message: a couple
 * of dozen per frame at the very worst. Without a cap a drain is a livelock
 * -- anything that fills it faster than the loop empties it parks the
 * draining thread there for as long as it keeps arriving, and on the game
 * thread that is the frame loop and wait_remote(). The remainder stays
 * queued for the next call. */
#define RX_BUDGET 128

/* One datagram off the socket (receive thread, s_rx_lock held). It is
 * dropped, with one log line per session and reason, unless it comes from
 * the peer's address, carries our protocol version and session id (the
 * guest learns the id from the host's first packet) and names the remote
 * player. */
static void rx_datagram(void* buf, int n, const struct sockaddr_storage* from, socklen_t from_len) {
    Hdr* h = buf;
    if (s_datagram_handler && from->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)from;
        if (s_datagram_handler(buf, (size_t)n, a->sin_addr.s_addr, ntohs(a->sin_port)))
            return;
    }
    s_rx_pkts++;
    if (n < (int)(sizeof(Hdr) + NET_MAC_LEN)) {
        s_rx_malformed++;
        return;
    }
    /* Everything downstream measures the message, not the datagram: the
     * tag is checked here and then left behind, so every length test and
     * every body parser below stays the one it was. */
    int body = n - NET_MAC_LEN;
    if (!packet_shape(buf, body)) {
        s_rx_malformed++;
        return;
    }
    /* The authentication gate, ahead of every other check: a datagram
     * that cannot prove it belongs to this session must not pin the peer
     * address, teach us the session id, fail our protocol version or
     * reach rx_dispatch.
     *
     * A handshake-derived key does not exist until the handshake, and
     * the two peers get it one leg apart -- the guest when it accepts
     * RULES, the host when it accepts the READY that answers it. Until a
     * datagram has actually verified, one that does not is taken as a
     * peer that has not keyed yet rather than as an attack; from the
     * first tag that checks out, nothing unauthenticated is accepted
     * again for the rest of the session. That upgrade is one round trip
     * wide and it is never given back, so the window is the bootstrap
     * one that already existed rather than a downgrade an attacker can
     * reopen.
     *
     * A key pinned from MELEE_NET_KEY gets no such grace: both peers
     * hold it from connect, so there is no leg to wait out and the first
     * untagged datagram is already a forgery. Without this the knob
     * would authenticate nothing at all -- an attacker who never sends a
     * valid tag would simply keep the grace open forever.
     *
     * The key is read under tx_lock: the handshake installs it on the
     * game thread while this runs on the receive thread. */
    SDL_LockMutex(net.tx_lock);
    bool keyed = net_key_ready();
    bool mac_ok = keyed && net_mac_ok(buf, (size_t)body);
    bool pinned = net_key_pinned();
    SDL_UnlockMutex(net.tx_lock);
    if (mac_ok) {
        s_mac_seen = true;
    } else if (s_mac_seen || pinned) {
        s_rx_bad_mac++;
        if (!s_warn_mac) {
            s_warn_mac = true;
            pc_log_line("net: dropped a datagram with a bad MAC (magic %c len %d) at frame %d",
                h->magic >= 32 && h->magic < 127 ? h->magic : '?', n, atomic_load(&s_frame_pub));
        }
        return;
    } else if (keyed && ++s_mac_quiet == MAC_QUIET_MAX) {
        /* We hold a key and two seconds of the peer's traffic has gone by
         * without one tag of it verifying. That is not the handshake leg
         * the upgrade above allows for; it is two peers holding different
         * keys -- a stale MELEE_NET_KEY on one side is how -- and the
         * session is running unauthenticated. Said once, out loud,
         * because the alternative is a session that looks protected in
         * the log and is not. */
        pc_log_line("net: %d datagrams from the peer and none of them authenticate; this "
                    "session is UNAUTHENTICATED (the two sides hold different keys)",
            MAC_QUIET_MAX);
    }
    wire_hdr(h);
    bool same_addr = addr_eq(from, &net.peer);
    /* Once pinned, unrelated traffic cannot change any session state.
     *
     * Before pinning, the address is NOT a usable filter: the two sides
     * pick their own for each other, and a dual-stack LAN peer routinely
     * answers from a different family than the one the lobby handed us
     * (measured PC<->tablet: we dialled its IPv6 link-local, it answered
     * from 192.168.1.109). Requiring a match there breaks the handshake
     * outright. What IS gated on the address is the one datagram that
     * needs no session state to do damage -- see the BYE in rx_dispatch:
     * before pinning, a forged one from anywhere would otherwise end the
     * session on its own. */
    if (s_heard && !same_addr) {
        s_rx_bad_src++;
        if (!s_warn_src) {
            char got[80] = "?", want[80] = "?";
            net_addr_text((const struct sockaddr*)from, got, sizeof got);
            net_addr_text((const struct sockaddr*)&net.peer, want, sizeof want);
            s_warn_src = true;
            pc_log_line("net: dropped a datagram from %s (the peer is %s)", got, want);
        }
        return;
    }
    if (h->player != net.remote) {
        s_rx_bad_player++;
        return;
    }
    /* Learning the session id is how the guest finds the host without the
     * lobby having told it one -- and it is also the one piece of session
     * state a stranger could set with a single well-shaped datagram,
     * because the MAC gate above is a grace window while no key exists.
     * A 16-byte RelAck carrying someone else's session id used to win that
     * race, after which every genuine host datagram failed the session
     * check for the rest of the session (learn_session requires
     * net.session == 0, so there is no second chance) and the match died
     * on the handshake timeout. Only the message that carries the session
     * in the first place -- a well-formed RULES -- may establish it.
     * "Well-formed" here means the Rel frame declares a full Rules
     * payload: the genuine host always sends sizeof(Rules) (on_rules
     * drops anything else), so a one-packet forgery has to shape a
     * payload that also survives the guest's validation, and the
     * session is still only learned -- no address pin, no key. */
    bool is_rules;
    {
        /* packet_shape has already proved this is a 'R' whose declared
         * length matches the datagram, so the Rel view is in bounds and
         * r->len can be trusted here. */
        const Rel* rel = buf;
        is_rules = h->magic == 'R' && rel->type == REL_RULES && ntohs(rel->len) == sizeof(Rules);
    }
    bool learn_session = net.session == 0 && net.local == 1 && h->session != 0 && is_rules;
    bool guest_preamble = !s_heard && net.local == 0 && h->session == 0;
    if (h->session != net.session && !learn_session && !guest_preamble) {
        s_rx_bad_sess++;
        if (!s_warn_sess) {
            s_warn_sess = true;
            pc_log_line("net: dropped a datagram for session %08x player %u (ours %08x, peer %d)",
                h->session, h->player, net.session, net.remote);
        }
        return;
    }
    if (h->version != WIRE_VERSION) {
        if (same_addr && !s_rx_left) {
            s_rx_left = true;
            s_rx_why = PC_NET_PEER_INCOMPATIBLE;
            pc_log_line("net: peer speaks protocol %u, we speak %u", h->version, WIRE_VERSION);
        }
        return;
    }
    if (learn_session) {
        SDL_LockMutex(net.tx_lock);
        net.session = h->session;
        SDL_UnlockMutex(net.tx_lock);
    }
    if (!s_heard && !same_addr) {
        char got[80] = "?", want[80] = "?";
        net_addr_text((const struct sockaddr*)from, got, sizeof got);
        net_addr_text((const struct sockaddr*)&net.peer, want, sizeof want);
        pc_log_line("net: peer answers from %s, not %s; following it", got, want);
        SDL_LockMutex(net.tx_lock);
        memcpy(&net.peer, from, sizeof net.peer);
        net.peer_len = from_len;
        SDL_UnlockMutex(net.tx_lock);
    }
    if (net.sim_rx_delay_ns == 0) {
        rx_dispatch(buf, body);
    } else {
        /* A full simulator queue is one more loss. */
        held_put(s_rx_held, buf, (size_t)body, SDL_GetTicksNS() + net.sim_rx_delay_ns);
    }
}

/* Drain the socket through the gate, then release what the link simulator
 * held back. Runs on the receive thread, or on the game thread when there
 * is none. False on a hard socket error (sock_err_note logged it). */
static bool rx_pump(void) {
    for (int budget = RX_BUDGET; budget > 0; budget--) {
        union {
            Hdr h;
            uint8_t raw[512];
        } u;
        struct sockaddr_storage from;
        socklen_t from_len = sizeof from;
        int n = (int)recvfrom(net.sock, (char*)&u, sizeof u, 0, (struct sockaddr*)&from, &from_len);
        if (n < 0) {
            int e = sock_last_err();
            if (sock_would_block(e)) {
                break; /* nothing more queued */
            }
            if (sock_transient(e)) {
                continue; /* EINTR / ICMP unreachable: more may still be queued */
            }
            sock_err_note("recvfrom", e);
            return false;
        }
        if (n == 0) {
            continue; /* empty datagram: nothing to parse */
        }
        SDL_LockMutex(s_rx_lock);
        rx_datagram(&u, n, &from, from_len);
        SDL_UnlockMutex(s_rx_lock);
    }
    if (net.sim_rx_delay_ns != 0) {
        SDL_LockMutex(s_rx_lock);
        uint64_t now = SDL_GetTicksNS();
        for (Held* h; (h = held_due(s_rx_held, now)) != NULL;) {
            h->release_ns = 0;
            rx_dispatch(h->buf, h->len);
        }
        SDL_UnlockMutex(s_rx_lock);
    }
    return true;
}

/* The longest the receive thread waits between looks at s_rx_run: all it
 * bounds is how long pc_net_disconnect() waits for the thread to notice. */
#define RX_WAIT_MS 5

static int SDLCALL rx_main(void* ud) {
    (void)ud;
    /* The simulator's receive hold is released from here too, and a 1 ms
     * turn keeps its delay within a millisecond of the knob. */
    int ms = net.sim_rx_delay_ns != 0 ? 1 : RX_WAIT_MS;
    while (atomic_load(&s_rx_run)) {
        sock_wait(net.sock, ms);
        if (!rx_pump()) {
            SDL_DelayNS(RX_WAIT_MS * 1000000ull); /* a hard error is logged, not spun on */
        }
    }
    return 0;
}

/* When the peer was last heard from, 0 before its first datagram: the
 * receive thread's clock, read under its lock (game thread). */
static uint64_t rx_heard_ns(void) {
    SDL_LockMutex(s_rx_lock);
    uint64_t t = s_heard ? s_last_rx_ns : 0;
    SDL_UnlockMutex(s_rx_lock);
    return t;
}

/* Apply what the receive thread queued, in arrival order (game thread): the
 * frame loop calls this once per fresh tick and wait_remote() once a turn. */
void recv_inputs(void) {
    atomic_store(&s_frame_pub, net.frame);
    if (s_rx_thread == NULL) {
        rx_pump();
    }
    for (int budget = RX_BUDGET; budget > 0; budget--) {
        RxMsg m;
        SDL_LockMutex(s_rx_lock);
        bool got = s_rxq_n > 0;
        if (got) {
            m = s_rxq[s_rxq_head];
            s_rxq_head = (s_rxq_head + 1) % RXQ;
            s_rxq_n--;
        }
        SDL_UnlockMutex(s_rx_lock);
        if (!got) {
            break;
        }
        switch (m.u.h.magic) {
        case 'M':
            on_inputs(&m.u.pk);
            break;
        case 'A':
            on_ack(&m.u.ack, m.rtt_us);
            break;
        case 'R':
            on_rel(&m.u.rel, m.len);
            break;
        default: /* 'K' */
            on_rel_ack(&m.u.rack);
            break;
        }
    }
    SDL_LockMutex(s_rx_lock);
    bool left = s_rx_left;
    int why = s_rx_why;
    SDL_UnlockMutex(s_rx_lock);
    if (left && !s_peer_left) {
        s_peer_left = true;
        s_status = why;
    }
}

/* Frames the last 16 rollbacks restored to, newest last: a desync with an
 * uncorrected misprediction in it has no rollback to the frame that went
 * wrong, and that is otherwise invisible. */
static int32_t s_rb_recent[16];
static unsigned s_rb_recent_n;

/* On a desync: what the input rings and the checksum ring finally held
 * around the frame, after every rollback. The two peers' logs together are
 * the only thing that separates "we simulated different inputs" (a
 * misprediction nobody corrected, a delay mismatch, a lost packet taken as
 * contiguous) from "the same inputs built different state" (an impure
 * tick): the state dump beside it is the first pass only. */
static uint8_t s_sim_n[RING]; /* times each frame index was simulated */

static void dump_rings_around(int32_t f) {
    for (int32_t i = f - 16; i <= f + 1; i++) {
        if (i < 0 || i > net.frame || i <= net.frame - RING) {
            continue;
        }
        const WirePad* m = &s_local_ring[i & (RING - 1)];
        const WirePad* t = &s_remote_ring[i & (RING - 1)];
        pc_log_line("net: ring f%d p%d %04x/%d,%d p%d %04x/%d,%d ck %08x x%u%s", i, net.local + 1,
            m->button, m->stickX, m->stickY, net.remote + 1, t->button, t->stickX, t->stickY,
            s_ck_ring[i & (RING - 1)], s_sim_n[i & (RING - 1)],
            i > s_remote_have ? " predicted" : "");
    }
    /* Sized for the worst case (16 frames of up to 10 digits, SESSION_MAX_FRAMES
     * is 1e8) and still guarded: snprintf returns the length it WOULD have
     * written, so accumulating its return value past the end makes the next
     * size argument wrap and the next write land outside the buffer. */
    char rb[16 * 11 + 1];
    int n = 0;
    for (unsigned i = s_rb_recent_n > 16 ? s_rb_recent_n - 16 : 0; i < s_rb_recent_n; i++) {
        int w = snprintf(rb + n, sizeof rb - (size_t)n, "%d ", s_rb_recent[i % 16]);
        if (w < 0 || w >= (int)(sizeof rb - (size_t)n)) {
            n = (int)sizeof rb - 1;
            break;
        }
        n += w;
    }
    rb[n > 0 ? n - 1 : 0] = '\0';
    pc_log_line("net: ring have %d newest %d wrote %d delay %d barrier %d rb_frame %d resim %d, "
                "last rollbacks %s",
        s_remote_have, s_remote_newest, s_wrote, net.delay, net.rb_barrier, s_rb_frame, net.resim,
        rb);
}

static void check_desync(void) {
    if (net.desync_reported || net.hs == HS_PENDING || s_remote_ck_frame < net.ck_from ||
        s_remote_ck_frame > confirmed_frame() || s_remote_ck_frame <= net.frame - RING)
    {
        return;
    }
    uint32_t mine = s_ck_ring[s_remote_ck_frame & (RING - 1)];
    if (mine != s_remote_ck) {
        net.desync_reported = true;
        pc_log_line("net: DESYNC at frame %d (local %08x remote %08x)", s_remote_ck_frame, mine,
            s_remote_ck);
        dump_rings_around(s_remote_ck_frame);
        dump_states_around(s_remote_ck_frame);
    }
}

/* ---- resume after an interruption --------------------------------------
 * A silence longer than STALL_TIMEOUT_MS used to end the session outright,
 * so a two-second Wi-Fi hiccup killed a match both peers could have
 * finished: the game thread is merely parked in wait_remote(), every byte
 * of game state is intact and both input rings still hold their last RING
 * frames. The stall timeout now opens a reconnect phase instead. The
 * socket, the 4 ms transmit timer and the 16 ms resends of the wait loop
 * stay exactly as they are, so the peer keeps being probed; on top of that
 * each side that opens a phase states what it holds in exactly one reliable
 * REL_RESUME (a statement, never answered: see net_resume_rel). Both sides
 * refill the gap through the ordinary input path (send_inputs' unacked
 * window, widened to REDUNDANCY while the phase is open) and the wait ends
 * at the frame it was waiting for, the frames that were predicted before
 * the interruption being rolled back as usual.
 *
 * The rings bound what can be resumed and both sides' rings are frozen
 * during the phase: whoever is not parked can only run WINDOW + delay
 * frames past the other before it parks too, so one check at exchange time
 * cannot go stale.
 *
 * None of this runs in the common case and the frame loop grows no branch
 * for it: the phase is entered from the stall path, the refill rides on the
 * redundancy floor send_inputs already loads, and a refused exchange is
 * noticed by the very next wait (which is at most WINDOW frames away, since
 * a parked peer stops producing input).
 *
 * MELEE_NET_RECONNECT_MS bounds the phase. The default is RECONNECT_MS: a
 * Wi-Fi roam or a hotspot handover completes well inside it, and the whole
 * outage (STALL_TIMEOUT_MS to notice plus this window) stays near 6 s, which
 * is about as long as a player waits before quitting anyway. Both were 7 s
 * and 15 s until faf888914 shortened them, trading a longer outage tolerance
 * for much faster "the peer is gone" detection; 0 disables resume, i.e. the
 * hard disconnect this file did before. */

/* Resume payload in wire order (big-endian, like every wire field). The
 * codec's be32() is private to net_wire.c and every field here is 32 bits,
 * so the image swaps as one array; that is its own inverse, so this serves
 * both send and receive. */
static void wire_resume(Resume* r) {
    uint8_t* p = (uint8_t*)r;
    for (size_t i = 0; i < sizeof *r; i += 4) {
        uint8_t b0 = p[i], b1 = p[i + 1];
        p[i] = p[i + 3];
        p[i + 1] = p[i + 2];
        p[i + 2] = b1;
        p[i + 3] = b0;
    }
}

/* What we hold, so the peer can decide whether the gap is coverable. Queued
 * once per phase: the reliable channel resends it every 250 ms until its ack
 * arrives, so a lossy link needs no help here. */
static void resume_send(void) {
    Resume r = {net.session, net.seed, s_wrote, s_remote_have, net.frame};
    wire_resume(&r);
    s_rc_sent = pc_net_send_reliable(REL_RESUME, &r, sizeof r);
}

/* Refuse to resume: end the session with a status of its own rather than
 * run on with a gap neither ring can fill. The BYE pc_net_disconnect()
 * sends carries the same status, so the other side reports it too. */
static void resume_fail(void) {
    s_rc = RSM_FAILED;
    s_status = PC_NET_PEER_RESUME;
}

/* Only an established session can be resumed, and waiting on one that is not
 * actively harms the caller: the lobby reports a failure from pc_lan_poll(),
 * which runs on the very thread parked in wait_remote(), so holding the
 * phase open through a handshake that will never finish turns a 7 s "connect
 * failed" into a 22 s hang (tools/net_lan_test.py host_dies, where the host
 * is killed while the guest is still connecting at frame 3). Before the
 * handshake there is also nothing to resume: no agreed seed, no agreed start
 * frame, and rings holding a couple of menu frames.
 *
 * HS_DONE is "established" everywhere now (net_handshake.c's hs_done() pins
 * seed, start_frame and ck_from); HS_PENDING, HS_FAILED and HS_IDLE all mean
 * the parameters are still unagreed, whether or not the guest has adopted
 * the host's session id. This used to need a grace period, because a direct
 * MELEE_NET session ran no handshake and stayed HS_IDLE for its whole life,
 * so idleness could not be read as "not yet"; it runs the same handshake as
 * the lobby now (handshake_direct), so the state means one thing again. */
static bool session_established(void) {
    return net.hs == HS_DONE;
}

/* The peer's RESUME (net_reliable.c dispatches it here; game thread).
 *
 * One-way by design: a RESUME is a statement, never a request, and this
 * never sends anything. The receiver has both sides' numbers in front of it
 * -- s_wrote - r.have is what the peer needs from us, r.newest -
 * s_remote_have what we need from it -- so one message clears or refuses the
 * whole exchange, and when both peers stall (the usual case) each opens its
 * own phase and states its own numbers anyway. An earlier version answered
 * a RESUME whenever no phase was open on this side; both sides then answered
 * each other's answers for the rest of the match, ~60 logged lines a second
 * inside the frame loop (Main's integration run: 2686 copies of the last
 * line below from a handful of interruptions). */
void net_resume_rel(const void* payload, int len) {
    Resume r;
    if (len != (int)sizeof r) {
        pc_log_line("net: RESUME of %d bytes ignored (expected %d)", len, (int)sizeof r);
        return;
    }
    /* Not established yet: there is nothing to resume, and the seed we would
     * compare below is not the agreed one, so a mismatch here would fail the
     * session for the wrong reason. Our own wait ends it fast anyway. */
    if (!session_established()) {
        pc_log_line("net: RESUME at frame %d ignored, the session is not established (hs %d)",
            net.frame, net.hs);
        return;
    }
    memcpy(&r, payload, sizeof r);
    wire_resume(&r);
    /* A restarted peer or a different match cannot be resumed into: every
     * frame we would refill was simulated from another seed. */
    if (r.session != net.session || r.seed != net.seed) {
        pc_log_line("net: cannot resume, the peer answers for session %08x seed %u (ours %08x "
                    "seed %u)",
            r.session, r.seed, net.session, net.seed);
        resume_fail();
        return;
    }
    /* Each side can only serve frames still in its RING-frame ring. In
     * int64 so a corrupt payload at the int32 extremes cannot overflow the
     * comparison. */
    if ((int64_t)s_wrote - r.have > RING || (int64_t)r.newest - s_remote_have > RING) {
        pc_log_line("net: cannot resume at frame %d, the gap outruns the %d-frame ring (peer "
                    "holds our %d of %d, we hold its %d of %d)",
            net.frame, RING, r.have, s_wrote, s_remote_have, r.newest);
        resume_fail();
        return;
    }
    /* The peer may hold frames whose acks died with the link: starting the
     * refill above them saves a round trip. Same guard as on_ack, it cannot
     * hold a frame we never sent. */
    if (r.have > s_last_acked && r.have <= s_wrote) {
        s_last_acked = r.have;
    }
    if (r.newest > s_remote_newest) {
        s_remote_newest = r.newest;
    }
    pc_log_line("net: peer resumes from frame %d, holds our %d, newest %d (we are at %d, hold "
                "its %d, newest %d)",
        r.frame, r.have, r.newest, net.frame, s_remote_have, s_wrote);
}

/* The stall timeout fired: open the phase instead of dropping the session.
 * False when resume is off or the session was never established, and the
 * caller then times out exactly as it did before this file grew a phase --
 * same line, same status, same 7 s. */
static bool resume_begin(uint64_t now) {
    if (s_rc_window_ms <= 0 || !session_established()) {
        return false;
    }
    s_rc = RSM_ACTIVE;
    s_rc_ns = now;
    s_rc_sent = false;
    s_red_floor = REDUNDANCY; /* every refill packet costs a round trip */
    pc_log_line("net: interrupted at frame %d (peer silent %d ms), reconnecting for up to %d ms",
        net.frame, STALL_TIMEOUT_MS, s_rc_window_ms);
    resume_send();
    return true;
}

/* Once per wait-loop turn while the phase is open. False ends the session:
 * the window expired, or the exchange was refused. */
static bool resume_poll(uint64_t now) {
    if (s_rc == RSM_FAILED) {
        return false;
    }
    if (!s_rc_sent) {
        resume_send(); /* the reliable queue was full when the phase opened */
    }
    if (now - s_rc_ns > (uint64_t)s_rc_window_ms * 1000000ull) {
        pc_log_line("net: resume window of %d ms expired at frame %d", s_rc_window_ms, net.frame);
        s_status = PC_NET_PEER_TIMEOUT; /* the session ends as it always did */
        return false;
    }
    return true;
}

/* The peer's inputs are flowing again. */
static void resume_end(uint64_t now) {
    pc_log_line("net: resumed at frame %d after %.1f s (hold remote %d, its newest %d)", net.frame,
        (now - s_rc_ns) / 1e9, s_remote_have, s_remote_newest);
    s_rc = RSM_NONE;
    s_rc_sent = false;
    s_red_floor = REDUNDANCY_FLOOR;
}

/* Block until the remote input for `need` is here. False when the session is
 * over: the peer announced it is gone, the reconnect window expired or the
 * interruption could not be resumed (s_status says which).
 *
 * An established session that falls silent past the stall timeout enters the
 * reconnect phase above rather than ending; before the peer's first packet
 * there is nothing to resume, so that wait keeps its own timeout.
 *
 * "Silent" is measured from the last datagram the peer sent, NOT from the
 * start of this wait. A peer whose game thread is inside a load -- a stage,
 * a character, a first-time shader compile on a phone -- stops advancing for
 * seconds while tx_timer keeps its input window flowing every 7 ms. Timing
 * that out as silence killed the session on every slow load and froze the
 * transition screen ("NOW LOADING") until the reconnect window expired.
 * A peer that talks but never advances still has to end somewhere, so a
 * no-progress cap bounds the wait; it is minutes, not seconds, because a
 * cold-cache load on a phone is legitimately tens of seconds. */
#define NO_PROGRESS_TIMEOUT_MS 120000
/* The connect wait, with a floor: a session that never went through
 * connect_impl (a fixture, a future caller) must not read as "wait zero". */
static int connect_timeout_ms(void) {
    return net.connect_timeout_ms > 0 ? net.connect_timeout_ms : CONNECT_TIMEOUT_MS;
}
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
        net_watchdog_heartbeat();
        /* Before now, so no stamp the receive thread writes is later than it. */
        uint64_t heard = rx_heard_ns();
        uint64_t now = SDL_GetTicksNS();
        if (s_rc != RSM_NONE) {
            if (!resume_poll(now)) {
                return false;
            }
        } else {
            /* Before the first packet there is no rx clock, so the connect
             * wait is still measured from t0. */
            uint64_t quiet = heard ? now - heard : now - t0;
            uint64_t limit = (heard ? STALL_TIMEOUT_MS : connect_timeout_ms()) * 1000000ull;
            /* Measured from the last forward progress, not from the start of
             * this wait: t0 restarts with every wait_remote() call, so a peer
             * that advanced the contiguous mark one frame just under the bound
             * each time kept the game frozen for ever while looking healthy.
             * Before the first advance there is nothing to measure from, and
             * t0 is the connect wait's own clock anyway. */
            uint64_t since = s_progress_ns ? s_progress_ns : t0;
            bool stuck = now - since > NO_PROGRESS_TIMEOUT_MS * 1000000ull;
            if (quiet > limit || stuck) {
                s_no_progress = stuck && quiet <= limit;
                if (!heard || s_no_progress || !resume_begin(now)) {
                    s_status = PC_NET_PEER_TIMEOUT;
                    return false;
                }
            }
        }
        if (now - last_send > 16000000ull) {
            send_inputs(); /* peer may be waiting on us, or lost our packets */
            last_send = now;
        }
        /* The receive thread is draining the socket meanwhile; half a
         * millisecond is how late its queue can be seen here. */
        SDL_DelayNS(500000);
    }
    if (s_rc != RSM_NONE) {
        resume_end(SDL_GetTicksNS());
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

/* True when this run's simulation has to be reproducible on another machine:
 * a netplay session, a recording, a replay, or a sync test. Game code uses
 * it to suppress the handful of retail behaviours that are deliberately
 * seeded from the local machine (see gmTitle_801A165C). Offline play keeps
 * every one of them. */
bool pc_net_deterministic(void) {
    return net.active || record_active() || net.synctest;
}

int pc_net_local_player(void) {
    return net.local;
}

/* ---- scene hand-off -------------------------------------------------
 *
 * A scene ends when its own code asks to, and how many TICKS it takes to
 * get there depends on the machine: the loads a scene waits on are polled
 * once per tick and the pad queue turns the wait into simulated frames, so
 * the peer that reads an archive faster leaves the scene earlier. Both then
 * run the next scene from different frames, which is a real divergence and
 * not an arithmetic one (docs/netcode-plan.md section 5.2).
 *
 * So the exit frame is agreed instead of inferred: each peer announces the
 * frame its scene asked to end on, and both leave on the later of the two
 * plus a lead that covers the reliable round trip. This is the pattern the
 * lobby already uses to hand over to the CSS (pc_lan_start_frame). */
#define SCENE_HANDOFF 20 /* frames of lead: ~330 ms, menus are lockstep */
#define SCENE_SLOTS 8    /* announcements kept: the peer may be a scene ahead */
/* How long a scene may wait for the other half of a hand-off before the
 * session is declared dead. Five seconds is far longer than any real
 * hand-off (the announcement is one reliable round trip on top of
 * SCENE_HANDOFF) and short enough that a peer which never answers does not
 * leave the player in a scene they cannot leave. */
#define SCENE_HOLD_MAX 300

/* Defined below; pc_net_scene_hold() is the one caller ahead of it. */
void pc_net_disconnect(void);

/* Every hand-off carries the count of scene exits the sender has made, and a
 * peer's frame is only ever paired with the exit of the same number. Without
 * it a late retransmit or an early announcement is indistinguishable from
 * this scene's answer: one peer then agrees a frame from the PREVIOUS scene,
 * releases immediately while the other is still waiting, and the sims part
 * on the frame that peer enters the next scene (measured tablet<->PC, "peer
 * 121" against an exit asked at 5775). */
static uint32_t s_scene_seq;                     /* exits we have completed */
static int32_t s_scene_exit_local = -1;          /* frame our scene asked to end on */
static int32_t s_scene_exit_at = -1;             /* agreed frame, once both are in */
static int32_t s_scene_wait_since = -1;          /* frame the incomplete wait began */
static int32_t s_scene_exit_remote[SCENE_SLOTS]; /* the peer's, by its own seq */

void net_scene_rel(const void* payload, int len) {
    SceneMsg m;
    if (len != (int)sizeof m) {
        pc_log_line("net: REL_SCENE of %d bytes ignored", len);
        return;
    }
    memcpy(&m, payload, sizeof m);
    int64_t f = (int32_t)ntohl(m.frame);
    /* A correct peer announces its own frame, which tracks ours to within the
     * few frames the hand-off is built for. Anything else is a corrupt or
     * hostile message, and pc_net_scene_hold() would turn it into a scene the
     * game can never leave (a far-future frame) or an exit taken alone (a
     * frame near INT32_MAX, where later + SCENE_HANDOFF overflows negative
     * and this side walks out while the peer is still waiting). */
    if (f < 0 || f > (int64_t)net.frame + RING) {
        pc_log_line("net: REL_SCENE for frame %d ignored (we are at %d)", (int32_t)f, net.frame);
        return;
    }
    /* The sequence is the peer's count of completed exits, so the only ones
     * that can be about a scene either side still cares about are the one we
     * are in and its immediate neighbours: the peer may be a scene ahead if
     * it left first, and a retransmit of the previous one may still arrive.
     * Anything further is stale or invented, and taking it would alias into
     * a live slot through the modulo -- an exit frame from eight scenes ago,
     * or a far-future one planted deliberately, landing in the slot the
     * current hand-off reads. */
    uint32_t seq = ntohl(m.seq);
    int64_t ahead = (int64_t)seq - (int64_t)s_scene_seq;
    if (ahead < -1 || ahead > 1) {
        pc_log_line("net: REL_SCENE seq %u ignored (we are on %u)", seq, s_scene_seq);
        return;
    }
    s_scene_exit_remote[seq % SCENE_SLOTS] = (int32_t)f;
}

static void scene_handoff_reset(void) {
    s_scene_seq = 0;
    s_scene_exit_local = s_scene_exit_at = -1;
    s_scene_wait_since = -1;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        s_scene_exit_remote[i] = -1;
    }
}

/* True while the caller must keep ticking the scene it has asked to leave.
 * Offline, and once the peer is gone, it never holds. */
bool pc_net_scene_hold(void) {
    if (!net.active) {
        /* Replaying a netplay recording: the scene has to end where the
         * recorded pair agreed, not where this run's own code asks to. */
        return record_replay_scene_hold(net.frame);
    }
    int32_t* remote = &s_scene_exit_remote[s_scene_seq % SCENE_SLOTS];
    if (s_scene_exit_local < 0) {
        SceneMsg m = {htonl(s_scene_seq), htonl((uint32_t)net.frame)};
        /* Only latch the frame if the announcement actually went out. The
         * lane is stop-and-wait four deep and shares itself with the resume,
         * delay, chat, stage-pick and ranked messages, so it can be full at
         * the instant a scene ends; latching anyway drops the announcement
         * for good, the peer waits on an exit frame it never gets, and the
         * two sims end up in different scenes. Retry on the next tick, the
         * way resume_send() already does. */
        if (pc_net_send_reliable(REL_SCENE, &m, sizeof m)) {
            s_scene_exit_local = net.frame;
        }
    }
    /* A hand-off that never completes is otherwise silent: the scene simply
     * never ends and the session stays healthy around it, which is
     * indistinguishable from a scene that has not asked yet. Say so once a
     * second while waiting, naming which half is missing -- and stop waiting
     * eventually. The peer keeps sending inputs, so neither the stall timeout
     * nor the watchdog notices a peer that has simply stopped answering this
     * exchange, and the player is left in a scene they cannot leave. */
    if (s_scene_exit_local < 0 || *remote < 0) {
        static int32_t said;
        if (s_scene_wait_since < 0) {
            s_scene_wait_since = net.frame;
        }
        if (net.frame - s_scene_wait_since >= SCENE_HOLD_MAX) {
            pc_log_line("net: scene %u hand-off never completed in %d frames (ours %d, peer "
                        "%d); leaving netplay",
                s_scene_seq, SCENE_HOLD_MAX, s_scene_exit_local, *remote);
            s_status = PC_NET_PEER_TIMEOUT;
            pc_net_disconnect();
            return false; /* the session is over: the scene ends here */
        }
        if (net.frame - said >= 60) {
            said = net.frame;
            pc_log_line("net: scene %u hand-off waiting at frame %d (ours %d, peer %d)",
                s_scene_seq, net.frame, s_scene_exit_local, *remote);
        }
        return true; /* the peer is still in the scene: wait for its frame */
    }
    s_scene_wait_since = -1;
    if (s_scene_exit_at < 0) {
        int32_t later = *remote > s_scene_exit_local ? *remote : s_scene_exit_local;
        s_scene_exit_at = later + SCENE_HANDOFF;
        /* Nothing may still be predicted when the scene goes: the next
         * scene's first tick raises the barrier past every outstanding
         * frame, and snapshot_unusable() rejects those snapshots anyway, so
         * a correction arriving then is dropped and the peers finish the
         * match on different remote inputs. Running the last frames of the
         * scene lockstep costs nothing -- they are the frames after the
         * match has already been decided -- and it is the same thing the
         * entry side does. */
        barrier_raise(s_scene_exit_at);
        pc_log_line("net: scene %u ends at frame %d (asked %d, peer %d)", s_scene_seq,
            s_scene_exit_at, s_scene_exit_local, *remote);
    }
    if (net.frame < s_scene_exit_at) {
        return true;
    }
    *remote = -1;
    s_scene_exit_local = s_scene_exit_at = -1;
    s_scene_seq++;
    return false;
}

/* True when the newest local sample holds no stick deflection and no button.
 * A whole-frame correction taken while the player is mid-input costs them
 * that frame of it; taken at rest it costs nothing anyone can feel. The
 * sample is the one already quantised for the wire, so idle stick noise
 * (at_rest) does not read as movement. */
bool net_local_idle(void) {
    if (s_wrote < 0) {
        return true;
    }
    const WirePad* p = &s_local_ring[s_wrote & (RING - 1)];
    return p->button == 0 && p->stickX == 0 && p->stickY == 0 && p->substickX == 0 &&
           p->substickY == 0 && p->triggerLeft == 0 && p->triggerRight == 0;
}

static void audio_journal_reset(void);
static void pad_stats_reset(void);
static void seed_stats_reset(void);
static void audit_reset(void);
static bool s_seen_remote;

static void session_reset(void) {
    scene_handoff_reset();
    s_seed_have = false;
    s_lockstep = snapshot_state_region_missing() != NULL;
    s_seen_remote = false;
    memset(s_local_ring, 0, sizeof s_local_ring);
    memset(s_remote_ring, 0, sizeof s_remote_ring);
    memset(s_ck_ring, 0, sizeof s_ck_ring);
    memset(s_sim_n, 0, sizeof s_sim_n);
    memset(s_rb_recent, 0, sizeof s_rb_recent);
    s_rb_recent_n = 0;
    sim_reset();
    memset(s_rx_held, 0, sizeof s_rx_held);
    net.frame = 0;
    net.tick_frame = -1;
    net.resim = false;
    s_remote_have = s_remote_newest = s_last_acked = s_rb_frame = s_remote_ck_frame = -1;
    s_remote_ck = 0;
    net.desync_reported = s_heard = s_peer_left = s_no_progress = false;
    s_last_rx_ns = 0;
    s_progress_ns = 0;
    s_warn_src = s_warn_sess = s_warn_bad = s_warn_mac = false;
    /* A fresh session starts with no key and trusting nothing it has not
     * yet authenticated; the key arrives with the handshake. */
    net_key_clear();
    s_mac_seen = false;
    s_mac_quiet = 0;
    s_status = PC_NET_PEER_OK;
    s_rc = RSM_NONE;
    s_rc_sent = false;
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
    s_rx_malformed = s_rx_bad_src = s_rx_bad_sess = s_rx_bad_player = s_tx_would_block = 0;
    s_rx_bad_mac = s_rx_full = 0;
    s_rx_have = -1;
    s_rx_left = false;
    s_rx_why = 0;
    s_rxq_head = s_rxq_n = 0;
    atomic_store(&s_frame_pub, 0);
    s_red_target = REDUNDANCY;
    s_red_floor = REDUNDANCY_FLOOR;
    s_resim_run = 0;
    s_resim_eat = 0;
    s_resim_eat_logged = false;
    s_timer_tried = false;
    s_loss_pct = 0;
    s_loss_tx_mark = s_loss_ack_mark = 0;
    sync_reset();
    audio_journal_reset();
    pad_stats_reset();
    seed_stats_reset();
    audit_reset();
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
    net.direct = false; /* pc_net_init() sets it back for a MELEE_NET session */
    net.seed = 0;
    net.start_frame = -1;
    net.ck_from = 0;
    snaps_free();
}

void pc_net_disconnect(void) {
    if (net.sock == SOCK_INVALID) {
        return;
    }
    /* Timer and receive thread first: a removed timer cannot fire again, and
     * taking tx_lock below waits out a callback already running; a joined
     * thread reads the socket no more, and what it queued dies with the
     * session. Nothing touches the socket after this returns. */
    if (s_timer) {
        SDL_RemoveTimer(s_timer);
        s_timer = 0;
    }
    if (s_rx_thread != NULL) {
        atomic_store(&s_rx_run, false);
        SDL_WaitThread(s_rx_thread, NULL);
        s_rx_thread = NULL;
    }
    s_rxq_head = s_rxq_n = 0;
    if (!s_peer_left) {
        /* Tell the peer why so it need not wait out the silence; sent
         * as a 5-packet burst so NAT/firewall drops are tolerated. */
        uint8_t why = s_status == PC_NET_PEER_OK ? PC_NET_PEER_LEFT : (uint8_t)s_status;
        net.sim_hold = false; /* straight out: the held queue dies with the socket */
        for (int i = 0; i < 5; i++) {
            SDL_LockMutex(net.tx_lock);
            send_bye(why);
            SDL_UnlockMutex(net.tx_lock);
            SDL_Delay(4);
        }
        if (s_status == PC_NET_PEER_OK) {
            s_status = PC_NET_PEER_LEFT;
        }
    }
    SDL_LockMutex(net.tx_lock);
    net.active = false;
    net.resim = false;
    sock_close(net.sock);
    net.sock = SOCK_INVALID;
    net.hs = HS_IDLE;
    /* After the BYE burst above, which still needed the key to be accepted:
     * the nonces it was derived from are dead with the session, so the key
     * does not outlive it in this process's memory either. */
    net_key_clear();
    s_mac_seen = false;
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
static int32_t s_exit_after; /* 0 = knob unset */
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

bool pc_net_desync(void) {
    return net.active && net.desync_reported;
}

void pc_net_peer_status_clear(void) {
    s_status = PC_NET_PEER_OK;
}

int pc_net_quality(void) {
    if (!net.active) {
        return 0;
    }
    if (s_rc == RSM_ACTIVE) {
        return 3; /* reconnecting; RSM_FAILED is one tick from the disconnect */
    }
    /* A desync is as bad as a stall and never recovers, so it reads as at
     * least 2 however healthy the link looks around it. */
    if (s_peer_left || net.desync_reported || net.frame - s_stall_frame < 120) {
        return 2;
    }
    if (s_loss_pct >= 5 || s_rb_depth_recent >= 4 || jitter_us() >= 8000) {
        return 1;
    }
    return 0;
}

static int s_configured_delay = -1;
void pc_net_set_input_delay(int frames) {
    s_configured_delay = frames >= 0 && frames <= 4 ? frames : -1;
}

/* Is this a destination a peer could answer from? A network, broadcast or
 * multicast address is never a host, and a session pointed at one can only
 * time out -- which, with the connect wait parking the render thread, used to
 * present as a crash rather than as a failure to connect. Seen in the wild
 * (issue #87): a pairing dialled a /8 network base -- the peer's first octet
 * with the other three zeroed -- instead of the address its datagrams had
 * actually arrived from. */
static bool addr_is_host(const struct sockaddr* sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)sa;
        uint32_t ip = ntohl(a->sin_addr.s_addr);
        return a->sin_port != 0 && ip != 0 && ip != 0xffffffffu && (ip >> 28) < 14 &&
               (ip & 0xffffffu) != 0;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)sa;
        static const uint8_t any[16] = {0};
        return a->sin6_port != 0 && memcmp(&a->sin6_addr, any, sizeof any) != 0 &&
               a->sin6_addr.s6_addr[0] != 0xff;
    }
    return false;
}

static bool connect_impl(
    sock_t supplied, const char* ip, uint16_t port, int player, uint32_t seed) {
    if (net.tx_lock == NULL) {
        net.tx_lock = SDL_CreateMutex();
        s_rx_lock = SDL_CreateMutex();
        sock_startup();
    }
    pc_net_disconnect();
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = supplied == SOCK_INVALID ? AF_UNSPEC : AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(ip, portstr, &hints, &res) != 0 || res == NULL) {
        pc_log_line("net: cannot resolve %s:%u", ip, port);
        return false;
    }
    if (!addr_is_host(res->ai_addr)) {
        pc_log_line("net: refusing to connect to %s:%u -- not a unicast host address", ip, port);
        freeaddrinfo(res);
        return false;
    }
    memcpy(&net.peer, res->ai_addr, res->ai_addrlen);
    net.peer_len = (socklen_t)res->ai_addrlen;
    int family = res->ai_family;
    freeaddrinfo(res);

    sock_t sock = supplied == SOCK_INVALID ? socket(family, SOCK_DGRAM, 0) : supplied;
    if (sock == SOCK_INVALID) {
        pc_log_line("net: socket() failed");
        return false;
    }
    const char* lport = getenv("MELEE_NET_PORT");
    unsigned short bind_port = (unsigned short)(lport ? atoi(lport) : 41000);
    struct sockaddr_storage local;
    memset(&local, 0, sizeof local);
    socklen_t local_len;
    if (family == AF_INET6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*)&local;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(bind_port);
        local_len = sizeof *a;
    } else {
        struct sockaddr_in* a = (struct sockaddr_in*)&local;
        a->sin_family = AF_INET;
        a->sin_port = htons(bind_port);
        local_len = sizeof *a;
    }
    if (supplied == SOCK_INVALID && bind(sock, (struct sockaddr*)&local, local_len) != 0) {
        /* The reason matters: "address already in use" is another instance
         * (or one still tearing down) holding the port, and the session then
         * silently does not happen at all -- which looks exactly like two
         * peers playing unconnected games side by side. */
        pc_log_line("net: bind(%u) failed, error %d (in use by another instance?)", bind_port,
            sock_last_err());
        sock_close(sock);
        return false;
    }
    if (!sock_nonblock(sock)) {
        pc_log_line("net: could not put the socket in non-blocking mode");
        if (supplied == SOCK_INVALID)
            sock_close(sock);
        return false;
    }
    if (supplied != SOCK_INVALID) {
        struct sockaddr_in bound;
        socklen_t length = sizeof bound;
        if (getsockname(sock, (struct sockaddr*)&bound, &length) == 0)
            bind_port = ntohs(bound.sin_port);
    }
    /* Ask the network for expedited forwarding (DSCP EF / 46): low-latency
     * queueing where the path honours it, harmless where it does not. */
#if defined(_WIN32)
    /* ponytail: Windows ignores IP_TOS for DSCP; real marking there needs the
     * qWAVE API (QOSAddSocketToFlow). Skipped in the prototype. */
#else
    {
        int tos = 0xB8; /* DSCP 46 << 2, ECN 0 */
        int qr = family == AF_INET6 ?
                     setsockopt(sock, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos) :
                     setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
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
    /* A hand-typed address may be a peer the user has not started yet, so it
     * keeps the long wait. One handed over from matchmaking (pc_net_connect_
     * socket, the only caller that passes a socket) belongs to a peer that
     * answered a signed offer seconds ago: if it says nothing now, the
     * destination is dead, and 60 s of a parked render thread looks exactly
     * like the crash in issue #87. */
    net.connect_timeout_ms =
        supplied == SOCK_INVALID ? CONNECT_TIMEOUT_MS : MATCH_CONNECT_TIMEOUT_MS;
    const char* delay = getenv("MELEE_NET_DELAY");
    net.delay_auto = delay ? strcmp(delay, "auto") == 0 : s_configured_delay < 0;
    net.delay = net.delay_auto ? 2 : delay ? atoi(delay) : s_configured_delay;
    if (net.delay < 0 || net.delay >= RING / 2) {
        net.delay = 2;
    }
    net.delay_next = net.delay;
    s_wrote = net.delay - 1; /* frames 0..delay-1 stay neutral on both sides */
    /* MELEE_NET_ROLLBACK=off: keep the barrier ahead of every frame, which
     * is the lockstep the out-of-memory path already falls back to (nothing
     * is predicted, nothing is snapshotted, nothing is re-run). The session
     * then runs at the link's round trip, so this is a diagnostic: a row
     * that desyncs with rollback on and survives with it off has its fault
     * in the re-simulation, not in the input pipeline. */
    const char* rb = getenv("MELEE_NET_ROLLBACK");
    if (rb != NULL && strcmp(rb, "off") == 0) {
        barrier_raise(INT32_MAX);
        pc_log_line("net: rollback off (MELEE_NET_ROLLBACK), lockstep at the link's round trip");
    }
    /* 0 means "no resume phase at all", so it must survive the parse; a
     * negative or unparseable value falls back to the default rather than
     * silently disabling the feature. */
    const char* rc_ms = getenv("MELEE_NET_RECONNECT_MS");
    char* rc_end = NULL;
    int64_t rc_want = rc_ms != NULL ? strtoll(rc_ms, &rc_end, 10) : RECONNECT_MS;
    /* Anything unparseable, negative or wider than the field falls back to the
     * default; 0 is legal and means "no reconnect phase, drop as before". */
    bool rc_bad =
        rc_ms != NULL && (rc_end == rc_ms || *rc_end != '\0' || rc_want < 0 || rc_want > INT32_MAX);
    s_rc_window_ms = rc_bad ? RECONNECT_MS : (int32_t)rc_want;
    sim_env(bind_port);
    /* The host names the session; the guest takes it from the first packet. */
    net.session = net.local == 0 ?
                      (uint32_t)(SDL_GetPerformanceCounter() ^ (uint64_t)getpid() << 20) | 1u :
                      0;
    net.seed = seed;
    if (seed != 0) {
        *HSD_RandSeedPtr = seed;
    }
    /* MELEE_NET_KEY: one secret typed by whoever starts both instances. The
     * handshake derives a key of its own from the two nonces, but only once
     * it completes, so this is what covers the bootstrap window before that
     * -- and it is all a peer has if the two never agree. It pins the key
     * for the whole session -- a handshake later on will not replace it --
     * because rekeying in flight would drop every datagram that straddled
     * the change, and the two peers cannot make that change on the same
     * frame. */
    const char* key = getenv("MELEE_NET_KEY");
    if (key != NULL && key[0] != '\0') {
        net_key_direct(key);
        pc_log_line("net: datagrams authenticated from MELEE_NET_KEY (the handshake will not "
                    "rekey this session)");
    }
    SDL_LockMutex(net.tx_lock);
    net.sock = sock;
    net.active = true;
    SDL_UnlockMutex(net.tx_lock);
    pc_log_line(
        "net: rollback with %s:%u, local port %u, player P%d, delay %s%d, window %d, "
        "seed %u, session %08x, proto %d, sim loss %d%% delay %d/%d ms jitter %d reorder %d%% "
        "dup %d%% burst %d",
        ip, port, bind_port, net.local + 1, net.delay_auto ? "auto " : "", net.delay, WINDOW, seed,
        net.session, WIRE_VERSION, net.sim_loss, (int)(net.sim_delay_ns / 1000000),
        (int)(net.sim_rx_delay_ns / 1000000), net.sim_jitter_ms, net.sim_reorder, net.sim_dup,
        net.sim_burst);
    atomic_store(&s_rx_run, true);
    s_rx_thread = SDL_CreateThread(rx_main, "net rx", NULL);
    if (s_rx_thread == NULL) {
        /* Still a working session, only without the fix: recv_inputs drains
         * the socket itself, and a stall of this thread reads as latency. */
        pc_log_line("net: no receive thread; receiving on the game thread");
    }
    /* The FP control word, from inside the shipped process. Flush-to-zero or
     * a non-default rounding mode on one peer and not the other silently
     * changes every result; it was measured IEEE-default on both targets,
     * but only ever in a shell process, because a release APK cannot be
     * ptraced. Two peers' logs now answer it directly. */
    {
        uint64_t fp = 0;
#if defined(__aarch64__)
        __asm__ volatile("mrs %0, fpcr" : "=r"(fp));
#elif defined(__x86_64__) || defined(__i386__)
        uint32_t mxcsr = 0;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        fp = mxcsr;
#endif
        pc_log_line("net: fp control word %08" PRIx64 " (0 = IEEE default on aarch64; "
                    "x86 default 1f80)",
            fp);
    }
    return true;
}

bool pc_net_connect(const char* ip, uint16_t port, int player, uint32_t seed) {
    return connect_impl(SOCK_INVALID, ip, port, player, seed);
}
bool pc_net_connect_socket(
    intptr_t socket, const char* ip, uint16_t port, int player, uint32_t seed) {
    if (socket == -1)
        return false;
    return connect_impl((sock_t)socket, ip, port, player, seed);
}

void pc_net_init(void) {
    s_game_thread = SDL_GetCurrentThreadID(); /* pc_platform_init runs on it */
    const char* peer = getenv("MELEE_NET");
    if (peer == NULL || peer[0] == '\0') {
        return;
    }
    char host[256];
    const char* colon = strrchr(peer, ':');
    if (colon == NULL || (size_t)(colon - peer) >= sizeof host) {
        pc_log_line("net: MELEE_NET must be host:port");
        return;
    }
    memcpy(host, peer, (size_t)(colon - peer));
    host[colon - peer] = '\0';
    const char* player = getenv("MELEE_NET_PLAYER");
    const char* seed = getenv("MELEE_SEED");
    if (!pc_net_connect(host, (uint16_t)atoi(colon + 1), player && player[0] == '1',
            seed ? (uint32_t)strtoul(seed, NULL, 0) : 0))
    {
        return;
    }
    /* No lobby agreed this match, so the session agrees it itself: the same
     * RULES/READY exchange a lobby session runs, hosted by player 1, fired
     * from the frame loop once the game has filled its own rules in
     * (handshake_direct, net_handshake.c). Everything match-affecting is
     * therefore validated rather than assumed, including the seed -- the
     * host's wins, so MELEE_SEED no longer has to be set, or set equal, on
     * both peers. */
    net.direct = true;
    /* Said plainly rather than left to be inferred from a missing line: the
     * key arrives with the handshake, so until it completes there is nothing
     * to authenticate these datagrams with and anything that can reach this
     * port can inject input, acks, a delay change or a BYE. A shared secret
     * closes that window from the first packet. */
    const char* key = getenv("MELEE_NET_KEY");
    if (key == NULL || key[0] == '\0') {
        pc_log_line("net: this MELEE_NET session is UNAUTHENTICATED until its handshake "
                    "completes; set the same MELEE_NET_KEY on both peers to authenticate its "
                    "datagrams from the first one");
    }
}

int32_t pc_net_start_frame(void) {
    return net.active ? net.start_frame : -1;
}

void pc_net_poll(void) {
    if (!net.active) {
        return;
    }
    recv_inputs();
    send_inputs(); /* includes reliable retransmits while simulation is parked */
    if (s_peer_left) {
        pc_net_disconnect();
    }
}

int32_t pc_net_frame(void) {
    return net.tick_frame;
}

bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks) {
    if (!net.active) {
        return false;
    }
    *ping_ms = (int)(net.ping_us / 1000);
    *delay_frames = net.delay;
    *rollbacks = s_rollbacks;
    return true;
}

/* ---- audio answers the simulation is allowed to see --------------------
 * pc/net.h has the why. One journal per frame in a RING-deep ring, replayed
 * in call order: the order of audio questions inside a tick is a function
 * of the state and the inputs, so the answer sequence is too. Journalling
 * is armed only between the start of a tick's frame and the end of that
 * tick, so a query from the render phase or the audio thread cannot shift
 * the sequence. */
#define AUDIO_J 32
static struct AudioJournal {
    int32_t frame;
    uint8_t n;
    int32_t v[AUDIO_J];
} s_aj[RING];
static uint8_t s_aj_i;                   /* answers replayed/recorded so far this tick */
static bool s_aj_on;                     /* a tick is running */
static unsigned s_aj_over, s_aj_replays; /* answers that did not fit / were replayed */
static int s_aj_off = -1;                /* MELEE_NET_AUDIO_JOURNAL=off */

static bool audio_journal_live(void) {
    if (s_aj_off < 0) {
        const char* e = getenv("MELEE_NET_AUDIO_JOURNAL");
        s_aj_off = e != NULL && strcmp(e, "off") == 0;
    }
    return s_aj_off == 0 && s_aj_on && net.active && SDL_GetCurrentThreadID() == s_game_thread;
}

/* A frame's simulation begins (a fresh tick or a re-run of it). */
static void audio_journal_begin(int32_t f) {
    s_aj_i = 0;
    s_aj_on = true;
    struct AudioJournal* j = &s_aj[f & (RING - 1)];
    if (j->frame != f) {
        j->frame = f;
        j->n = 0;
    }
}

bool pc_net_audio_replay(int32_t* out) {
    if (!audio_journal_live()) {
        return false;
    }
    struct AudioJournal* j = &s_aj[net.tick_frame & (RING - 1)];
    if (j->frame != net.tick_frame || s_aj_i >= j->n) {
        return false;
    }
    *out = j->v[s_aj_i++];
    s_aj_replays++;
    return true;
}

int32_t pc_net_audio_record(int32_t v) {
    if (!audio_journal_live()) {
        return v;
    }
    struct AudioJournal* j = &s_aj[net.tick_frame & (RING - 1)];
    if (j->frame == net.tick_frame && j->n == s_aj_i && s_aj_i < AUDIO_J) {
        j->v[s_aj_i] = v;
        j->n = ++s_aj_i;
    } else {
        s_aj_over++;
    }
    return v;
}

/* "Is that voice still playing" during a session: the simulation is told no,
 * because the true answer is a wall-clock fact and the two peers' audio
 * clocks are not the same clock (axdriver.c AXDriver_8038D9D8 has the why).
 * False outside a session, so offline play keeps the real answer.
 * MELEE_NET_AUDIO_DEAF=off restores it and is the regression test for the
 * desync it causes. s_deaf_true counts how often the engine would have said
 * "still playing", which is the size of the behaviour change. */
static int s_deaf_off = -1;
static unsigned s_deaf_asks, s_deaf_true;

bool pc_net_audio_deaf(bool* answer) {
    if (s_deaf_off < 0) {
        const char* e = getenv("MELEE_NET_AUDIO_DEAF");
        s_deaf_off = e != NULL && strcmp(e, "off") == 0;
    }
    if (s_deaf_off != 0 || !net.active || SDL_GetCurrentThreadID() != s_game_thread) {
        return false;
    }
    s_deaf_asks++;
    *answer = false;
    return true;
}

/* What the engine would have answered, for the report: called by axdriver.c
 * only when the choke is on and only to measure it. */
void pc_net_audio_deaf_note(bool live) {
    if (live) {
        s_deaf_true++;
    }
}

static void audio_journal_reset(void) {
    memset(s_aj, 0, sizeof s_aj);
    s_aj_i = 0;
    s_aj_on = false;
    s_aj_over = 0;
    s_aj_replays = 0;
    s_deaf_asks = 0;
    s_deaf_true = 0;
}

/* Only the sound path asks (axdriver.c AXDriver_8038CFF4, lbaudio_ax.c
 * lbAudioAx_80023F28): a re-simulated frame must not start its sounds a
 * second time. That choke is the one thing a re-simulated frame does
 * differently from a fresh one, so MELEE_NET_RESIM_SFX=1 lifts it: a row
 * that desyncs with the choke and survives without it has its divergence
 * inside the sound path, not in the physics. */
bool pc_net_resim(void) {
    static int choke = -1;
    if (choke < 0) {
        const char* e = getenv("MELEE_NET_RESIM_SFX");
        choke = e != NULL && e[0] == '1' ? 0 : 1;
    }
    return choke != 0 && (net.resim || net.synctest);
}

/* Every input wait, including a failed snapshot, has the same disconnect
 * handling. A failed snapshot must not allow a speculative tick to run. */
static bool wait_input(int32_t need) {
    if (wait_remote(need)) {
        return true;
    }
    /* A refused resume logged its own reason, and the peer was not
     * silent at all: its answer was simply unusable. */
    if (!s_peer_left && s_rc != RSM_FAILED) {
        if (s_no_progress) {
            pc_log_line("net: peer still sending but stuck at frame %d for %d ms, leaving netplay",
                s_remote_newest, NO_PROGRESS_TIMEOUT_MS);
        } else {
            pc_log_line("net: peer silent for %d ms at frame %d, leaving netplay",
                rx_heard_ns() ? STALL_TIMEOUT_MS : connect_timeout_ms(), net.frame);
        }
    }
    /* The BYE can arrive while the game thread is parked in
     * wait_remote() rather than in recv_inputs() above, and that is the
     * usual case once one-way delay is high enough to stall every
     * frame. Without this the stalling side never ends the test and the
     * harness kills it, which reads as a netplay failure. */
    if (s_peer_left) {
        exit_if_test_done();
    }
    pc_net_disconnect();
    return false;
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
 * grown, disable further prediction without invalidating older snapshots.
 * The caller must wait for real input before running this frame. */
static bool snap_predicted(int32_t f) {
    Snapshot* s = snap_slot(f);
    if (s_lockstep || f <= net.rb_barrier) {
        return false;
    }
    if (!snapshot_take(s, f)) {
        /* Out of memory, or a platform whose linker cannot bracket the
         * decomp's statics at all (net_snapshot.c). */
        const char* why = snapshot_state_region_missing();
        s_lockstep = true;
        pc_log_line("net: %s at frame %d, lockstep from here",
            why != NULL ? why : "out of memory for snapshots", f);
        return false;
    }
    return true;
}

/* MELEE_NET_RESIM_AUDIT_POISON=1: the audit's discarded pass runs with a
 * deliberately wrong remote input, which is what a real misprediction is.
 * The true re-run after it must still land exactly where the first pass
 * did; anything the wrong pass left behind that the snapshot does not
 * cover shows up there and nowhere else. */
static bool s_audit_poison;      /* a wrong-input pass is running now */
static bool s_audit_poison_want; /* and the knob that asks for one */

/* The queue slot HSD_PadRenewMasterStatus will consume next. */
static PADStatus* pad_head(void) {
    PadLibData* p = &HSD_PadLibData;
    return &p->queue->stat[p->qread * 4];
}

/* The raw pad queue must never move its read cursor under the inputs a tick
 * is about to consume. With qtype 0 a full queue makes HSD_PadRenewRawStatus
 * shift qread, merge the dropped sample's buttons into the next one and
 * overwrite the slot write_head just filled (controller.c:77-105); the tick
 * then simulates a raw local sample -- the remote port reads as "no
 * controller", the local port as the undelayed physical pad -- while the
 * frame's checksum still reports the synced inputs. One such frame on one
 * peer is a permanent divergence.
 *
 * pc_net_connect sets qtype 2 (drop the new sample instead), but in
 * MELEE_NET mode it connects from pc_platform_init, and the game's own
 * gmMain_8015FD24 later runs HSD_PadInit, which copies default_libinfo_data
 * over the whole PadLibData and puts qtype back to 0. So the invariant is
 * re-asserted every tick: one byte, and it cannot be lost by a re-init.
 * MELEE_NET_PAD_QTYPE=0 restores the shifting queue and is the regression
 * test for the desync it causes. */
static void pad_qtype_hold(void) {
    static int want = -1;
    if (want < 0) {
        const char* e = getenv("MELEE_NET_PAD_QTYPE");
        want = e != NULL ? atoi(e) : 2;
        if (want != 2) {
            pc_log_line(
                "net: raw pad queue type %d (MELEE_NET_PAD_QTYPE), expect input slips", want);
        }
    }
    HSD_PadLibData.qtype = (uint8_t)want;
}

/* Detector for the same hazard, kept as the regression test: what
 * write_head put in the queue head for the frame about to be simulated,
 * against what the tick actually consumed. Both must agree on every tick of
 * a session; a mismatch is the divergence above, and the counters below go
 * into the periodic report so a run can be believed. */
static uint16_t s_head_button[4];
static int32_t s_head_frame = -1;
static const PADStatus* s_head_ptr; /* slot write_head filled */
static uint8_t s_head_qread, s_head_qwrite, s_head_qcount;
static uint32_t s_head_retrace; /* frame boundaries seen at the write */
static unsigned s_pad_slips, s_pad_full, s_tick_idle;
static uint8_t s_qdepth_max;

static void pad_stats_reset(void) {
    s_pad_slips = 0;
    s_pad_full = 0;
    s_tick_idle = 0;
    s_qdepth_max = 0;
    s_head_frame = -1;
    net.pad_reuse = 0;
    net.pad_empty = 0;
}

static void head_note(const PADStatus* head, int32_t f) {
    const PadLibData* p = &HSD_PadLibData;
    for (int i = 0; i < 4; i++) {
        s_head_button[i] = head[i].button;
    }
    s_head_frame = f;
    s_head_ptr = head;
    s_head_qread = p->qread;
    s_head_qwrite = p->qwrite;
    s_head_qcount = p->qcount;
    s_head_retrace = VIGetRetraceCount();
    if (p->qcount >= p->qnum) {
        s_pad_full++;
    }
    if (p->qcount > s_qdepth_max) {
        s_qdepth_max = p->qcount;
    }
}

static void head_check(void) {
    if (s_head_frame < 0 || !net.active) {
        return;
    }
    const PadLibData* p = &HSD_PadLibData;
    for (int i = 0; i < 2; i++) {
        int port = i == 0 ? net.local : net.remote;
        /* HSD_PadADConvert ORs synthetic direction bits above 0xffff into
         * button (controller.c), so only the real pad bits can be compared. */
        uint32_t saw = HSD_PadMasterStatus[port].button & 0xffffu;
        if (saw != s_head_button[port]) {
            s_pad_slips++;
            if (s_pad_slips <= 8) {
                /* The slot the tick consumed is one behind the read cursor it
                 * left behind; if that is not the slot write_head filled,
                 * something moved the queue in between. */
                int read_slot = (p->qread + p->qnum - 1) % p->qnum;
                pc_log_line("net: pad slip at frame %d port %d: wrote %04x, tick consumed %04x "
                            "(wrote slot %d of %d at qcount %d/%d qwrite %d; tick read slot %d, "
                            "now qread %d qcount %d qwrite %d; retrace %u -> %u)",
                    s_head_frame, port, s_head_button[port], saw, s_head_qread, p->qnum,
                    s_head_qcount, p->qnum, s_head_qwrite, read_slot, p->qread, p->qcount,
                    p->qwrite, s_head_retrace, VIGetRetraceCount());
            }
        }
    }
    /* And did the tick advance the simulation at all? gm_RunSimTick only
     * runs gm_EvaluateAllControllerInputs and the scene's frame proc when
     * lb_80019A30(0) is set, which lb_80019900 recomputes from an
     * accumulator in lb_0195.c -- a TU the snapshot deliberately excludes.
     * At 60 Hz it is true every tick; if a re-run tick ever lands on a
     * false one, that frame was counted but never simulated. */
    if (!lb_80019A30(0)) {
        s_tick_idle++;
        if (s_tick_idle <= 4) {
            pc_log_line(
                "net: tick at frame %d did not advance the sim (lb_80019A30 false)", s_head_frame);
        }
    }
    s_head_frame = -1;
}

/* Ports 0-3 of the queue head become the synced inputs for frame f. */
static void write_head(PADStatus* head, int32_t f) {
    static const WirePad neutral;
    const WirePad* mine = f >= net.delay ? &s_local_ring[f & (RING - 1)] : &neutral;
    from_wire(&head[net.local], mine);
    const WirePad* theirs = &s_remote_ring[f & (RING - 1)];
    WirePad wrong;
    if (s_audit_poison) {
        wrong = *theirs;
        wrong.stickX = (int8_t)-wrong.stickX;
        wrong.button ^= 0x0100; /* A */
        theirs = &wrong;
    }
    from_wire(&head[net.remote], theirs);
    if (!s_seen_remote && theirs->button != 0) {
        s_seen_remote = true;
        pc_log_line("net: first remote button press (%04x) at frame %d", theirs->button, f);
    }
    for (int i = 2; i < 4; i++) {
        memset(&head[i], 0, sizeof head[i]);
        head[i].err = PAD_ERR_NO_CONTROLLER;
    }
    head_note(head, f);
}

/* Re-expose the slot the last tick consumed so the next tick has one to
 * consume; the raw sample in it was already captured. Fails when full. */
static PADStatus* unconsume(void) {
    PadLibData* p = &HSD_PadLibData;
    if (p->qcount >= p->qnum) {
        return NULL;
    }
    p->qread = (uint8_t)((p->qread + p->qnum - 1) % p->qnum);
    p->qcount++;
    return &p->queue->stat[p->qread * 4];
}

/* Set up the re-run of frame f: refresh its snapshot if it is still
 * predicted (the old one holds the discarded timeline), then feed it. */
static bool resim_prepare(int32_t f) {
    if (f > s_remote_have) {
        if (!snap_predicted(f) && !wait_input(f)) {
            return false;
        }
        if (f > s_remote_have) {
            predict(f);
        }
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
                        "sample",
                f);
        }
    }
    write_head(head, f);
    s_ck_ring[f & (RING - 1)] = frame_checksum(head);
    /* The state ring is the last simulation of each frame, which is the
     * confirmed timeline (the desync dump and the audit's field-level diff
     * both read it); without this it held first-pass values only. The
     * recording wants the same treatment: this re-run replaces the staged
     * record for f, so what reaches the file is the pass that stood. */
    record_state(head, f);
    record_frame(head, s_ck_ring[f & (RING - 1)], f);
    if (s_sim_n[f & (RING - 1)] < 255) {
        s_sim_n[f & (RING - 1)]++;
    }
    net.tick_frame = f;
    audio_journal_begin(f); /* the re-run replays this frame's audio answers */
    return true;
}

static bool rollback_to(int32_t f) {
    Snapshot* s = snap_slot(f);
    const char* why = s->buf == NULL || s->frame != f ? "no snapshot" :
                      f <= net.rb_barrier             ? "behind the barrier" :
                                                        snapshot_unusable(s);
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
    /* Rumble nodes and active heads were rewound: their free-list head must
     * come from that same snapshot, not the discarded live timeline. */
    pad.rumble_info = HSD_PadLibData.rumble_info;
    HSD_PadLibData = pad;
    OSRestoreInterrupts(intr);
    s_rollbacks++;
    s_rb_recent[s_rb_recent_n++ % 16] = f;
    if (depth > s_rb_depth_max) {
        s_rb_depth_max = depth;
    }
    if (depth > s_rb_depth_cur) {
        s_rb_depth_cur = depth;
    }
    net.resim = true;
    return resim_prepare(f);
}

/* ---- per-tick entry --------------------------------------------------- */

/* Draws of random.c's LCG between two seeds, -1 past the bound. A
 * re-simulation that took a different branch usually shows up here first:
 * same fighter positions, a seed a few draws apart. */
static int seed_steps(uint32_t from, uint32_t to) {
    for (int i = 0; i <= 4096; i++) {
        if (from == to) {
            return i;
        }
        from = from * 214013u + 2531011u;
    }
    return -1;
}

/* Seed at the end of the last tick of a present, and how far it had moved
 * by the start of the next one: everything between those two points is
 * outside every tick (the render phase, the audio thread), so a draw there
 * is state the rollback re-run can never reproduce. */
static unsigned s_seed_out_draws, s_seed_out_frames;

static void seed_stats_reset(void) {
    s_seed_out_draws = 0;
    s_seed_out_frames = 0;
}

/* Called at the top of a tick. The seed is folded into the netplay checksum
 * (net_snapshot.c:148) and outside a fight the checksum is ONLY the pads and
 * this seed, so a draw between two ticks that happens on one peer and not
 * the other is by itself a desync.
 *
 * Two separate causes, both measured, both fixed:
 *
 * 1. The wall clock. gmtitle.c:161-170 stirred the RNG once per
 *    seconds-of-minute, so two machines that reached the title eight
 *    wall-seconds apart stirred 41 times against 49 (phone<->PC). Fixed at
 *    the source, because no repair here can invent the draws the other peer
 *    made.
 *
 * 2. Scene timing. The rest are scene-enter draws -- stage init, CPU AI init
 *    inside Fighter_Create -- backtraced across three 9900-frame sessions
 *    and never paced by rendering. Two peers of similar speed make them on
 *    the same frame, but a slow device does not: an SM-T505 against this PC
 *    drew at frame 232 where the PC drew later, and desynced there. The seed
 *    a tick starts from must therefore be the seed the last tick ended with,
 *    whatever the gap between them contained.
 *
 * Netplay only (`net.active`). A recording leg must NOT do this: whether a
 * draw falls inside or outside a tick depends on load timing, so rewriting
 * the seed makes a recording that only replays on a machine of the same
 * speed -- measured, it moved the replay divergence to frame 1. Netplay is
 * the opposite case: both peers run one frame stream and the repair is
 * applied identically on each, which is what makes them agree. */
static void seed_out_of_tick_check(void) {
    if (!s_seed_have) {
        return;
    }
    uint32_t now = *HSD_RandSeedPtr;
    if (now == s_seed_after_tick) {
        return;
    }
    int steps = seed_steps(s_seed_after_tick, now);
    s_seed_out_frames++;
    s_seed_out_draws += steps > 0 ? (unsigned)steps : 0;
    if (s_seed_out_frames <= 3) {
        pc_log_line("net: seed moved outside the tick at frame %d: %08x -> %08x (%d draws)%s",
            net.frame, s_seed_after_tick, now, steps, net.active ? ", restoring" : "");
    }
    if (net.active) {
        *HSD_RandSeedPtr = s_seed_after_tick;
    }
}

/* How many TICKS a load costs must not depend on this machine's disc speed.
 * The game polls a read's completion once per tick, so a peer whose file
 * cache is warm finishes in one tick while the other spends hundreds --
 * measured tablet<->PC on a real match: the tablet entered the fight at
 * frame 39470 and this PC at 40353, 883 frames (~15 s) later, with an
 * identical seed and identical pads, and the session desynced on the first
 * frame `frame_checksum` folded fighter fields for (it folds them only while
 * in_fight()). Draining the queue before the tick makes the read complete
 * inside the tick that issued it, so the poll succeeds on the same tick for
 * both peers.
 *
 * Netplay only, and bounded: a wedged read must not hang the game, and while
 * this blocks, the peer simply sees the stall that wait_remote() is built to
 * ride out. */
static void dvd_settle(void) {
    if (aurora_dvd_inflight() <= 0 && aurora_arq_inflight() <= 0) {
        return;
    }
    const uint64_t t0 = SDL_GetTicksNS();
    static bool warned;
    while (aurora_dvd_inflight() > 0 || aurora_arq_inflight() > 0) {
        if (SDL_GetTicksNS() - t0 > 5000000000ull) {
            /* The transfer is not going to settle in time. Letting the tick
             * run here was the dangerous half of this: past this point the
             * two peers can be simulating the same frame with different I/O
             * completed, and nothing downstream would notice until the
             * checksum happened to cover whatever the load changed. Run on,
             * because parking the game thread indefinitely is worse, but
             * take rollback off the table for a while: a lockstep frame is
             * still the same frame on both sides, and it cannot be re-run
             * from a snapshot that may predate the completion. */
            barrier_raise(net.frame + IO_QUIET);
            if (!warned) {
                warned = true;
                pc_log_line("net: disc/ARAM transfer still in flight after 5 s at frame %d; "
                            "running on in lockstep to frame %d",
                    net.frame, net.rb_barrier);
            }
            return;
        }
        /* This thread is alive and spinning, not wedged: say so, or the
         * watchdog's 5 s threshold is exactly this loop's own 5 s cap and a
         * disc drain reports a healthy thread (and dumps a stack from inside
         * whatever it happens to hold). */
        net_watchdog_heartbeat();
        SDL_DelayNS(200000);
    }
}

/* MELEE_NET_STALL_TEST=frame[:ms] (fixture): park the game thread mid-match,
 * standing in for a load the netcode cannot shorten -- a stage, a character,
 * a first-time shader compile. Only the guest does it, so one exported value
 * stalls exactly one side of a two-process run. tx_timer keeps sending
 * throughout, which is the condition wait_remote() has to tell apart from a
 * peer that is actually gone. */
static void stall_test(void) {
    static bool parsed;
    static int32_t at = -1;
    static int ms = 10000;
    if (!parsed) {
        parsed = true;
        const char* s = getenv("MELEE_NET_STALL_TEST");
        if (s != NULL) {
            const char* colon = strchr(s, ':');
            at = (int32_t)atoi(s);
            if (colon != NULL) {
                ms = atoi(colon + 1);
            }
        }
    }
    if (at >= 0 && net.frame == at && net.local == 1) {
        pc_log_line("net: stall test: game thread asleep %d ms at frame %d", ms, at);
        SDL_Delay((Uint32)ms);
    }
}

/* Read the latest published hardware state immediately before input is
 * sent. Extra time-sync ticks and rollback must reuse their recorded sample;
 * they must never sample hardware a second time. Physical port zero is the
 * local controller even when matchmaking assigned this peer player two. */
static void capture_local_sample(bool fresh) {
    if (fresh) {
        PADStatus pads[4];
        PADRead(pads);
        s_raw_last = pads[0];
    }
}

/* A fresh frame: capture the local sample, exchange inputs, predict or
 * stall, and feed the queue head. `raw` is false for the extra tick a
 * time-sync advance adds, which reuses the last physical sample. */
static void fresh_tick(PADStatus* head, bool raw) {
    if (net.active) {
        /* Part cleanly rather than run the frame counter toward the end of
         * its range; every ring index and every wire field is an absolute
         * int32 frame, and there is no wrap strategy behind them. */
        if (net.frame >= SESSION_MAX_FRAMES) {
            pc_log_line("net: session frame limit reached at %d, disconnecting", net.frame);
            pc_net_disconnect();
            return;
        }
        stall_test();
        static int timing_debug = -1;
        static uint64_t sample_send_total, sample_send_max;
        static unsigned sample_send_count;
        if (timing_debug < 0)
            timing_debug = getenv("MELEE_NET_DEBUG") != NULL;
        uint64_t sampled_at = timing_debug && raw ? SDL_GetTicksNS() : 0;
        capture_local_sample(raw);
        /* One slot per tick; a delay change (delay_auto) leaves a gap to fill
         * with the same sample, or already-sent frames that must not move. */
        int64_t target_w = (int64_t)net.frame + net.delay;
        for (int32_t w = s_wrote + 1; (int64_t)w <= target_w; w++) {
            to_wire(&s_local_ring[w & (RING - 1)], &s_raw_last);
            s_wrote = w;
        }
        send_inputs();
        if (sampled_at) {
            uint64_t elapsed = SDL_GetTicksNS() - sampled_at;
            sample_send_total += elapsed;
            if (elapsed > sample_send_max)
                sample_send_max = elapsed;
            if (++sample_send_count == 600) {
                pc_log_line("net timing: input poll-to-send mean %.3f max %.3f ms at frame %d",
                    sample_send_total / 600000000.0, sample_send_max / 1000000.0, net.frame);
                sample_send_count = 0;
                sample_send_total = sample_send_max = 0;
            }
        }
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
            /* Both peers must enter a scene on the SAME frame; when they do
             * not, the sims are running different code and the checksum
             * says so a frame later (docs/netcode-plan.md section 5.2). Two
             * peers' logs diffed on this line give the gap directly, which
             * is the measurement any fix for it has to move. */
            pc_log_line("net: scene %d -> %d at frame %d", s_scene_last, scene, net.frame);
            s_scene_last = scene;
            int lead = in_fight() ? 10 : IO_QUIET;
            barrier_raise(net.frame + lead);
        }
        /* Predict at most WINDOW frames past the remote, and only in a
         * fight past the barrier (plan §10.4: menus lockstep, rollback
         * armed in GS_VS once the match's own loads have gone quiet).
         *
         * Slow (or fast) motion is lockstep as well. A re-run tick calls
         * lb_80019900 again, and its frame-skip accumulator lives in
         * lb_0195.c, which the snapshot deliberately excludes (melee_state.ld)
         * because rewinding the pad alarm makes it queue catch-up ticks. The
         * accumulator is only idempotent per tick while the tick period
         * equals the step, i.e. at game speed 1.0; Slo-Mo Melee halves it
         * (gmslomo.c sets game_speed 0.5 and declares itself GS_VS), so
         * after a rollback of odd depth the two peers advance the scene on
         * opposite ticks -- one simulates a frame the other skips. */
        bool lockstep = s_lockstep || !in_fight() || net.frame <= net.rb_barrier ||
                        gmVsMelee_StartData.rules.game_speed != 1.0F;
        int32_t need = lockstep ? net.frame : (net.frame >= WINDOW ? net.frame - WINDOW : 0);
        if (!wait_input(need)) {
            return;
        }
        if ((net.frame % SYNC_INTERVAL) == 0 && net.frame > 0) {
            time_sync();
        }
        if (s_remote_have < net.frame) {
            if (!snap_predicted(net.frame) && !wait_input(net.frame)) {
                return;
            }
            if (s_remote_have < net.frame) {
                predict(net.frame);
            }
        }
        /* Re-resolve the slot the next HSD_PadRenewMasterStatus will read.
         * The pointer this tick started with was taken before wait_remote
         * and snap_predicted, and every OSRestoreInterrupts in between
         * delivers the pad alarms that came due during the stall (src/pc/os.c
         * deliver_pending); with the raw queue full and qtype back to 0 those
         * move qread, and the inputs would then be written into a slot no
         * tick ever reads. pad_qtype_hold() keeps qtype at 2 so the cursor
         * cannot move at all; this is the second lock on the same door. */
        head = pad_head();
        write_head(head, net.frame); /* after anything that can run the pad alarm */
    }
    replay_feed(head);

    if (net.start_frame == net.frame && net.hs != HS_FAILED) {
        *HSD_RandSeedPtr = net.seed; /* both peers enter the match from the agreed seed */
    }
    uint32_t ck = frame_checksum(head);
    s_ck_ring[net.frame & (RING - 1)] = ck;
    s_sim_n[net.frame & (RING - 1)] = 1;
    /* No-op unless netplay is running or MELEE_NET_STATE_LOG is set: a solo
     * replay is how a cross-platform divergence is localised. */
    record_state(head, net.frame);
    if (net.active) {
        check_desync(); /* reports and dumps both rings on the first mismatch */
    }
    record_frame(head, ck, net.frame);
    /* Everything the peer has acknowledged can no longer be rolled back, so
     * its staged record is final and can go to the file in frame order. */
    record_confirm(confirmed_frame());

    if ((net.frame % 60) == 0) {
        s_rb_depth_recent = s_rb_depth_cur; /* pc_net_quality: deepest rollback last second */
        s_rb_depth_cur = 0;
        /* Loss over the last second, smoothed over three. Both consumers of
         * this react to a burst -- send_inputs() widens the redundancy and
         * tx_timer() turns the mid-frame resend back on -- so the ten-second
         * window this replaces could answer a burst after it was over.
         *
         * The smoothing is symmetric on purpose. A one-second window is only
         * ~60 acks, so its binomial spread is wide; taking the rise
         * immediately and decaying slowly latches onto the top of that
         * spread instead of its mean, which measured 6% on a link losing 1%
         * and is enough on its own to make pc_net_quality() call a good
         * connection bad. */
        int sent = (int)(net.tx_inputs - s_loss_tx_mark);
        s_loss_tx_mark = net.tx_inputs;
        SDL_LockMutex(s_rx_lock); /* the receive thread counts the acks */
        int acked = (int)(s_rx_acks - s_loss_ack_mark);
        s_loss_ack_mark = s_rx_acks;
        SDL_UnlockMutex(s_rx_lock);
        if (sent > 0) {
            int now_pct = sent > acked ? 100 * (sent - acked) / sent : 0;
            s_loss_pct = (s_loss_pct * 2 + now_pct) / 3;
        }
    }
    if ((net.frame % 600) == 0 && net.frame > 0 && net.active) {
        /* The rx counters are the receive thread's: read and cleared under
         * its lock, which it waits out for one log line every 10 s. */
        SDL_LockMutex(s_rx_lock);
        pc_log_line("net: frame %d, rollbacks %u (max depth %d, lost %u), stalls %u (worst "
                    "%.1f ms), skips %u, advances %u, ping %u ms (avg %.0f, min %u, max %u, "
                    "jitter %.1f), loss %d%% (%u tx %u rx), offset %+.1f ms, remote behind %d, "
                    "barrier %d, quality %d, dup %u reorder %u sock_err %u resim_eat %u red %d, "
                    "drops (bad_src %u bad_sess %u bad_player %u bad_mac %u malformed %u "
                    "would_block %u rx_full %u), "
                    "pad reuse %u empty %u, audio replayed %u over %u, seed out-of-tick %u draws "
                    "in %u frames, pad slips %u (queue worst %u, full at write %u), idle ticks %u, "
                    "audio liveness asked %u (engine would say yes %u)",
            net.frame, s_rollbacks, s_rb_depth_max, s_rb_lost, s_stalls, s_stall_ns_max / 1e6,
            net.skips, s_advances, net.ping_us / 1000,
            s_ping_n ? s_ping_sum / 1000.0 / s_ping_n : 0.0, s_rtt_min / 1000, s_rtt_max / 1000,
            jitter_us() / 1000.0, s_loss_pct, net.tx_pkts, s_rx_pkts, net.offset_last / 1000.0,
            net.frame - 1 - s_remote_have, net.rb_barrier, pc_net_quality(), s_rx_dups,
            s_rx_reorders, s_sock_err, s_resim_eat, s_red_target, s_rx_bad_src, s_rx_bad_sess,
            s_rx_bad_player, s_rx_bad_mac, s_rx_malformed, s_tx_would_block, s_rx_full,
            net.pad_reuse, net.pad_empty, s_aj_replays, s_aj_over, s_seed_out_draws,
            s_seed_out_frames, s_pad_slips, s_qdepth_max, s_pad_full, s_tick_idle, s_deaf_asks,
            s_deaf_true);
        s_stall_ns_max = 0;
        s_ping_sum = 0;
        s_ping_n = 0;
        s_rtt_min = s_rtt_max = 0;
        net.tx_pkts = s_rx_pkts = net.tx_inputs = s_rx_acks = 0;
        s_loss_tx_mark = s_loss_ack_mark = 0; /* the marks index those counters */
        s_rx_dups = s_rx_reorders = s_sock_err = s_resim_eat = 0;
        s_rx_bad_src = s_rx_bad_sess = s_rx_bad_player = s_rx_malformed = s_tx_would_block = 0;
        s_rx_bad_mac = s_rx_full = 0;
        SDL_UnlockMutex(s_rx_lock);
        snap_stats_report();
    }
#ifdef MELEE_FP_PERTURB_NAME
    if ((net.frame % 600) == 0 && net.frame > 0) {
        extern unsigned pc_fp_perturb_calls; /* src/pc/libm/pc_perturb.c */
        pc_log_line("net: fp perturb " MELEE_FP_PERTURB_NAME " fired %u times by frame %d",
            pc_fp_perturb_calls, net.frame);
    }
#endif
    net.tick_frame = net.frame;
    audio_journal_begin(net.tick_frame);
    net.frame++;
}

/* ---- re-simulation audit ----------------------------------------------
 * MELEE_NET_RESIM_AUDIT=K: every AUDIT_EVERY frames, roll back K frames and
 * re-run them from the same input rings. Nothing about the inputs changed,
 * so every byte of the state must come back as it was; a block that does
 * not is the re-simulation diverging from the fresh pass on its own, which
 * is a desync waiting for a peer that re-ran a different set of frames.
 *
 * Each audit re-runs the range twice and reports two comparisons: the first
 * re-run against the first pass, and the second re-run against the first
 * re-run. The pair separates the two faults that both look like "the
 * re-simulation diverged": a tick that is not a pure function of (state,
 * inputs) breaks both, while a first pass that did something a re-run never
 * does -- rendered, loaded, ran a frame twice, read a clock -- breaks only
 * the first. Off unless the variable is set. */
#define AUDIT_EVERY 120
static int s_audit_k;
static Snapshot s_audit_after; /* state after the first pass of the frames */
static Snapshot s_audit_run1;  /* and after the first re-run of them */
static Snapshot s_audit_now;   /* region list of the state being compared */
static bool s_audit_running;
static int s_audit_pass; /* 1 = first re-run, 2 = second */
static int32_t s_audit_from;
static unsigned s_audit_runs, s_audit_ck_bad, s_audit_pure_bad;
static uint32_t s_audit_ck[RING], s_audit_ck1[RING]; /* checksums of pass 1 / re-run 1 */
static char s_audit_state[SNAPS][320]; /* their state lines, for the field-level diff */
static char s_audit_state1[SNAPS][320];
static int32_t s_audit_done = -1; /* frame the last audit ran at */

static void audit_reset(void) {
    memset(&s_audit_after, 0, sizeof s_audit_after);
    memset(&s_audit_run1, 0, sizeof s_audit_run1);
    memset(&s_audit_now, 0, sizeof s_audit_now);
    s_audit_running = false;
    s_audit_pass = 0;
    s_audit_done = -1;
    s_audit_runs = 0;
    s_audit_ck_bad = 0;
    s_audit_pure_bad = 0;
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
#ifdef MELEE_FP_PERTURB_NAME
        /* A determinism-harness build (CMakeLists.txt MELEE_FP_PERTURB): say
         * so on every single run, because it desyncs against every other
         * build by design. src/pc/libm/pc_perturb.c owns the counter. */
        pc_log_line("net: FP PERTURB " MELEE_FP_PERTURB_NAME " +1 ULP (determinism test build, "
                    "never ship)");
#endif
        const char* ea = getenv("MELEE_NET_EXIT_AFTER_FRAMES");
        s_exit_after = ea != NULL ? (int32_t)strtol(ea, NULL, 10) : 0;
        const char* au = getenv("MELEE_NET_RESIM_AUDIT");
        s_audit_k = au != NULL ? atoi(au) : 0;
        if (s_audit_k > SNAPS) {
            s_audit_k = SNAPS; /* one snapshot per predicted frame is all there is */
        }
        const char* po = getenv("MELEE_NET_RESIM_AUDIT_POISON");
        s_audit_poison_want = po != NULL && po[0] == '1';
        if (s_audit_k > 0) {
            pc_log_line("net: resim audit on, %d frames every %d%s (MELEE_NET_RESIM_AUDIT)",
                s_audit_k, AUDIT_EVERY,
                s_audit_poison_want ? ", with a mispredicted pass first" : "");
        }
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
    if (net.active) {
        net_watchdog_arm(); /* this is the thread that must keep moving */
        pad_qtype_hold();   /* HSD_PadInit wipes it; the tick depends on it */
        dvd_settle();
    }
    if (net.active || record_active()) {
        /* Also on the record/replay legs: a recording made with a seed that
         * drifts with the frame rate cannot replay identically anywhere,
         * which is the M0 claim (docs/netcode-plan.md section 5). */
        seed_out_of_tick_check();
    }
    if (net.synctest) {
        synctest_before_tick();
    }
    if (!net.active && !record_active()) {
        net.frame++;
        return;
    }
    /* Every tick must consume a queue entry: HSD_PadRenewMasterStatus renews
     * the game's inputs only when the queue is non-empty, so a tick that
     * finds it empty runs on the previous frame's inputs while write_head
     * and the checksum below say it ran on this frame's -- a divergence on
     * one side only. pc_net_pace_adjust_ns pins one entry there; this counts
     * the invariant (pad_empty must stay 0) and repairs it if some flush got
     * in between. The local sample is read back out of the head only when
     * this present really queued one. */
    PadLibData* p = &HSD_PadLibData;
    bool fresh = p->qcount > 0 && !net.pad_reused;
    if (p->qcount == 0) {
        net.pad_empty++;
    }
    if (net.sync_mode == SYNC_LEGACY) {
        fresh_tick(&p->queue->stat[p->qread * 4], true); /* pre-fix path, net_sync.c */
        return;
    }
    /* SYNC_OFF measures the link and never acts on it (net_sync.c), so it
     * keeps the normal input path: a control run has to differ from the run
     * under test in the time sync alone, not in how pads reach the tick. */
    PADStatus* head = p->qcount > 0 ? &p->queue->stat[p->qread * 4] : unconsume();
    fresh_tick(head != NULL ? head : &p->queue->stat[p->qread * 4], fresh);
}

/* Compare the live state against what an earlier run of the same frames
 * left, by region and by per-frame checksum. `label` names the pair being
 * compared; returns the number of frames whose checksum did not come back. */
static int audit_diff(const Snapshot* want_s, const uint32_t* want_ck,
    const char (*want_state)[320], const char* label) {
    Snapshot* n = &s_audit_now;
    if (!snapshot_take(n, net.frame)) {
        pc_log_line("net: audit could not snapshot the re-run state");
        return -1;
    }
    int blocks = 0, ranges = 0;
    const uint8_t* want = want_s->buf;
    for (int i = 0; i < want_s->nregions; i++) {
        const Region* r = &want_s->regions[i];
        const Region* q = i < n->nregions ? &n->regions[i] : NULL;
        if (q == NULL || q->ptr != r->ptr || q->len != r->len) {
            /* Report and skip: a heap whose extent moved says nothing about
             * the regions after it, and stopping here hid them. */
            pc_log_line("net: audit f%d+%d %s region %s moved (%p/%zu -> %p/%zu)", s_audit_from,
                s_audit_k, label, r->name, r->ptr, r->len, q ? q->ptr : NULL, q ? q->len : 0);
            want += r->len;
            continue;
        }
        /* Runs of adjacent differing 16-byte blocks, merged: a re-run
         * differs in a few hundred blocks that belong to a handful of
         * objects, and one line per object is what `nm` can be pointed at. */
        size_t run_from = 0;
        bool in_run = false;
        for (size_t off = 0; off <= r->len; off += 16) {
            size_t len = off < r->len ? (r->len - off < 16 ? r->len - off : 16) : 0;
            bool diff = len != 0 && memcmp((const uint8_t*)r->ptr + off, want + off, len) != 0;
            if (diff) {
                blocks++;
                if (!in_run) {
                    in_run = true;
                    run_from = off;
                }
            } else if (in_run) {
                in_run = false;
                ranges++;
                if (ranges <= 40) {
                    pc_log_line("net: audit f%d+%d %s differs %s+0x%zx..0x%zx at %p (%zu bytes)",
                        s_audit_from, s_audit_k, label, r->name, run_from, off,
                        (const uint8_t*)r->ptr + run_from, off - run_from);
                }
            }
        }
        want += r->len;
    }
    /* The blocks above include state no checksum looks at (the HUD, the
     * debug font, render-phase scratch). What decides a desync is whether
     * the re-run reproduced each frame's checksum: inputs, the RNG seed and
     * every fighter's position, facing, percent, action and stocks. */
    int ck_bad = 0;
    for (int32_t f = s_audit_from; f < net.frame; f++) {
        if (s_ck_ring[f & (RING - 1)] != want_ck[f & (RING - 1)]) {
            if (ck_bad < 4) {
                pc_log_line("net: audit f%d %s checksum %08x -> %08x", f, label,
                    want_ck[f & (RING - 1)], s_ck_ring[f & (RING - 1)]);
                /* Which field moved: the state line the earlier run wrote
                 * against the one this run wrote (net_snapshot.c). */
                pc_log_line("net: audit f%d was %s", f, want_state[(f - s_audit_from) % SNAPS]);
                pc_log_line("net: audit f%d now %s", f, state_line(f));
            }
            ck_bad++;
        }
    }
    /* A branch that only changed how many random numbers the frame drew is
     * the commonest shape of an unfaithful re-run: same positions, a seed a
     * few draws along. */
    uint32_t seed_was = want_s->seed_val, seed_now = *HSD_RandSeedPtr;
    if (seed_was != seed_now) {
        pc_log_line("net: audit f%d+%d %s seed %08x -> %08x (%d draws on)", s_audit_from, s_audit_k,
            label, seed_was, seed_now, seed_steps(seed_was, seed_now));
    }
    pc_log_line("net: audit f%d+%d %s: %d blocks in %d ranges differ, %d checksums differ, "
                "seed %s",
        s_audit_from, s_audit_k, label, blocks, ranges, ck_bad,
        seed_was == seed_now ? "same" : "MOVED");
    return ck_bad;
}

/* The discarded mispredicted pass is done: drop the wrong input and re-run
 * the range with the true one, which is exactly what a real rollback does. */
static bool audit_after_poison(void) {
    s_audit_poison = false;
    s_audit_pass = 1;
    return rollback_to(s_audit_from);
}

/* The first re-run is done: compare it with the first pass, then keep its
 * own result and re-run the range a second time. Returns true when that
 * second re-run is under way (the caller must let the frame loop tick). */
static bool audit_after_run1(void) {
    int ck_bad = audit_diff(&s_audit_after, s_audit_ck, s_audit_state, "vs pass1");
    if (ck_bad > 0) {
        s_audit_ck_bad++;
    }
    /* The same range again, from the same snapshot: whatever differs
     * between two re-runs is the tick itself reading something outside the
     * state, and whatever differs only against pass 1 is something the
     * first pass did that a re-run does not. */
    if (!snapshot_take(&s_audit_run1, net.frame)) {
        return false;
    }
    memcpy(s_audit_ck1, s_ck_ring, sizeof s_audit_ck1);
    for (int i = 0; i < s_audit_k; i++) {
        snprintf(s_audit_state1[i], sizeof s_audit_state1[0], "%s", state_line(s_audit_from + i));
    }
    s_audit_pass = 2;
    return rollback_to(s_audit_from);
}

static void audit_after_run2(void) {
    int ck_bad = audit_diff(&s_audit_run1, s_audit_ck1, s_audit_state1, "vs re-run1");
    if (ck_bad > 0) {
        s_audit_pure_bad++;
    }
    s_audit_runs++;
    pc_log_line("net: audit f%d+%d done (%u first-pass / %u re-run mismatches of %u audits)",
        s_audit_from, s_audit_k, s_audit_ck_bad, s_audit_pure_bad, s_audit_runs);
}

bool pc_net_after_tick(bool scene_ending) {
    s_aj_on = false; /* the tick is over: later audio queries are not its own */
    head_check();    /* did this tick consume the inputs write_head wrote? */
    if (net.synctest) {
        return synctest_after_tick();
    }
    if (!net.active) {
        /* The record/replay leg never reaches the arming at the bottom, so
         * without this s_seed_have stays false and the check above is a
         * permanent no-op there -- a recording whose seed was stirred by the
         * wall clock could never replay identically anywhere, which is the
         * whole M0 claim (docs/netcode-plan.md section 5). */
        if (record_active()) {
            s_seed_after_tick = *HSD_RandSeedPtr;
            s_seed_have = true;
        }
        return false;
    }
    recv_inputs();
    if (s_rb_frame >= 0) {
        int32_t f = s_rb_frame;
        s_rb_frame = -1;
        if (rollback_to(f)) {
            s_resim_run = 0;
            return true;
        }
    }
    if (net.resim) {
        if (net.tick_frame + 1 < net.frame) {
            s_resim_run++;
            return resim_prepare(net.tick_frame + 1);
        }
        net.resim = false;
        resim_note(s_resim_run + 1); /* the tick that just ran was a re-run too */
        s_resim_run = 0;
        if (s_audit_running) {
            bool more = s_audit_pass == 0 ? audit_after_poison() :
                        s_audit_pass == 1 ? audit_after_run1() :
                                            (audit_after_run2(), false);
            if (more) {
                return true; /* the next pass of this audit is running */
            }
            s_audit_running = false;
            s_audit_poison = false;
        }
    }
    /* The audit's own rollback: same rings, so the re-run must reproduce the
     * state it replaces (MELEE_NET_RESIM_AUDIT). */
    if (s_audit_k > 0 && !s_audit_running && s_rb_frame < 0 && in_fight() &&
        (net.frame % AUDIT_EVERY) == 0 && net.frame != s_audit_done)
    {
        int32_t f = net.frame - s_audit_k;
        Snapshot* s = f > 0 ? snap_slot(f) : NULL;
        if (s != NULL && s->frame == f && f > net.rb_barrier && snapshot_unusable(s) == NULL &&
            snapshot_take(&s_audit_after, net.frame))
        {
            memcpy(s_audit_ck, s_ck_ring, sizeof s_audit_ck);
            for (int i = 0; i < s_audit_k; i++) {
                snprintf(s_audit_state[i], sizeof s_audit_state[0], "%s", state_line(f + i));
            }
            s_audit_from = f;
            s_audit_done = net.frame;
            s_audit_running = true;
            s_audit_pass = s_audit_poison_want ? 0 : 1;
            s_audit_poison = s_audit_poison_want;
            if (rollback_to(f)) {
                return true;
            }
            s_audit_running = false;
            pc_log_line("net: audit could not roll back to frame %d", f);
        }
    }
    check_desync();
    handshake_direct();
    if (net.direct && net.hs == HS_FAILED) {
        /* A direct session has no lobby to report a refusal to, so it ends
         * itself. Whichever check failed has already logged why; what this
         * adds is that the session is over rather than about to play on and
         * desync a minute later. INCOMPATIBLE rides the BYE, so the peer
         * reports the same class even when its own side of the exchange was
         * still waiting. */
        s_status = PC_NET_PEER_INCOMPATIBLE;
        pc_log_line("net: refusing this session at frame %d, the peers did not agree on the "
                    "match parameters",
            net.frame);
        pc_net_disconnect();
        return false;
    }
    if (!scene_ending && net.advance_left > 0 && (net.frame % 5) == 0) {
        /* Behind the peer: one extra tick this present, once per 5 frames. */
        PADStatus* head = unconsume();
        if (head != NULL) {
            net.advance_left--;
            s_advances++;
            fresh_tick(head, false);
            return true;
        }
    }
    s_seed_after_tick = *HSD_RandSeedPtr; /* seed_out_of_tick_check reads it next present */
    s_seed_have = true;
    return false;
}
