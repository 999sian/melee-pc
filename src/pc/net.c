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
 *   MELEE_NET_DELAY=n            input delay in frames (default 2)
 *   MELEE_NET_SIM_LOSS=percent   drop that share of outgoing packets
 *   MELEE_NET_SIM_DELAY_MS=ms    hold every outgoing packet that long
 *   MELEE_NET_HANDSHAKE_TEST=1   run the lobby handshake at frame 300 without a lobby
 */
#include "compat.h"
#include "pc/net.h"
#include "pc/pc.h"

#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/synth.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/ft/fighter.h>
#include <melee/ft/inlines.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/types.h>
#include <melee/pl/player.h>
#pragma GCC diagnostic pop
#include <xxhash.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define getpid _getpid
static void sock_nonblock(sock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
static void sock_nonblock(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#define sock_close close
#endif

/* ---- wire format ------------------------------------------------------ */

#define RING 64            /* frames of history kept per side; power of two */
#define REDUNDANCY 16      /* unacked frames repeated in every input packet */
#define WINDOW 7           /* predicted frames allowed before a hard stall */
#define SNAPS 8            /* snapshot ring, one per predicted frame; > WINDOW */
#define FRAME_US 16683
#define STALL_TIMEOUT_MS 7000
#define CONNECT_TIMEOUT_MS 60000
#define SYNC_INTERVAL 30   /* frames between time-sync decisions (Slippi) */
#define OFFSET_SAMPLES 30

/* 8-byte pad, same fields Slippi puts on the wire. */
typedef struct WirePad {
    uint16_t button;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerLeft, triggerRight;
} WirePad;

typedef struct Packet {
    uint8_t magic;         /* 'M' */
    uint8_t player;
    int32_t newest;        /* newest local frame the sender holds */
    int32_t first;         /* frame of pads[0]; pads[i] is frame first+i */
    int32_t ck_frame;      /* frame the checksum was taken before */
    uint32_t ck;
    uint32_t send_time_us; /* sender clock, echoed back in the ack for RTT */
    uint8_t count;
    WirePad pads[REDUNDANCY];
} __attribute__((packed)) Packet;

typedef struct Ack {
    uint8_t magic;         /* 'A' */
    uint8_t player;
    int32_t frame;         /* newest contiguous frame the sender now holds */
    uint32_t echo_time_us; /* send_time_us of the packet being acked */
} __attribute__((packed)) Ack;

/* Reliable lobby message (stop-and-wait, see "reliable channel" below). */
#define REL_MAX 256
typedef struct Rel {
    uint8_t magic;         /* 'R' */
    uint8_t player;
    uint8_t seq;
    uint8_t type;          /* < 0x10 handled here (handshake), else for the caller */
    uint16_t len;
    uint8_t payload[REL_MAX];
} __attribute__((packed)) Rel;

typedef struct RelAck {
    uint8_t magic;         /* 'K' */
    uint8_t player;
    uint8_t seq;
} __attribute__((packed)) RelAck;

/* ---- state ------------------------------------------------------------ */

static bool s_active;
static sock_t s_sock = SOCK_INVALID;
static struct sockaddr_storage s_peer;
static socklen_t s_peer_len;
static int s_local, s_remote, s_delay;
static int32_t s_frame;                  /* next fresh frame to simulate */
static int32_t s_tick_frame = -1;        /* frame the last prepared tick simulates */
static bool s_resim;                     /* re-running frames after a rollback */
static WirePad s_local_ring[RING];       /* indexed by frame & (RING-1) */
static WirePad s_remote_ring[RING];      /* real input, or the prediction in use */
static int32_t s_remote_have = -1;       /* newest contiguous real remote frame */
static int32_t s_remote_newest = -1;     /* newest frame the peer reported holding */
static int32_t s_last_acked = -1;        /* newest local frame the peer holds */
static int32_t s_rb_frame = -1;          /* oldest mispredicted frame not rolled back yet */
static int32_t s_remote_ck_frame = -1;
static uint32_t s_remote_ck;
static uint32_t s_ck_ring[RING];         /* our checksum entering each frame */
static bool s_desync_reported;
static bool s_heard;                     /* any packet from the peer yet */
static PADStatus s_raw_last;             /* newest physical sample (port 0) */

static unsigned s_stalls, s_skips, s_advances, s_rollbacks, s_rb_lost;
static int s_rb_depth_max;
static uint64_t s_stall_ns_max;
static uint32_t s_ping_us;               /* smoothed RTT */
static uint64_t s_ping_sum;
static unsigned s_ping_n;

/* Time sync, copied from Slippi (SlippiNetplay.cpp CalcTimeOffsetUs):
 * positive offset = we run ahead of the peer. */
static int32_t s_offset[OFFSET_SAMPLES];
static int s_offset_n, s_offset_i;
static int32_t s_offset_last;
static int s_skip_left, s_advance_left;
static uint64_t s_send_ns;               /* when the newest local frame first went out */
static int32_t s_send_frame = -1;

static unsigned s_io_count;              /* game-thread disc requests issued so far */
static int32_t s_io_frame = -1000;       /* frame of the newest one */
static SDL_ThreadID s_game_thread;

/* Outgoing side: the game thread and a 4 ms SDL timer (mid-frame resend +
 * release of held packets + reliable retransmit) share the socket under
 * s_tx_lock. s_active flips under it too, so the timer never sends on a
 * closed socket. */
static SDL_Mutex* s_tx_lock;
static SDL_TimerID s_timer;
static Packet s_last_pkt;
static bool s_last_valid;
static uint64_t s_last_send_ns;
static int s_sim_loss;                   /* percent of outgoing packets dropped */
static uint64_t s_sim_delay_ns;

typedef struct Held {
    uint64_t release_ns;
    uint16_t len;
    uint8_t buf[sizeof(Rel)];
} Held;
#define HELD_MAX 128
static Held s_held[HELD_MAX];
static int s_held_head, s_held_n;

/* Reliable channel: one 'R' in flight, resent every 250 ms until its 'K'
 * arrives, 4 queued behind it; 4 received messages wait for the caller.
 * Transmit side under s_tx_lock, receive side game thread only. */
#define REL_QUEUE 4
#define REL_RESEND_NS 250000000ull
#define REL_RULES 0x01               /* host -> guest {seed, start_frame} */
#define REL_READY 0x02               /* guest -> host */

typedef struct RelMsg {
    uint8_t type;
    uint16_t len;
    uint8_t payload[REL_MAX];
} RelMsg;

static RelMsg s_rel_tx[REL_QUEUE];      /* [s_rel_tx_head] is the one in flight */
static int s_rel_tx_head, s_rel_tx_n;
static uint8_t s_rel_seq;               /* seq of the message in flight */
static uint64_t s_rel_sent_ns;          /* 0: not sent yet */
static int s_rel_resends;
static RelMsg s_rel_rx[REL_QUEUE];
static int s_rel_rx_head, s_rel_rx_n;
static uint8_t s_rel_expect;            /* next seq accepted */

/* Match handshake: RULES host -> guest, READY back. RULES carries the seed,
 * start_frame (the frame the lobby leaves for the CSS on both peers; each
 * side applies the seed when it learns it and again entering that frame,
 * and frame checksums are only compared from it on, since the two lobbies
 * run different states until then) and everything match-affecting from
 * plan §5 item 7: the memcard GameRules, the item/stage switches from
 * GamePrefs and the frozen-stadium toggle. The guest overwrites its copies
 * (restored at disconnect) so CSS/SSS/match read the same values on both
 * peers; unlock-all is forced on for both (pc_net_rules). */
enum { HS_IDLE, HS_PENDING, HS_DONE, HS_FAILED };
#define HS_TIMEOUT_MS 15000
#define HS_LEAD_FRAMES 120   /* ponytail: 2 s for READY; a slower link misses the start */
static int s_hs;
static bool s_hs_host;
static uint64_t s_hs_t0;
static uint32_t s_seed;                 /* agreed RNG seed (0: none) */
static int32_t s_start_frame = -1;
static int32_t s_ck_from;               /* checksums before this frame are not compared */

typedef struct Rules {
    uint32_t seed;
    int32_t start_frame;
    GameRules game;
    uint8_t item_freq;
    uint64_t item_mask;
    uint32_t stage_mask;
    uint8_t frozen_stadium;
} __attribute__((packed)) Rules;

static bool s_rules_on;                 /* a RULES set is in force (host or guest) */
static bool s_rules_frozen;
static bool s_rules_saved;              /* guest: s_rules_orig holds its own values */
static Rules s_rules_orig;

/* ---- helpers ---------------------------------------------------------- */

static int8_t at_rest(int8_t v) {
    /* Slippi's clamp: idle stick noise must not look like a new input. */
    return v >= -2 && v <= 2 ? 0 : v;
}

static void to_wire(WirePad* w, const PADStatus* p) {
    w->button = p->button;
    w->stickX = at_rest(p->stickX);
    w->stickY = at_rest(p->stickY);
    w->substickX = at_rest(p->substickX);
    w->substickY = at_rest(p->substickY);
    w->triggerLeft = p->triggerLeft;
    w->triggerRight = p->triggerRight;
}

static void from_wire(PADStatus* p, const WirePad* w) {
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

static uint32_t fnv1a(uint32_t h, const void* data, size_t n) {
    const uint8_t* b = data;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* Disc requests issued by the game thread. The audio thread streams music
 * through the same path into ARAM and its own statics, none of which a
 * snapshot covers, so those do not count. */
void pc_net_note_io(void) {
    if (SDL_GetCurrentThreadID() != s_game_thread) {
        return;
    }
    s_io_count++;
    s_io_frame = s_frame;
}

int aurora_dvd_inflight(void);
int aurora_arq_inflight(void);

/* True when a game-thread disc or ARAM transfer may still be pending on a
 * worker thread: its completion writes into game memory whenever it lands,
 * so no snapshot may be taken or restored until it has. The in-flight
 * counters also see the music stream, so they are only believed for a
 * while after the game thread itself asked for something. */
static bool io_inflight(void) {
    return s_frame - s_io_frame < 120 && (aurora_dvd_inflight() > 0 || aurora_arq_inflight() > 0);
}

extern struct GameSceneInfo* gm_804D6720; /* current scene, gmscene.c */

static bool in_fight(void) {
    return gm_804D6720 != NULL &&
           (gm_804D6720->scene_kind == GS_VS || gm_804D6720->scene_kind == GS_SUDDEN_DEATH);
}

/* Newest frame ticked in the current timeline. */
static int32_t simulated_upto(void) {
    return s_resim ? s_tick_frame : s_frame - 1;
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

/* Every outgoing datagram passes here (caller holds s_tx_lock). The loss and
 * delay simulators replace `tc netem`, which needs root. */
static void tx(const void* buf, size_t len) {
    if (s_sim_loss > 0 && rand() % 100 < s_sim_loss) {
        return;
    }
    if (s_sim_delay_ns == 0) {
        sendto(s_sock, (const char*) buf, len, 0, (struct sockaddr*) &s_peer, s_peer_len);
        return;
    }
    if (s_held_n == HELD_MAX) {
        return; /* simulator queue full: one more loss */
    }
    Held* h = &s_held[(s_held_head + s_held_n++) % HELD_MAX];
    h->release_ns = SDL_GetTicksNS() + s_sim_delay_ns;
    h->len = (uint16_t) len;
    memcpy(h->buf, buf, len);
}

/* Release held packets whose simulated delay has passed (caller holds s_tx_lock). */
static void tx_flush(void) {
    uint64_t now = SDL_GetTicksNS();
    while (s_held_n > 0 && s_held[s_held_head].release_ns <= now) {
        Held* h = &s_held[s_held_head];
        sendto(s_sock, (const char*) h->buf, h->len, 0, (struct sockaddr*) &s_peer, s_peer_len);
        s_held_head = (s_held_head + 1) % HELD_MAX;
        s_held_n--;
    }
}

/* Send or resend the reliable message in flight (caller holds s_tx_lock). */
static void rel_service(void) {
    if (s_rel_tx_n == 0) {
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    if (s_rel_sent_ns != 0 && now - s_rel_sent_ns < REL_RESEND_NS) {
        return;
    }
    const RelMsg* m = &s_rel_tx[s_rel_tx_head];
    Rel r = { 'R', (uint8_t) s_local, s_rel_seq, m->type, m->len, { 0 } };
    memcpy(r.payload, m->payload, m->len);
    tx(&r, offsetof(Rel, payload) + m->len);
    if (s_rel_sent_ns != 0) {
        s_rel_resends++;
        pc_log_line("net: reliable seq %u type %02x len %u resend #%d", s_rel_seq, m->type,
                    m->len, s_rel_resends);
    }
    s_rel_sent_ns = now;
}

/* Slippi's second send: the newest input packet goes out again mid-frame,
 * so a lost packet costs half a frame instead of a whole one. Runs on SDL's
 * timer thread; it only touches the frozen packet copy, the held queue and
 * the reliable transmit queue. Removes itself once the session is gone. */
static Uint32 SDLCALL tx_timer(void* ud, SDL_TimerID id, Uint32 interval) {
    (void) ud;
    (void) id;
    SDL_LockMutex(s_tx_lock);
    if (!s_active) {
        s_timer = 0;
        SDL_UnlockMutex(s_tx_lock);
        return 0;
    }
    tx_flush();
    rel_service();
    uint64_t now = SDL_GetTicksNS();
    if (s_last_valid && now - s_last_send_ns >= 7000000ull) {
        s_last_pkt.send_time_us = (uint32_t) (now / 1000);
        tx(&s_last_pkt, packet_len(&s_last_pkt));
        s_last_send_ns = now;
    }
    SDL_UnlockMutex(s_tx_lock);
    return interval;
}

static void send_inputs(void) {
    Packet pk;
    memset(&pk, 0, sizeof pk);
    pk.magic = 'M';
    pk.player = (uint8_t) s_local;
    /* The local sample taken at frame f is the input for frame f + delay. */
    int32_t newest = s_frame + s_delay;
    /* Everything the peer has not acked yet, oldest first so its contiguous
     * mark can always advance; the cap only bites when acks are starved. */
    int32_t first = s_last_acked + 1;
    if (first < 0) {
        first = 0;
    }
    if (first <= newest - RING) {
        first = newest - RING + 1;
    }
    int32_t count = newest - first + 1;
    if (count > REDUNDANCY) {
        count = REDUNDANCY;
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
    pk.send_time_us = (uint32_t) (now / 1000);
    SDL_LockMutex(s_tx_lock);
    tx_flush();
    rel_service();
    tx(&pk, packet_len(&pk));
    s_last_pkt = pk;
    s_last_valid = true;
    s_last_send_ns = now;
    SDL_UnlockMutex(s_tx_lock);
    if (newest != s_send_frame) {
        s_send_frame = newest;
        s_send_ns = now;
    }
}

static void send_ack(int32_t frame, uint32_t echo) {
    Ack a = { 'A', (uint8_t) s_local, frame, echo };
    SDL_LockMutex(s_tx_lock);
    tx(&a, sizeof a);
    SDL_UnlockMutex(s_tx_lock);
}

/* ---- receive ---------------------------------------------------------- */

static void on_inputs(const Packet* pk, int n) {
    int count = pk->count > REDUNDANCY ? REDUNDANCY : pk->count;
    if (n < (int) (offsetof(Packet, pads) + count * sizeof(WirePad))) {
        return;
    }
    int32_t last = pk->first + count - 1;
    /* Only contiguous data is taken; the ack below tells the peer what to
     * resend, so a gap never has to be tracked. */
    if (count > 0 && pk->first <= s_remote_have + 1 && last > s_remote_have &&
        last < s_frame + RING / 2) {
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
    send_ack(s_remote_have, pk->send_time_us);
    if (pk->newest > s_remote_newest) {
        s_remote_newest = pk->newest;
        if (s_send_frame >= 0 && s_ping_n > 0) {
            /* Clocks are not shared, so the peer's send time is estimated as
             * now - ping/2 (Slippi); the frame difference is in our units. */
            int64_t now_us = (int64_t) (SDL_GetTicksNS() / 1000);
            int64_t their_send_us = now_us - s_ping_us / 2;
            int64_t off = their_send_us - (int64_t) (s_send_ns / 1000) +
                          (int64_t) FRAME_US * (s_send_frame - pk->newest);
            s_offset[s_offset_i] = (int32_t) off;
            s_offset_i = (s_offset_i + 1) % OFFSET_SAMPLES;
            if (s_offset_n < OFFSET_SAMPLES) {
                s_offset_n++;
            }
        }
    }
}

static void on_ack(const Ack* a) {
    if (a->frame > s_last_acked) {
        s_last_acked = a->frame;
    }
    uint32_t rtt = (uint32_t) (SDL_GetTicksNS() / 1000) - a->echo_time_us;
    if (rtt < 5000000u) {
        s_ping_us = s_ping_us == 0 ? rtt : (s_ping_us * 3 + rtt) / 4;
        s_ping_sum += rtt;
        s_ping_n++;
    }
}

/* ---- reliable channel ------------------------------------------------- */

bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    if (!s_active || len < 0 || len > REL_MAX) {
        return false;
    }
    SDL_LockMutex(s_tx_lock);
    bool ok = s_rel_tx_n < REL_QUEUE;
    if (ok) {
        RelMsg* m = &s_rel_tx[(s_rel_tx_head + s_rel_tx_n++) % REL_QUEUE];
        m->type = type;
        m->len = (uint16_t) len;
        if (len > 0) {
            memcpy(m->payload, payload, (size_t) len);
        }
        rel_service();
    }
    SDL_UnlockMutex(s_tx_lock);
    return ok;
}

int pc_net_recv_reliable(uint8_t* type, void* payload, int max) {
    if (s_rel_rx_n == 0) {
        return -1;
    }
    RelMsg* m = &s_rel_rx[s_rel_rx_head];
    int n = m->len > max ? max : m->len;
    *type = m->type;
    memcpy(payload, m->payload, (size_t) n);
    s_rel_rx_head = (s_rel_rx_head + 1) % REL_QUEUE;
    s_rel_rx_n--;
    return n;
}

static void hs_done(void) {
    s_hs = HS_DONE;
    s_ck_from = s_start_frame;
    s_desync_reported = false; /* anything before start_frame was the lobbies differing */
    pc_log_line("net: handshake done seed=%u start_frame=%d (frame %d)", s_seed, s_start_frame,
                s_tick_frame);
}

/* The match-affecting part of RULES, from (capture) or into (apply) the
 * game's own copies. */
static void rules_capture(Rules* ru) {
    const struct GamePrefs* p = gmMainLib_GetGamePrefs();
    ru->game = *gmMainLib_GetGameRules();
    ru->item_freq = p->item_freq;
    ru->item_mask = p->item_mask;
    ru->stage_mask = p->stage_mask;
    ru->frozen_stadium = pc_is_frozen_stadium_enabled();
}

static void rules_apply(const Rules* ru, bool from_peer) {
    struct GamePrefs* p = gmMainLib_GetGamePrefs();
    if (from_peer && !s_rules_saved) {
        rules_capture(&s_rules_orig);
        s_rules_saved = true;
    }
    *gmMainLib_GetGameRules() = ru->game;
    p->item_freq = ru->item_freq;
    p->item_mask = ru->item_mask;
    p->stage_mask = ru->stage_mask;
    s_rules_frozen = ru->frozen_stadium != 0;
    s_rules_on = true;
    pc_log_line("net: RULES %s mode=%u time=%u stock=%u handicap=%u dmg=%u stage_sel=%u ff=%u "
                "pause=%u sd=%u items=%u/%016llx stages=%08x frozen=%u unlock_all=1",
                from_peer ? "applied" : "in force", ru->game.mode, ru->game.time_limit,
                ru->game.stock_count, ru->game.handicap, ru->game.damage_ratio,
                ru->game.stage_sel, ru->game.friendly_fire, ru->game.pause, ru->game.unk_xc,
                ru->item_freq, (unsigned long long) ru->item_mask, ru->stage_mask,
                ru->frozen_stadium);
}

static void rules_restore(void) {
    if (s_rules_saved) {
        s_rules_saved = false;
        rules_apply(&s_rules_orig, false);
        pc_log_line("net: RULES restored own settings");
    }
    s_rules_on = false;
}

bool pc_net_rules(bool* unlock_all, bool* frozen_stadium) {
    if (!s_rules_on) {
        return false;
    }
    *unlock_all = true;
    *frozen_stadium = s_rules_frozen;
    return true;
}

/* Reliable types below 0x10: the match handshake. */
static void handshake_msg(uint8_t type, const uint8_t* payload, int len) {
    if (type == REL_RULES && len == (int) sizeof(Rules)) {
        Rules ru;
        memcpy(&ru, payload, sizeof ru);
        s_seed = ru.seed;
        s_start_frame = ru.start_frame;
        *HSD_RandSeedPtr = s_seed;
        rules_apply(&ru, true);
        if (s_start_frame <= s_tick_frame) {
            pc_log_line("net: RULES late, start_frame %d already passed (frame %d)", s_start_frame,
                        s_tick_frame);
        }
        if (!pc_net_send_reliable(REL_READY, NULL, 0)) {
            pc_log_line("net: READY not queued, reliable queue full");
        }
        hs_done();
    } else if (type == REL_READY && s_hs == HS_PENDING && s_hs_host) {
        hs_done();
    }
}

static void on_rel(const Rel* r, int n) {
    if (r->len > REL_MAX || n < (int) (offsetof(Rel, payload) + r->len)) {
        return;
    }
    if (r->seq == s_rel_expect) {
        if (r->type < 0x10) {
            handshake_msg(r->type, r->payload, r->len);
        } else if (s_rel_rx_n < REL_QUEUE) {
            RelMsg* m = &s_rel_rx[(s_rel_rx_head + s_rel_rx_n++) % REL_QUEUE];
            m->type = r->type;
            m->len = r->len;
            memcpy(m->payload, r->payload, r->len);
        } else {
            return; /* caller is not draining: no ack, the peer resends later */
        }
        s_rel_expect++;
    } else if (r->seq != (uint8_t) (s_rel_expect - 1)) {
        return; /* neither the next one nor a repeat of the last */
    }
    RelAck k = { 'K', (uint8_t) s_local, r->seq };
    SDL_LockMutex(s_tx_lock);
    tx(&k, sizeof k);
    SDL_UnlockMutex(s_tx_lock);
}

static void on_rel_ack(const RelAck* k) {
    SDL_LockMutex(s_tx_lock);
    if (s_rel_tx_n > 0 && k->seq == s_rel_seq) {
        s_rel_tx_head = (s_rel_tx_head + 1) % REL_QUEUE;
        s_rel_tx_n--;
        s_rel_seq++;
        s_rel_sent_ns = 0;
        s_rel_resends = 0;
        rel_service(); /* next queued message goes out at once */
    }
    SDL_UnlockMutex(s_tx_lock);
}

static void recv_inputs(void) {
    for (;;) {
        union {
            Packet pk;
            Ack ack;
            Rel rel;
            RelAck rack;
        } u;
        int n = (int) recvfrom(s_sock, (char*) &u, sizeof u, 0, NULL, NULL);
        if (n <= 0) {
            return;
        }
        if (n < 2 || u.pk.player != s_remote) {
            continue;
        }
        if (n >= (int) offsetof(Packet, pads) && u.pk.magic == 'M') {
            on_inputs(&u.pk, n);
        } else if (n == (int) sizeof(Ack) && u.ack.magic == 'A') {
            on_ack(&u.ack);
        } else if (n >= (int) offsetof(Rel, payload) && u.rel.magic == 'R') {
            on_rel(&u.rel, n);
        } else if (n == (int) sizeof(RelAck) && u.rack.magic == 'K') {
            on_rel_ack(&u.rack);
        } else {
            continue;
        }
        s_heard = true;
    }
}

static void check_desync(void) {
    if (s_desync_reported || s_hs == HS_PENDING || s_remote_ck_frame < s_ck_from ||
        s_remote_ck_frame > confirmed_frame() || s_remote_ck_frame <= s_frame - RING) {
        return;
    }
    uint32_t mine = s_ck_ring[s_remote_ck_frame & (RING - 1)];
    if (mine != s_remote_ck) {
        s_desync_reported = true;
        pc_log_line("net: DESYNC at frame %d (local %08x remote %08x)", s_remote_ck_frame, mine,
                    s_remote_ck);
    }
}

/* Block until the remote input for `need` is here. Returns false on timeout. */
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
        uint64_t now = SDL_GetTicksNS();
        uint64_t limit = (s_heard ? STALL_TIMEOUT_MS : CONNECT_TIMEOUT_MS) * 1000000ull;
        if (now - t0 > limit) {
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
    return true;
}

/* ---- time sync -------------------------------------------------------- */

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

static void time_sync(void) {
    s_offset_last = offset_us();
    if (s_offset_last > 10000 && s_skip_left == 0) {
        s_skip_left = (s_offset_last - 10000) / FRAME_US + 1;
        if (s_skip_left > 5) {
            s_skip_left = 5;
        }
    } else if (s_offset_last < -26683 && s_advance_left == 0) {
        s_advance_left = -s_offset_last / FRAME_US;
        if (s_advance_left > 3) {
            s_advance_left = 3;
        }
    }
}

/* Stall one frame. The sim loop ticks once per queued pad sample and the
 * 1/60 s pad alarm is wall-clock, so merely sleeping would be paid back as an
 * extra tick: let the period pass, deliver the alarm, drop what it queued. */
static void skip_frame(void) {
    PadLibData* p = &HSD_PadLibData;
    SDL_DelayNS(FRAME_US * 1000ull);
    uint8_t before = p->qcount;
    OSRestoreInterrupts(OSDisableInterrupts()); /* delivers due alarms */
    for (int grew = p->qcount - before; grew > 0; grew--) {
        p->qwrite = (uint8_t) ((p->qwrite + p->qnum - 1) % p->qnum);
        p->qcount--;
    }
    s_skips++;
}

/* ---- public ----------------------------------------------------------- */

bool pc_net_active(void) {
    return s_active;
}

int pc_net_local_player(void) {
    return s_local;
}

static void snaps_invalidate(void);

/* Back to frame 0 with empty rings; called with the timer parked (s_active
 * false), so only the game thread is looking. */
static bool s_seen_remote;

static void session_reset(void) {
    s_seen_remote = false;
    memset(s_local_ring, 0, sizeof s_local_ring);
    memset(s_remote_ring, 0, sizeof s_remote_ring);
    memset(s_ck_ring, 0, sizeof s_ck_ring);
    s_frame = 0;
    s_tick_frame = -1;
    s_resim = false;
    s_remote_have = s_remote_newest = s_last_acked = s_rb_frame = s_remote_ck_frame = -1;
    s_remote_ck = 0;
    s_desync_reported = s_heard = false;
    s_stalls = s_skips = s_advances = s_rollbacks = s_rb_lost = 0;
    s_rb_depth_max = 0;
    s_stall_ns_max = 0;
    s_ping_us = 0;
    s_ping_sum = 0;
    s_ping_n = 0;
    s_offset_n = s_offset_i = 0;
    s_offset_last = 0;
    s_skip_left = s_advance_left = 0;
    s_send_ns = 0;
    s_send_frame = -1;
    s_io_frame = -1000;
    s_last_valid = false;
    s_held_head = s_held_n = 0;
    s_rel_tx_head = s_rel_tx_n = s_rel_rx_head = s_rel_rx_n = 0;
    s_rel_seq = s_rel_expect = 0;
    s_rel_sent_ns = 0;
    s_rel_resends = 0;
    s_hs = HS_IDLE;
    s_hs_host = false;
    s_seed = 0;
    s_start_frame = -1;
    s_ck_from = 0;
    snaps_invalidate();
}

void pc_net_disconnect(void) {
    if (s_sock == SOCK_INVALID) {
        return;
    }
    SDL_LockMutex(s_tx_lock);
    s_active = false;
    sock_close(s_sock);
    s_sock = SOCK_INVALID;
    s_hs = HS_IDLE;
    rules_restore();
    HSD_PadLibData.qtype = 0;
    SDL_UnlockMutex(s_tx_lock);
    /* The timer parks itself when it sees !s_active; removing it here makes
     * that synchronous so nothing can touch the socket after this returns. */
    if (s_timer) {
        SDL_RemoveTimer(s_timer);
        s_timer = 0;
    }
    pc_log_line("net: disconnected at frame %d", s_tick_frame);
}

bool pc_net_connect(const char* ip, uint16_t port, int player, uint32_t seed) {
    if (s_tx_lock == NULL) {
        s_tx_lock = SDL_CreateMutex();
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
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
    memcpy(&s_peer, res->ai_addr, res->ai_addrlen);
    s_peer_len = (socklen_t) res->ai_addrlen;
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
    sock_nonblock(sock);
    session_reset();
    /* A full raw queue (any stall) with qtype 0 makes the pad alarm shift
     * qread, dropping the head write_head() just filled: the tick then eats
     * a raw sample (remote port = no controller) and the peers diverge. 2
     * drops the new raw sample instead; the local input is read from the
     * head anyway. */
    HSD_PadLibData.qtype = 2;
    s_local = player ? 1 : 0;
    s_remote = 1 - s_local;
    const char* delay = getenv("MELEE_NET_DELAY");
    s_delay = delay ? atoi(delay) : 2;
    if (s_delay < 0 || s_delay >= RING / 2) {
        s_delay = 2;
    }
    const char* loss = getenv("MELEE_NET_SIM_LOSS");
    const char* sim_delay = getenv("MELEE_NET_SIM_DELAY_MS");
    s_sim_loss = loss ? atoi(loss) : 0;
    s_sim_delay_ns = sim_delay ? (uint64_t) atoi(sim_delay) * 1000000ull : 0;
    if (s_sim_loss > 0) {
        srand((unsigned) getpid());
    }
    s_seed = seed;
    if (seed != 0) {
        *HSD_RandSeedPtr = seed;
    }
    SDL_LockMutex(s_tx_lock);
    s_sock = sock;
    s_active = true;
    SDL_UnlockMutex(s_tx_lock);
    pc_log_line("net: rollback with %s:%u, local port %u, player P%d, delay %d, window %d, "
                "seed %u, sim loss %d%% delay %d ms",
                ip, port, bind_port, s_local + 1, s_delay, WINDOW, seed, s_sim_loss,
                (int) (s_sim_delay_ns / 1000000));
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
    return s_tick_frame;
}

uint32_t pc_net_seed(void) {
    return s_seed;
}

int pc_net_handshake_state(void) {
    return s_hs;
}

/* Drive the pending handshake a step; true once done, with start_frame. */
static bool hs_poll(int32_t* start_frame) {
    if (s_hs == HS_PENDING) {
        recv_inputs();
        if (SDL_GetTicksNS() - s_hs_t0 > HS_TIMEOUT_MS * 1000000ull) {
            s_hs = HS_FAILED;
            pc_log_line("net: handshake timed out, no %s", s_hs_host ? "READY" : "RULES");
        }
    }
    if (s_hs != HS_DONE) {
        return false;
    }
    *start_frame = s_start_frame;
    return true;
}

bool pc_net_host_match(uint32_t seed, int32_t* start_frame) {
    if (s_hs == HS_IDLE) {
        if (!s_active) {
            return false;
        }
        s_hs = HS_PENDING;
        s_hs_host = true;
        s_hs_t0 = SDL_GetTicksNS();
        s_seed = seed;
        *HSD_RandSeedPtr = seed;
        s_start_frame = s_tick_frame + HS_LEAD_FRAMES;
        Rules ru = { seed, s_start_frame };
        rules_capture(&ru);
        pc_net_send_reliable(REL_RULES, &ru, sizeof ru);
        pc_log_line("net: RULES sent seed=%u start_frame=%d", seed, s_start_frame);
        rules_apply(&ru, false);
    }
    return hs_poll(start_frame);
}

bool pc_net_guest_wait_match(uint32_t* seed, int32_t* start_frame) {
    if (s_hs == HS_IDLE) {
        if (!s_active) {
            return false;
        }
        s_hs = HS_PENDING; /* RULES may already have landed: then s_hs is DONE */
        s_hs_host = false;
        s_hs_t0 = SDL_GetTicksNS();
    }
    if (!hs_poll(start_frame)) {
        return false;
    }
    *seed = s_seed;
    return true;
}

/* MELEE_NET_HANDSHAKE_TEST=1: the lobby handshake without a lobby, from
 * frame 300, plus one caller-typed message each way at frame 600. */
static void handshake_test(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("MELEE_NET_HANDSHAKE_TEST") != NULL;
    }
    if (!on || s_tick_frame < 300 || s_hs == HS_FAILED) {
        return;
    }
    int32_t sf;
    uint32_t seed;
    if (s_local == 0) {
        pc_net_host_match(1234, &sf);
    } else {
        pc_net_guest_wait_match(&seed, &sf);
    }
    if (s_tick_frame == 600) {
        pc_net_send_reliable(0x10, "ping", 4);
    }
    uint8_t type;
    char buf[REL_MAX];
    int n = pc_net_recv_reliable(&type, buf, sizeof buf);
    if (n >= 0) {
        pc_log_line("net: reliable recv type %02x len %d '%.*s' at frame %d", type, n, n, buf,
                    s_tick_frame);
    }
}

bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks) {
    if (!s_active) {
        return false;
    }
    *ping_ms = (int) (s_ping_us / 1000);
    *delay_frames = s_delay;
    *rollbacks = s_rollbacks;
    return true;
}

/* ---- record / replay --------------------------------------------------
 * MELEE_NET_RECORD=file  writes the seed, then per frame the four PADStatus
 *                        actually simulated plus the frame checksum.
 * MELEE_NET_REPLAY=file  feeds those pads back in and reports the first
 *                        frame whose checksum differs: the determinism test
 *                        for M0 (docs/netcode-plan.md §5). Works solo or
 *                        together with netplay (record only). */
static FILE* s_rec;
static FILE* s_rep;
static bool s_rep_reported;

typedef struct FrameRecord {
    PADStatus pads[4];
    uint32_t ck;
} FrameRecord;

static void record_open(void) {
    const char* rec = getenv("MELEE_NET_RECORD");
    const char* rep = getenv("MELEE_NET_REPLAY");
    if (rec != NULL && rec[0] != '\0') {
        s_rec = fopen(rec, "wb");
        pc_log_line("net: %s %s", s_rec ? "recording to" : "cannot open", rec);
    }
    if (rep != NULL && rep[0] != '\0' && !s_active) {
        s_rep = fopen(rep, "rb");
        char magic[4];
        uint32_t seed;
        if (s_rep && (fread(magic, 4, 1, s_rep) != 1 || memcmp(magic, "MRC1", 4) != 0 ||
                      fread(&seed, 4, 1, s_rep) != 1)) {
            fclose(s_rep);
            s_rep = NULL;
        }
        if (s_rep) {
            *HSD_RandSeedPtr = seed;
        }
        pc_log_line("net: %s %s", s_rep ? "replaying" : "cannot open", rep);
    }
}

static FrameRecord s_rep_cur;

/* Load the next record's pads for this frame; false at end of file. */
static bool replay_load(PADStatus* head) {
    if (fread(&s_rep_cur, sizeof s_rep_cur, 1, s_rep) != 1) {
        return false;
    }
    memcpy(head, s_rep_cur.pads, sizeof s_rep_cur.pads);
    return true;
}

static void replay_compare(uint32_t ck) {
    if (!s_rep_reported && s_rep_cur.ck != ck) {
        s_rep_reported = true;
        pc_log_line("net: REPLAY DIVERGED at frame %d (recorded %08x now %08x)", s_frame,
                    s_rep_cur.ck, ck);
    }
}

static uint32_t frame_checksum(const PADStatus* head) {
    /* Inputs as simulated, the RNG seed entering the frame, and each
     * fighter's position, facing, percent, stocks and action state (plan
     * §4), so a physics divergence is caught on the frame it happens. */
    uint32_t ck = fnv1a(2166136261u, head, 4 * sizeof(PADStatus));
    ck = fnv1a(ck, HSD_RandSeedPtr, sizeof(u32));
    /* Player slots keep dangling entity pointers between scenes, so only
     * look while a fight is running and the entity really is a fighter. */
    for (int slot = 0; in_fight() && slot < 4; slot++) {
        HSD_GObj* gobj = Player_GetEntity(slot);
        if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
            continue;
        }
        const Fighter* fp = GET_FIGHTER(gobj);
        s32 stocks = Player_GetStocks(slot);
        ck = fnv1a(ck, &fp->cur_pos, sizeof fp->cur_pos);
        ck = fnv1a(ck, &fp->facing_dir, sizeof fp->facing_dir);
        ck = fnv1a(ck, &fp->dmg.x1830_percent, sizeof fp->dmg.x1830_percent);
        ck = fnv1a(ck, &fp->motion_id, sizeof fp->motion_id);
        ck = fnv1a(ck, &stocks, sizeof stocks);
    }
    return ck;
}

/* What went into the checksum, one line, so two peers' logs can be diffed
 * by eye when a DESYNC is reported. */
#define STATE_RING 64
static char s_state_ring[STATE_RING][320];

/* Remember what went into this frame's checksum (overwritten when the
 * frame is re-simulated, so the last write is the confirmed timeline). */
static void record_state(const PADStatus* head, int32_t frame) {
    char* buf = s_state_ring[frame & (STATE_RING - 1)];
    int n = snprintf(buf, sizeof s_state_ring[0], "f%d seed=%08x pads=%04x/%d,%d %04x/%d,%d", frame,
                     *HSD_RandSeedPtr, head[0].button, head[0].stickX, head[0].stickY,
                     head[1].button, head[1].stickX, head[1].stickY);
    for (int slot = 0; in_fight() && slot < 4 && n < (int) sizeof s_state_ring[0] - 80; slot++) {
        HSD_GObj* gobj = Player_GetEntity(slot);
        if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
            continue;
        }
        const Fighter* fp = GET_FIGHTER(gobj);
        n += snprintf(buf + n, sizeof s_state_ring[0] - (size_t) n,
                      " p%d=(%.3f,%.3f) v(%.3f,%.3f) kb(%.3f,%.3f) f%.0f %.1f%% m%d s%d", slot,
                      (double) fp->cur_pos.x, (double) fp->cur_pos.y, (double) fp->self_vel.x,
                      (double) fp->self_vel.y, (double) fp->x8c_kb_vel.x,
                      (double) fp->x8c_kb_vel.y, (double) fp->facing_dir,
                      (double) fp->dmg.x1830_percent, fp->motion_id, Player_GetStocks(slot));
    }
}

/* Dump the recorded states around `frame` (both peers do this on DESYNC, so
 * the two logs can be diffed line by line). */
static void dump_states_around(int32_t frame) {
    for (int32_t f = frame - 3; f <= frame + 1; f++) {
        const char* s = s_state_ring[f & (STATE_RING - 1)];
        if (f >= 0 && strncmp(s, "f", 1) == 0) {
            pc_log_line("net: state %s", s);
        }
    }
}

/* ---- snapshot ---------------------------------------------------------
 * Whole-region copy of everything the simulation can touch:
 *   1. the decomp's statics, bracketed by src/pc/melee_state.ld (the sound
 *      machine's TUs are excluded there: the audio thread owns them);
 *   2. every OSAlloc heap's live extent except the audio heap;
 *   3. the RNG seed pointer (aurora-side static the game redirects).
 * ponytail: plain memcpy each time; dirty tracking only if the measured cost
 * breaks the rollback budget. */
extern char __melee_data_start[], __melee_data_end[];
extern char __melee_bss_start[], __melee_bss_end[];

#define MAX_HEAPS 8

typedef struct Region {
    const char* name;
    void* ptr;
    size_t len;
} Region;

#define MAX_REGIONS (3 + MAX_HEAPS)

/* Everything a snapshot covers, as it stands right now. */
static int regions_now(Region* r) {
    int n = 0;
    void* lo;
    size_t len;
    aurora_heap_descs(&lo, &len);
    r[n++] = (Region) { "heapdescs", lo, len };
    r[n++] = (Region) { "data", __melee_data_start, (size_t) (__melee_data_end - __melee_data_start) };
    r[n++] = (Region) { "bss", __melee_bss_start, (size_t) (__melee_bss_end - __melee_bss_start) };
    for (int h = 0; h < MAX_HEAPS; h++) {
        void* hi;
        if (h == HSD_Synth_804D6018 || !aurora_heap_extent(h, &lo, &hi)) {
            continue;
        }
        static char names[MAX_HEAPS][8];
        snprintf(names[h], sizeof names[h], "heap%d", h);
        r[n++] = (Region) { names[h], lo, (size_t) ((char*) hi - (char*) lo) };
    }
    return n;
}

typedef struct Snapshot {
    int32_t frame;
    unsigned io_count;   /* s_io_count when taken; a later request forbids restore */
    uint8_t* buf;
    size_t cap;
    size_t used;
    int nregions;
    Region regions[MAX_REGIONS];
    u32* seed_ptr;
} Snapshot;

static void snap_put(Snapshot* s, const void* src, size_t n) {
    if (s->used + n > s->cap) {
        s->cap = (s->used + n) * 3 / 2;
        s->buf = realloc(s->buf, s->cap);
    }
    memcpy(s->buf + s->used, src, n);
    s->used += n;
}

static void snapshot_take(Snapshot* s, int32_t frame) {
    bool intr = OSDisableInterrupts();
    s->frame = frame;
    s->io_count = s_io_count;
    s->used = 0;
    s->seed_ptr = HSD_RandSeedPtr;
    s->nregions = regions_now(s->regions);
    for (int i = 0; i < s->nregions; i++) {
        snap_put(s, s->regions[i].ptr, s->regions[i].len);
    }
    OSRestoreInterrupts(intr);
}

static void snapshot_restore(const Snapshot* s) {
    bool intr = OSDisableInterrupts();
    const uint8_t* p = s->buf;
    for (int i = 0; i < s->nregions; i++) {
        memcpy(s->regions[i].ptr, p, s->regions[i].len);
        p += s->regions[i].len;
    }
    HSD_RandSeedPtr = s->seed_ptr;
    OSRestoreInterrupts(intr);
}

/* Hash of the same regions a snapshot covers, taken fresh from memory. */
static uint64_t state_hash(void) {
    Region r[MAX_REGIONS];
    int n = regions_now(r);
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    for (int i = 0; i < n; i++) {
        XXH3_64bits_update(st, r[i].ptr, r[i].len);
    }
    XXH3_64bits_update(st, HSD_RandSeedPtr, sizeof(u32));
    uint64_t h = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    return h;
}

/* Tally of differing 64-byte chunks across every mismatch, keyed by address
 * (statics keep their address; heap chunks are keyed by address too, which
 * is stable within a scene). Dumped with the periodic report. */
typedef struct DiffTally {
    const void* addr;
    const char* region;
    unsigned count;
} DiffTally;
#define TALLY_MAX 256
static DiffTally s_tally[TALLY_MAX];
static int s_tally_n;

static void tally_add(const char* region, const void* addr) {
    for (int i = 0; i < s_tally_n; i++) {
        if (s_tally[i].addr == addr) {
            s_tally[i].count++;
            return;
        }
    }
    if (s_tally_n < TALLY_MAX) {
        s_tally[s_tally_n++] = (DiffTally) { addr, region, 1 };
    }
}

static void tally_report(void) {
    for (int pass = 0; pass < 8 && s_tally_n > 0; pass++) {
        int best = -1;
        for (int i = 0; i < s_tally_n; i++) {
            if (s_tally[i].count > 0 && (best < 0 || s_tally[i].count > s_tally[best].count)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        pc_log_line("net:   %-6s %p x%u", s_tally[best].region, s_tally[best].addr,
                    s_tally[best].count);
        s_tally[best].count = 0; /* consumed; keeps the table for identity */
    }
}

static void snapshot_diff(const Snapshot* s) {
    const uint8_t* p = s->buf;
    for (int i = 0; i < s->nregions; i++) {
        const Region* r = &s->regions[i];
        for (size_t off = 0; off < r->len; off += 64) {
            size_t n = r->len - off < 64 ? r->len - off : 64;
            if (memcmp((uint8_t*) r->ptr + off, p + off, n) != 0) {
                tally_add(r->name, (uint8_t*) r->ptr + off);
            }
        }
        p += r->len;
    }
}

/* ---- sync test --------------------------------------------------------
 * MELEE_NET_SYNCTEST=1: every tick is run twice, snapshot -> tick -> hash ->
 * restore -> tick again -> hash, and the two hashes must match. Proves that
 * the snapshot covers all state the tick depends on and that a tick is a
 * pure function of (state, inputs), which is what rollback needs. */
static bool s_synctest;
static Snapshot s_snap;       /* state before the tick */
static Snapshot s_after1;     /* state after the first run of the tick */
static uint64_t s_hash_first;
static int s_retick;          /* 0 normal, 1 first tick done, 2 retick done */
static unsigned s_sync_fail, s_sync_skipped;
static uint64_t s_snap_ns, s_snap_ns_max;
static unsigned s_io_at_tick; /* value when the current tick started */
static bool s_io_inflight_at_take;

bool pc_net_resim(void) {
    return s_resim || s_synctest;
}

static bool synctest_after_tick(void) {
    if (s_retick == 0) {
        if (s_io_count != s_io_at_tick || s_io_inflight_at_take || io_inflight()) {
            /* The tick issued a disc read, or one is still completing on a
             * worker thread; re-running would issue it twice / lose the
             * completion. Rollback proper never spans a load either. */
            s_sync_skipped++;
            return false;
        }
        s_hash_first = state_hash();
        snapshot_take(&s_after1, s_snap.frame);
        snapshot_restore(&s_snap);
        s_retick = 1;
        s_resim = true;
        return true;
    }
    s_resim = false;
    s_retick = 0;
    uint64_t second = state_hash();
    if (second != s_hash_first) {
        s_sync_fail++;
        snapshot_diff(&s_after1);
    }
    if ((s_snap.frame % 600) == 0 && s_snap.frame > 0) {
        size_t heap_bytes = 0;
        for (int i = 3; i < s_snap.nregions; i++) {
            heap_bytes += s_snap.regions[i].len;
        }
        pc_log_line("net: synctest frame %d, %u mismatches, %u skipped (I/O), snapshot %.2f MB "
                    "(%d heaps %.2f MB), take %.2f ms (max %.2f)",
                    s_snap.frame, s_sync_fail, s_sync_skipped, s_snap.used / 1048576.0,
                    s_snap.nregions - 3, heap_bytes / 1048576.0, s_snap_ns / 1e6,
                    s_snap_ns_max / 1e6);
        s_snap_ns_max = 0;
        tally_report();
    }
    return false;
}

/* ---- rollback ---------------------------------------------------------
 * One snapshot per predicted frame (Slippi cadence), taken right before the
 * tick that consumes the prediction. When the real input for such a frame
 * arrives and differs, that frame's snapshot is restored and every frame
 * since is ticked again from the input rings with the resim flag on. */
static Snapshot s_snaps[SNAPS];

/* A new session restarts at frame 0: no old snapshot may match a frame. */
static void snaps_invalidate(void) {
    for (int i = 0; i < SNAPS; i++) {
        s_snaps[i].frame = -1;
    }
}

static void predict(int32_t f) {
    static const WirePad neutral;
    s_remote_ring[f & (RING - 1)] =
        s_remote_have >= 0 ? s_remote_ring[s_remote_have & (RING - 1)] : neutral;
}

/* Snapshot before a predicted frame. Nothing can be captured while a disc
 * transfer is pending, so that slot stays unusable and a misprediction there
 * is lost. */
static void snap_predicted(int32_t f) {
    Snapshot* s = &s_snaps[f & (SNAPS - 1)];
    if (io_inflight()) {
        s->frame = -1;
        return;
    }
    snapshot_take(s, f);
}

/* Ports 0-3 of the queue head become the synced inputs for frame f. */
static void write_head(PADStatus* head, int32_t f) {
    static const WirePad neutral;
    const WirePad* mine = f >= s_delay ? &s_local_ring[f & (RING - 1)] : &neutral;
    from_wire(&head[s_local], mine);
    const WirePad* theirs = &s_remote_ring[f & (RING - 1)];
    from_wire(&head[s_remote], theirs);
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
        /* ponytail: queue full (a hitch queued 5 samples during a rollback);
         * this re-run eats a real sample, time sync pays it back. */
        PadLibData* p = &HSD_PadLibData;
        head = &p->queue->stat[p->qread * 4];
    }
    write_head(head, f);
    s_ck_ring[f & (RING - 1)] = frame_checksum(head);
    s_tick_frame = f;
}

static bool rollback_to(int32_t f) {
    Snapshot* s = &s_snaps[f & (SNAPS - 1)];
    if (s->buf == NULL || s->frame != f || s->io_count != s_io_count || io_inflight()) {
        s_rb_lost++;
        pc_log_line("net: cannot roll back to frame %d (%s), expect a desync", f,
                    s->buf != NULL && s->frame == f ? "disc I/O since" : "no snapshot");
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
    s_resim = true;
    resim_prepare(f);
    return true;
}

/* ---- per-tick entry --------------------------------------------------- */

/* A fresh frame: capture the local sample, exchange inputs, predict or
 * stall, and feed the queue head. `raw` is false for the extra tick a
 * time-sync advance adds, which reuses the last physical sample. */
static void fresh_tick(PADStatus* head, bool raw) {
    if (s_active) {
        if (raw) {
            s_raw_last = head[0];
        }
        to_wire(&s_local_ring[(s_frame + s_delay) & (RING - 1)], &s_raw_last);
        send_inputs();
        recv_inputs();
        /* Predict at most WINDOW frames past the remote, and only in a
         * fight: a tick that loads from disc can never be re-run, menus and
         * scene changes load constantly, and the match's own loads trail
         * into its first frames, so stay in lockstep until they have been
         * quiet a while (plan §10.4: menus lockstep, rollback armed in GS_VS). */
        bool lockstep = !in_fight() || s_frame - s_io_frame < 120 || io_inflight();
        int32_t need = lockstep ? s_frame : s_frame - WINDOW;
        if (!wait_remote(need)) {
            pc_log_line("net: peer silent for %d ms at frame %d, leaving netplay",
                        s_heard ? STALL_TIMEOUT_MS : CONNECT_TIMEOUT_MS, s_frame);
            pc_net_disconnect();
            return;
        }
        if ((s_frame % SYNC_INTERVAL) == 0 && s_frame > 0) {
            time_sync();
        }
        if (s_skip_left > 0) {
            skip_frame();
            s_skip_left--;
        }
        if (s_remote_have < s_frame) {
            predict(s_frame);
            snap_predicted(s_frame);
        }
        write_head(head, s_frame); /* after anything that can run the pad alarm */
    }
    if (s_rep != NULL && !replay_load(head)) {
        pc_log_line("net: replay finished at frame %d%s", s_frame,
                    s_rep_reported ? "" : ", no divergence");
        fclose(s_rep);
        s_rep = NULL;
    }

    if (s_start_frame == s_frame && s_hs != HS_FAILED) {
        *HSD_RandSeedPtr = s_seed; /* both peers enter the match from the agreed seed */
    }
    uint32_t ck = frame_checksum(head);
    s_ck_ring[s_frame & (RING - 1)] = ck;
    if (s_active) {
        record_state(head, s_frame);
        bool before = s_desync_reported;
        check_desync();
        if (s_desync_reported && !before) {
            dump_states_around(s_remote_ck_frame);
        }
    }
    if (s_rec != NULL) {
        if (s_frame == 0) {
            fwrite("MRC1", 4, 1, s_rec);
            fwrite(HSD_RandSeedPtr, 4, 1, s_rec);
        }
        FrameRecord r;
        memcpy(r.pads, head, sizeof r.pads);
        r.ck = ck;
        fwrite(&r, sizeof r, 1, s_rec);
        fflush(s_rec); /* runs usually end by SIGTERM; keep every frame */
    }
    if (s_rep != NULL) {
        replay_compare(ck);
    }

    if ((s_frame % 600) == 0 && s_frame > 0 && s_active) {
        pc_log_line("net: frame %d, rollbacks %u (max depth %d, lost %u), stalls %u (worst "
                    "%.1f ms), skips %u, advances %u, ping %u ms (avg %.0f), offset %+.1f ms, "
                    "remote behind %d",
                    s_frame, s_rollbacks, s_rb_depth_max, s_rb_lost, s_stalls,
                    s_stall_ns_max / 1e6, s_skips, s_advances, s_ping_us / 1000,
                    s_ping_n ? s_ping_sum / 1000.0 / s_ping_n : 0.0, s_offset_last / 1000.0,
                    s_frame - 1 - s_remote_have);
        s_stall_ns_max = 0;
        s_ping_sum = 0;
        s_ping_n = 0;
    }
    s_tick_frame = s_frame;
    s_frame++;
}

void pc_net_sync(void) {
    static bool opened;
    if (!opened) {
        opened = true;
        record_open();
        s_synctest = getenv("MELEE_NET_SYNCTEST") != NULL;
        if (s_synctest) {
            pc_log_line("net: synctest on (every tick simulated twice, sound off)");
        }
    }
    if (s_active) {
        SDL_LockMutex(s_tx_lock);
        if (s_timer == 0) {
            s_timer = SDL_AddTimer(4, tx_timer, NULL);
        }
        SDL_UnlockMutex(s_tx_lock);
    }
    if (s_synctest) {
        uint64_t t0 = SDL_GetTicksNS();
        snapshot_take(&s_snap, s_frame);
        s_io_at_tick = s_io_count;
        s_io_inflight_at_take = io_inflight();
        s_snap_ns = SDL_GetTicksNS() - t0;
        if (s_snap_ns > s_snap_ns_max) {
            s_snap_ns_max = s_snap_ns;
        }
    }
    if (!s_active && s_rec == NULL && s_rep == NULL) {
        s_frame++;
        return;
    }
    PadLibData* p = &HSD_PadLibData;
    fresh_tick(&p->queue->stat[p->qread * 4], true);
}

bool pc_net_after_tick(void) {
    if (s_synctest) {
        return synctest_after_tick();
    }
    if (!s_active) {
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
    if (s_resim) {
        if (s_tick_frame + 1 < s_frame) {
            resim_prepare(s_tick_frame + 1);
            return true;
        }
        s_resim = false;
    }
    check_desync();
    handshake_test();
    if (s_advance_left > 0 && (s_frame % 5) == 0) {
        /* Behind the peer: one extra tick this present, once per 5 frames. */
        PADStatus* head = unconsume();
        if (head != NULL) {
            s_advance_left--;
            s_advances++;
            fresh_tick(head, false);
            return true;
        }
    }
    return false;
}
