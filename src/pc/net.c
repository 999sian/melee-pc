/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay prototype: two instances exchange one PADStatus per frame over UDP
 * and run in lockstep with a fixed input delay. Both peers must boot with the
 * same disc, the same MELEE_SEED and no memory card so the whole game (menus
 * included) advances on identical inputs from the first frame.
 *
 *   MELEE_NET=host:port     peer address (required; enables netplay)
 *   MELEE_NET_PORT=n        local UDP port (default 41000)
 *   MELEE_NET_PLAYER=0|1    which controller port the local player drives
 *   MELEE_NET_DELAY=n       input delay in frames (default 2)
 *
 * ponytail: lockstep only. Rollback replaces the blocking wait in
 * wait_remote() with prediction + snapshot restore (docs/netcode-plan.md §4). */
#include "compat.h"
#include "pc/net.h"
#include "pc/pc.h"

#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/synth.h>
#include <xxhash.h>

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
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

#define RING 64          /* frames of history kept per side; power of two */
#define REDUNDANCY 8     /* past frames repeated in every packet */
#define STALL_TIMEOUT_MS 10000

/* 8-byte pad, same fields Slippi puts on the wire. */
typedef struct WirePad {
    uint16_t button;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerLeft, triggerRight;
} WirePad;

typedef struct Packet {
    uint8_t magic;       /* 'M' */
    uint8_t player;
    int32_t newest;      /* frame of pads[0]; pads[i] is frame newest-i */
    int32_t ck_frame;    /* frame the checksum was taken after */
    uint32_t ck;
    uint8_t count;
    WirePad pads[REDUNDANCY];
} __attribute__((packed)) Packet;

/* ---- state ------------------------------------------------------------ */

static bool s_active;
static sock_t s_sock = SOCK_INVALID;
static struct sockaddr_storage s_peer;
static socklen_t s_peer_len;
static int s_local, s_remote, s_delay;
static int32_t s_frame;                  /* next frame to simulate */
static WirePad s_local_ring[RING];       /* indexed by frame & (RING-1) */
static WirePad s_remote_ring[RING];
static int32_t s_remote_have = -1;       /* newest contiguous remote frame */
static int32_t s_remote_ck_frame = -1;
static uint32_t s_remote_ck;
static uint32_t s_ck_ring[RING];         /* our checksum after each frame */
static bool s_desync_reported;
static unsigned s_stalls;
static uint64_t s_stall_ns_max;

/* ---- helpers ---------------------------------------------------------- */

static void to_wire(WirePad* w, const PADStatus* p) {
    w->button = p->button;
    w->stickX = p->stickX;
    w->stickY = p->stickY;
    w->substickX = p->substickX;
    w->substickY = p->substickY;
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

static void send_inputs(void) {
    Packet pk;
    memset(&pk, 0, sizeof pk);
    pk.magic = 'M';
    pk.player = (uint8_t) s_local;
    /* Local input for frame f is produced at frame f - delay, so the newest
     * we can offer is s_frame - 1 + delay. */
    int32_t newest = s_frame - 1 + s_delay;
    pk.newest = newest;
    int n = 0;
    for (int32_t f = newest; f >= 0 && n < REDUNDANCY; f--, n++) {
        pk.pads[n] = s_local_ring[f & (RING - 1)];
    }
    pk.count = (uint8_t) n;
    pk.ck_frame = s_frame - 1;
    pk.ck = s_frame > 0 ? s_ck_ring[(s_frame - 1) & (RING - 1)] : 0;
    sendto(s_sock, (const char*) &pk, sizeof pk, 0, (struct sockaddr*) &s_peer, s_peer_len);
}

static void recv_inputs(void) {
    for (;;) {
        Packet pk;
        int n = (int) recvfrom(s_sock, (char*) &pk, sizeof pk, 0, NULL, NULL);
        if (n < (int) sizeof pk || pk.magic != 'M' || pk.player != s_remote) {
            if (n <= 0) {
                return;
            }
            continue;
        }
        int count = pk.count > REDUNDANCY ? REDUNDANCY : pk.count;
        for (int i = 0; i < count; i++) {
            int32_t f = pk.newest - i;
            if (f > s_remote_have && f < s_remote_have + RING) {
                s_remote_ring[f & (RING - 1)] = pk.pads[i];
            }
        }
        /* Advance the contiguous mark: everything up to newest arrived in
         * this packet unless the gap exceeded the redundancy window. */
        int32_t oldest = pk.newest - count + 1;
        if (oldest <= s_remote_have + 1 && pk.newest > s_remote_have) {
            s_remote_have = pk.newest;
        }
        if (pk.ck_frame > s_remote_ck_frame) {
            s_remote_ck_frame = pk.ck_frame;
            s_remote_ck = pk.ck;
        }
    }
}

static void check_desync(void) {
    if (s_desync_reported || s_remote_ck_frame < 0 || s_remote_ck_frame >= s_frame ||
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

/* Block until the remote input for s_frame is here. Returns false on timeout. */
static bool wait_remote(void) {
    if (s_remote_have >= s_frame) {
        return true;
    }
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t last_send = t0;
    s_stalls++;
    for (;;) {
        recv_inputs();
        if (s_remote_have >= s_frame) {
            break;
        }
        uint64_t now = SDL_GetTicksNS();
        if (now - t0 > (uint64_t) STALL_TIMEOUT_MS * 1000000ull) {
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

/* ---- public ----------------------------------------------------------- */

bool pc_net_active(void) {
    return s_active;
}

void pc_net_init(void) {
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
    if (getenv("MELEE_SEED") == NULL) {
        pc_log_line("net: MELEE_SEED not set; peers will diverge at the first random call");
    }
    s_active = true;
    pc_log_line("net: lockstep with %s, local port %u, player P%d, delay %d", peer, bind_port,
                s_local + 1, s_delay);
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
    /* Inputs as simulated plus the RNG seed entering the frame. The seed is
     * consumed by hits, AI and effects, so a physics divergence reaches it
     * within a few frames. ponytail: add fighter positions when a mismatch
     * needs localising to a frame rather than a neighbourhood. */
    uint32_t ck = fnv1a(2166136261u, head, 4 * sizeof(PADStatus));
    return fnv1a(ck, HSD_RandSeedPtr, sizeof(u32));
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
static bool s_resim;          /* inside a re-simulated tick */
static Snapshot s_snap;       /* state before the tick */
static Snapshot s_after1;     /* state after the first run of the tick */
static uint64_t s_hash_first;
static int s_retick;          /* 0 normal, 1 first tick done, 2 retick done */
static unsigned s_sync_fail, s_sync_skipped;
static uint64_t s_snap_ns, s_snap_ns_max;
static unsigned s_io_count;   /* disc requests issued so far */
static unsigned s_io_at_tick; /* value when the current tick started */
static bool s_io_inflight_at_take;

void pc_net_note_io(void) {
    s_io_count++;
}

int aurora_dvd_inflight(void);
int aurora_arq_inflight(void);

/* True when a disc or ARAM transfer is still pending on a worker thread:
 * its completion will write into game memory whenever it lands, so no
 * snapshot may be taken or restored until it has. */
static bool io_inflight(void) {
    return aurora_dvd_inflight() > 0 || aurora_arq_inflight() > 0;
}

bool pc_net_resim(void) {
    return s_resim || s_synctest;
}

bool pc_net_after_tick(void) {
    if (!s_synctest) {
        return false;
    }
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

/* ---- per-tick entry --------------------------------------------------- */

static bool sync_netplay(PADStatus* head) {
    /* Our physical input (port 0, where keyboard/gamepad 1 land) becomes the
     * local player's input `delay` frames from now. */
    to_wire(&s_local_ring[(s_frame + s_delay) & (RING - 1)], &head[0]);
    send_inputs();
    recv_inputs();

    if (!wait_remote()) {
        pc_log_line("net: peer silent for %d ms at frame %d, leaving netplay", STALL_TIMEOUT_MS,
                    s_frame);
        s_active = false;
        return false;
    }

    /* Frames before `delay` have no local input yet; both sides use neutral. */
    WirePad neutral;
    memset(&neutral, 0, sizeof neutral);
    const WirePad* mine = s_frame >= s_delay ? &s_local_ring[s_frame & (RING - 1)] : &neutral;
    from_wire(&head[s_local], mine);
    const WirePad* theirs = &s_remote_ring[s_frame & (RING - 1)];
    from_wire(&head[s_remote], theirs);
    static bool seen_remote;
    if (!seen_remote && theirs->button != 0) {
        seen_remote = true;
        pc_log_line("net: first remote button press (%04x) at frame %d", theirs->button, s_frame);
    }
    for (int i = 2; i < 4; i++) {
        memset(&head[i], 0, sizeof head[i]);
        head[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return true;
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
    PADStatus* head = &p->queue->stat[p->qread * 4];

    if (s_active && !sync_netplay(head)) {
        return;
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
        pc_log_line("net: frame %d, stalls %u, worst stall %.1f ms", s_frame, s_stalls,
                    s_stall_ns_max / 1e6);
        s_stall_ns_max = 0;
    }
    s_frame++;
}
