/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay prototype: two instances exchange one PADStatus per frame over UDP
 * and run in rollback lockstep modelled on Slippi (docs/netcode-plan.md §4):
 * a remote input that has not arrived yet is predicted as "repeat last", a
 * snapshot is taken before every predicted tick, and when the real input
 * turns out different the state is restored and the frames since re-run.
 * Both peers must boot with the same disc, the same MELEE_SEED and no memory
 * card so the whole game (menus included) advances on identical inputs from
 * the first frame.
 *
 *   MELEE_NET=host:port          peer address (required; enables netplay)
 *   MELEE_NET_PORT=n             local UDP port (default 41000)
 *   MELEE_NET_PLAYER=0|1         which controller port the local player drives
 *   MELEE_NET_DELAY=n            input delay in frames (default 2)
 *   MELEE_NET_SIM_LOSS=percent   drop that share of outgoing packets
 *   MELEE_NET_SIM_DELAY_MS=ms    hold every outgoing packet that long
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
 * release of held packets) share the socket under s_tx_lock. */
static SDL_Mutex* s_tx_lock;
static Packet s_last_pkt;
static bool s_last_valid;
static uint64_t s_last_send_ns;
static int s_sim_loss;                   /* percent of outgoing packets dropped */
static uint64_t s_sim_delay_ns;

typedef struct Held {
    uint64_t release_ns;
    uint8_t len;
    uint8_t buf[sizeof(Packet)];
} Held;
#define HELD_MAX 128
static Held s_held[HELD_MAX];
static int s_held_head, s_held_n;

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
    h->len = (uint8_t) len;
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

/* Slippi's second send: the newest input packet goes out again mid-frame,
 * so a lost packet costs half a frame instead of a whole one. Runs on SDL's
 * timer thread; it only touches the frozen packet copy and the held queue. */
static Uint32 SDLCALL tx_timer(void* ud, SDL_TimerID id, Uint32 interval) {
    (void) ud;
    (void) id;
    if (!s_active) {
        return 0;
    }
    SDL_LockMutex(s_tx_lock);
    tx_flush();
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

static void recv_inputs(void) {
    for (;;) {
        union {
            Packet pk;
            Ack ack;
        } u;
        int n = (int) recvfrom(s_sock, (char*) &u, sizeof u, 0, NULL, NULL);
        if (n <= 0) {
            return;
        }
        if (n >= (int) offsetof(Packet, pads) && u.pk.magic == 'M' && u.pk.player == s_remote) {
            on_inputs(&u.pk, n);
        } else if (n == (int) sizeof(Ack) && u.ack.magic == 'A' && u.ack.player == s_remote) {
            on_ack(&u.ack);
        } else {
            continue;
        }
        s_heard = true;
    }
}

static void check_desync(void) {
    if (s_desync_reported || s_remote_ck_frame < 0 || s_remote_ck_frame > confirmed_frame() ||
        s_remote_ck_frame <= s_frame - RING) {
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
    const char* port = colon + 1;

#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
        pc_log_line("net: cannot resolve %s", peer);
        return;
    }
    memcpy(&s_peer, res->ai_addr, res->ai_addrlen);
    s_peer_len = (socklen_t) res->ai_addrlen;
    int family = res->ai_family;
    freeaddrinfo(res);

    s_sock = socket(family, SOCK_DGRAM, 0);
    if (s_sock == SOCK_INVALID) {
        pc_log_line("net: socket() failed");
        return;
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
    if (bind(s_sock, (struct sockaddr*) &local, local_len) != 0) {
        pc_log_line("net: bind(%u) failed", bind_port);
        return;
    }
    sock_nonblock(s_sock);

    const char* player = getenv("MELEE_NET_PLAYER");
    s_local = player && player[0] == '1' ? 1 : 0;
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
    if (getenv("MELEE_SEED") == NULL) {
        pc_log_line("net: MELEE_SEED not set; peers will diverge at the first random call");
    }
    s_tx_lock = SDL_CreateMutex();
    s_active = true;
    pc_log_line("net: rollback with %s, local port %u, player P%d, delay %d, window %d, sim "
                "loss %d%% delay %d ms",
                peer, bind_port, s_local + 1, s_delay, WINDOW, s_sim_loss,
                (int) (s_sim_delay_ns / 1000000));
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
    static bool seen_remote;
    if (!seen_remote && theirs->button != 0) {
        seen_remote = true;
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
            s_active = false;
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

    uint32_t ck = frame_checksum(head);
    s_ck_ring[s_frame & (RING - 1)] = ck;
    if (s_active) {
        check_desync();
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
        if (s_active) {
            SDL_AddTimer(4, tx_timer, NULL);
        }
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
