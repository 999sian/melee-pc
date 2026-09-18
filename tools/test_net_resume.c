/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the resume-after-interruption machine in src/pc/net.c: one
 * process plays both ends. The clock is virtual (SDL_GetTicksNS returns a
 * counter that SDL_DelayNS advances, so the 7 s stall timeout and the 15 s
 * reconnect window pass in milliseconds of real time) and the peer's side of
 * the exchange is injected from a step hook that runs once per wait-loop
 * turn: net_resume_rel() for its RESUME, on_inputs() for the pads that
 * refill the gap. The socket is real but bound to an ephemeral port with no
 * peer listening, so recv_inputs() just drains empty and every send lands in
 * the void; tx() captures the input packets for the assertions. The include
 * order matters: aurora's headers must come before src/. From the repo root:
 *   cc -std=gnu11 -DTARGET_PC=1 -DMELEE_PC=1 -DAURORA \
 *      -I extern/aurora/include -I src -I src/sdk_include \
 *      -I build/_deps/sdl-build/include-revision \
 *      -I ../melee-pc/build/_deps/sdl-src/include \
 *      tools/test_net_resume.c -o /tmp/test_net_resume && /tmp/test_net_resume
 */
#include "../src/pc/net.c"

#include <stdarg.h>

/* not <assert.h>: the decomp's debug.h owns __assert, and -DNDEBUG must not blind this */
#define assert(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define SESSION 0xABCD1234u
#define SEED 0x5EEDu
#define FRAME 200   /* the frame the parked game thread is on */
#define WROTE 201   /* newest local frame in our ring */
#define HAVE 190    /* newest contiguous remote frame we hold */
#define ACKED 185   /* newest local frame we know the peer holds */

/* ---- harness state ---------------------------------------------------- */

static uint64_t s_now = 1000000000ull;   /* virtual clock */
static void (*s_step)(void);             /* one scripted network step per wait-loop turn */
static char s_log[128 * 1024];
static size_t s_log_n;
static int s_resume_sends;               /* REL_RESUME messages we queued */
static int s_rel_fail;                   /* fail that many pc_net_send_reliable calls */
static Resume s_resume_out;              /* decoded payload of the last one */
static uint8_t s_resume_raw[sizeof(Resume)];
static Packet s_tx_pkt;                  /* last input packet handed to tx() */
static bool s_tx_pkt_valid;

static bool logged(const char* needle) {
    return strstr(s_log, needle) != NULL;
}

/* How many log lines contain `needle`: a line that should fire once per
 * event must not turn into one per frame. */
static int logged_count(const char* needle) {
    int n = 0;
    for (const char* p = s_log; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
        n++;
    }
    return n;
}

/* ---- stubs: the other net_*.c modules, SDL, the game ------------------ */

void pc_log_line(const char* fmt, ...) {
    va_list ap;
    char line[512];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("  log| %s\n", line);
    s_log_n += (size_t) snprintf(s_log + s_log_n, sizeof s_log - s_log_n, "%s\n", line);
}

uint64_t pc_sim_period_ns(void) {
    return 16666667ull;
}

Uint64 SDL_GetTicksNS(void) {
    return s_now;
}

void SDL_DelayNS(Uint64 ns) {
    (void) ns;
    s_now += 1000000ull; /* 1 ms per wait-loop turn */
    if (s_step != NULL) {
        s_step();
    }
}

SDL_Mutex* SDL_CreateMutex(void) {
    return NULL;
}
void SDL_LockMutex(SDL_Mutex* m) {
    (void) m;
}
void SDL_UnlockMutex(SDL_Mutex* m) {
    (void) m;
}
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback cb, void* ud) {
    (void) interval;
    (void) cb;
    (void) ud;
    return 1;
}
bool SDL_RemoveTimer(SDL_TimerID id) {
    (void) id;
    return true;
}
SDL_ThreadID SDL_GetCurrentThreadID(void) {
    return 1;
}
Uint64 SDL_GetPerformanceCounter(void) {
    return 424242;
}

/* net_wire.c: identity codecs, so a captured packet reads in host order.
 * wire_resume() is net.c's own and stays real. */
Hdr hdr(uint8_t magic) {
    Hdr h = { magic, WIRE_VERSION, net.session, (uint8_t) net.remote };
    return h;
}
void wire_packet(Packet* pk) {
    (void) pk;
}
void wire_ack(Ack* a) {
    (void) a;
}
void wire_rel(Rel* r) {
    (void) r;
}
void wire_hdr(Hdr* h) {
    (void) h;
}
void to_wire(WirePad* w, const PADStatus* p) {
    (void) p;
    memset(w, 0, sizeof *w);
}
void from_wire(PADStatus* p, const WirePad* w) {
    (void) w;
    memset(p, 0, sizeof *p);
}
bool addr_eq(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    (void) a;
    (void) b;
    return true;
}
uint32_t fnv1a(uint32_t h, const void* data, size_t n) {
    (void) data;
    (void) n;
    return h;
}

/* net_sim.c */
void tx(const void* buf, size_t len) {
    const uint8_t* p = buf;
    net.tx_pkts++;
    if (p[0] == 'M' && len >= offsetof(Packet, pads)) {
        memcpy(&s_tx_pkt, buf, len < sizeof s_tx_pkt ? len : sizeof s_tx_pkt);
        s_tx_pkt_valid = true;
        net.tx_inputs++;
    }
}
void tx_flush(void) {}
int held_put(Held* held, const void* buf, size_t len, uint64_t release_ns) {
    (void) held;
    (void) buf;
    (void) len;
    (void) release_ns;
    return -1;
}
Held* held_due(Held* held, uint64_t now) {
    (void) held;
    (void) now;
    return NULL;
}
void sim_env(uint16_t bind_port) {
    (void) bind_port;
}
void sim_reset(void) {}

/* net_reliable.c: the transmit side is what the resume machine drives. */
bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    assert(type == REL_RESUME);
    assert(len == (int) sizeof(Resume));
    if (s_rel_fail > 0) {
        s_rel_fail--;
        return false; /* lane queue full */
    }
    memcpy(s_resume_raw, payload, (size_t) len);
    memcpy(&s_resume_out, payload, sizeof s_resume_out);
    wire_resume(&s_resume_out);
    s_resume_sends++;
    return true;
}
int pc_net_recv_reliable(uint8_t* type, void* payload, int max) {
    (void) type;
    (void) payload;
    (void) max;
    return -1;
}
void rel_service(void) {}
void on_rel(const Rel* r, int n) {
    (void) r;
    (void) n;
}
void on_rel_ack(const RelAck* k) {
    (void) k;
}
void rel_reset(void) {}

/* net_handshake.c / net_sync.c */
void rules_restore(void) {}
void handshake_test(void) {}
void offset_note(int32_t off) {
    (void) off;
}
void jitter_note(uint32_t rtt) {
    (void) rtt;
}
uint32_t jitter_us(void) {
    return 0;
}
void time_sync(void) {}
void sync_reset(void) {}

/* The pad-slip detector (net.c head_note/head_check) reads the retrace count
 * and the game's master pad array; neither is linked into this harness. */
HSD_PadStatus HSD_PadMasterStatus[4];
u32 VIGetRetraceCount(void) {
    return 0;
}
bool lb_80019A30(int i) {
    (void) i;
    return false;
}
const char* state_line(int32_t frame) {
    (void) frame;
    return "";
}

/* net_snapshot.c */
static Snapshot s_snap;
bool snapshot_take(Snapshot* s, int32_t frame) {
    s->frame = frame;
    return true;
}
const char* snapshot_unusable(const Snapshot* s) {
    (void) s;
    return NULL;
}
void snapshot_restore(const Snapshot* s) {
    (void) s;
}
Snapshot* snap_slot(int32_t f) {
    (void) f;
    return &s_snap;
}
void snaps_free(void) {}
void snap_stats_report(void) {}
const char* snapshot_state_region_missing(void) {
    return NULL;
}
uint32_t frame_checksum(const PADStatus* head) {
    (void) head;
    return 0;
}
void record_state(const PADStatus* head, int32_t frame) {
    (void) head;
    (void) frame;
}
void dump_states_around(int32_t frame) {
    (void) frame;
}
void record_open(void) {}
bool record_active(void) {
    return false;
}
void replay_feed(PADStatus* head) {
    (void) head;
}
void record_frame(const PADStatus* head, uint32_t ck) {
    (void) head;
    (void) ck;
}
void synctest_before_tick(void) {}
bool synctest_after_tick(void) {
    return false;
}
void resim_note(int ticks, bool split) {
    (void) ticks;
    (void) split;
}
const char* snapshot_describe(const Snapshot* s, char* buf, size_t n) {
    (void) s;
    (void) n;
    return buf;
}

/* the game */
struct GameSceneInfo* gm_804D6720;
PadLibData HSD_PadLibData;
static u32 s_seed_val;
u32* HSD_RandSeedPtr = &s_seed_val;
BOOL OSDisableInterrupts(void) {
    return 1;
}
BOOL OSRestoreInterrupts(BOOL level) {
    return level;
}

/* ---- fixture ---------------------------------------------------------- */

/* A session mid-match with the game thread parked: frame FRAME, our ring
 * full up to WROTE, the peer's input known up to HAVE. */
static void setup(void) {
    /* A socket per case: pc_net_disconnect() closes the one it is given. */
    struct sockaddr_in a;
    sock_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sock != SOCK_INVALID);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; /* ephemeral: no collision with a concurrent run */
    assert(bind(sock, (struct sockaddr*) &a, sizeof a) == 0);
    assert(sock_nonblock(sock));
    session_reset();
    net.sock = sock;
    net.active = true;
    net.local = 0;
    net.remote = 1;
    net.session = SESSION;
    net.seed = SEED;
    net.delay = net.delay_next = 2;
    net.frame = FRAME;
    net.tick_frame = FRAME - 1;
    /* The peer's address: loopback discard, so sendto() succeeds and nothing
     * ever answers. */
    struct sockaddr_in* p = (struct sockaddr_in*) &net.peer;
    memset(p, 0, sizeof *p);
    p->sin_family = AF_INET;
    p->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    p->sin_port = htons(9);
    net.peer_len = sizeof *p;
    s_wrote = WROTE;
    s_remote_have = HAVE;
    s_last_acked = ACKED;
    s_heard = true;
    s_rc_window_ms = RECONNECT_MS;
    for (int32_t f = 0; f <= WROTE; f++) {
        s_local_ring[f & (RING - 1)].button = (uint16_t) (0x1000 + f);
    }
    s_resume_sends = 0;
    s_rel_fail = 0;
    s_tx_pkt_valid = false;
    s_step = NULL;
    s_log_n = 0;
    s_log[0] = '\0';
    memset(&s_resume_out, 0, sizeof s_resume_out);
    memset(s_resume_raw, 0, sizeof s_resume_raw);
}

/* The peer's half of the exchange. */
static void peer_resume(uint32_t session, uint32_t seed, int32_t newest, int32_t have) {
    Resume r = { session, seed, newest, have, newest - 2 };
    wire_resume(&r);
    net_resume_rel(&r, (int) sizeof r);
}

/* The peer's pads for first..last, buttons 0x2000 + frame so the refill can
 * be checked in the ring rather than just in the counter. */
static void peer_pads(int32_t first, int32_t last) {
    Packet pk;
    memset(&pk, 0, sizeof pk);
    pk.h = hdr('M');
    pk.seq = 7;
    pk.first = first;
    pk.newest = last;
    pk.ck_frame = -1;
    pk.count = (uint8_t) (last - first + 1);
    for (int i = 0; i < pk.count; i++) {
        pk.pads[i].button = (uint16_t) (0x2000 + first + i);
    }
    on_inputs(&pk, (int) (offsetof(Packet, pads) + (size_t) pk.count * sizeof(WirePad)));
}

/* ---- cases ------------------------------------------------------------ */

static uint64_t s_t0;
static bool s_did_exchange, s_did_pads;

/* Gap inside the ring: the exchange agrees, the pads follow, the wait ends. */
static void step_resume_ok(void) {
    if (!s_did_exchange && s_rc == RSM_ACTIVE) {
        s_did_exchange = true;
        /* The phase opened at the stall timeout, not before or after. */
        uint64_t waited = (s_now - s_t0) / 1000000ull;
        assert(waited >= STALL_TIMEOUT_MS && waited <= STALL_TIMEOUT_MS + 2);
        assert(pc_net_quality() == 3);
        assert(s_red_floor == REDUNDANCY); /* refill at the top of the window */
        assert(pc_net_peer_status() == PC_NET_PEER_OK);
        assert(s_resume_sends == 1);
        /* What we told the peer we hold. */
        assert(s_resume_out.session == SESSION && s_resume_out.seed == SEED);
        assert(s_resume_out.newest == WROTE && s_resume_out.have == HAVE);
        assert(s_resume_out.frame == FRAME);
        /* ...big-endian on the wire (0xABCD1234 leads with 0xAB). */
        assert(s_resume_raw[0] == 0xAB && s_resume_raw[1] == 0xCD);
        assert(s_resume_raw[2] == 0x12 && s_resume_raw[3] == 0x34);
        /* The peer holds our input up to 195 and has produced up to 203. */
        peer_resume(SESSION, SEED, 203, 195);
        assert(s_rc == RSM_ACTIVE);
        assert(s_status == PC_NET_PEER_OK);
        assert(s_last_acked == 195); /* our refill starts above what it holds */
        assert(s_remote_newest == 203);
        assert(s_resume_sends == 1); /* a RESUME is never answered */
        /* Nor does a repeat of it produce one. */
        peer_resume(SESSION, SEED, 203, 195);
        assert(s_resume_sends == 1);
    } else if (s_did_exchange && !s_did_pads && (s_now - s_t0) / 1000000ull >= 7100) {
        s_did_pads = true;
        peer_pads(HAVE + 1, 199);
    }
}

static void case_resume_inside_ring(void) {
    printf("case: gap inside the ring resumes\n");
    setup();
    s_did_exchange = s_did_pads = false;
    s_t0 = s_now;
    s_step = step_resume_ok;
    assert(wait_remote(199));
    assert(s_did_exchange && s_did_pads);
    assert(s_rc == RSM_NONE);
    assert(s_status == PC_NET_PEER_OK);
    assert(pc_net_quality() == 2); /* the stall itself, as before */
    assert(s_red_floor == REDUNDANCY_FLOOR); /* back to the ordinary cadence */
    assert(s_remote_have == 199);
    /* Their ring refilled into ours, frame by frame. */
    for (int32_t f = HAVE + 1; f <= 199; f++) {
        assert(s_remote_ring[f & (RING - 1)].button == (uint16_t) (0x2000 + f));
    }
    /* Ours refilled toward them: the reconnect send starts above the frame
     * the peer reported and carries every frame we hold, up to REDUNDANCY. */
    assert(s_tx_pkt_valid);
    assert(s_tx_pkt.first == 196 && s_tx_pkt.newest == WROTE);
    assert(s_tx_pkt.count == WROTE - 196 + 1);
    for (int i = 0; i < s_tx_pkt.count; i++) {
        assert(s_tx_pkt.pads[i].button == (uint16_t) (0x1000 + 196 + i));
    }
    assert(logged("net: interrupted at frame 200 (peer silent 7000 ms), reconnecting"));
    assert(logged("net: resumed at frame 200"));
    assert(!logged("cannot resume"));
}

/* The peer noticed first and we were not waiting: take its numbers, open no
 * phase, and above all send nothing back. Answering here is what produced
 * Main's 2686-lines-per-run spam: both sides answered each other's answers
 * for the rest of the match, one exchange per frame. */
static void case_one_way_while_running(void) {
    printf("case: a RESUME while we are not stalled is not answered\n");
    setup();
    peer_resume(SESSION, SEED, 203, 195);
    assert(s_resume_sends == 0);
    assert(s_rc == RSM_NONE);
    assert(pc_net_quality() != 3);
    assert(s_status == PC_NET_PEER_OK);
    assert(s_last_acked == 195); /* its numbers are still adopted */
    assert(s_remote_newest == 203);
    assert(logged_count("peer resumes from frame") == 1);
    /* Ten more arrivals: ten lines, still not one send. A reply would make
     * this unbounded on a real link. */
    for (int i = 0; i < 10; i++) {
        peer_resume(SESSION, SEED, 203, 195);
    }
    assert(s_resume_sends == 0);
    assert(logged_count("peer resumes from frame") == 11);
}

/* A gap the rings cannot cover must end the session, not resume into a
 * desync. */
static void step_gap_too_big(void) {
    if (!s_did_exchange && s_rc == RSM_ACTIVE) {
        s_did_exchange = true;
        peer_resume(SESSION, SEED, 203, WROTE - RING - 5);
        assert(s_rc == RSM_FAILED);
    }
}

static void case_gap_past_ring(void) {
    printf("case: gap larger than the ring fails\n");
    setup();
    s_did_exchange = false;
    s_t0 = s_now;
    s_step = step_gap_too_big;
    assert(!wait_remote(199));
    assert(s_did_exchange);
    assert(s_status == PC_NET_PEER_RESUME);
    assert(logged("cannot resume at frame 200, the gap outruns the 64-frame ring"));
    assert(!logged("resumed at frame"));
    /* A refused exchange must not move the send window. */
    assert(s_last_acked == ACKED);
}

/* Same for a peer that answers for another session or another seed. */
static void step_seed_mismatch(void) {
    if (!s_did_exchange && s_rc == RSM_ACTIVE) {
        s_did_exchange = true;
        peer_resume(SESSION, SEED + 1, 203, 195);
    }
}

static void case_seed_mismatch(void) {
    printf("case: a peer on another seed fails\n");
    setup();
    s_did_exchange = false;
    s_t0 = s_now;
    s_step = step_seed_mismatch;
    assert(!wait_remote(199));
    assert(s_status == PC_NET_PEER_RESUME);
    assert(logged("the peer answers for session abcd1234 seed 24302"));
    assert(s_last_acked == ACKED);
}

static void case_session_mismatch(void) {
    printf("case: a restarted peer (other session id) fails\n");
    setup();
    peer_resume(SESSION + 1, SEED, 203, 195);
    assert(s_rc == RSM_FAILED);
    assert(s_status == PC_NET_PEER_RESUME);
    assert(logged("the peer answers for session abcd1235"));
}

/* Nobody answers: the window bounds the phase and the session ends the way
 * it did before the phase existed. */
static void case_window_expires(void) {
    printf("case: the reconnect window expires\n");
    setup();
    s_t0 = s_now;
    assert(!wait_remote(199));
    uint64_t waited = (s_now - s_t0) / 1000000ull;
    assert(s_status == PC_NET_PEER_TIMEOUT);
    assert(waited >= STALL_TIMEOUT_MS + RECONNECT_MS);
    assert(waited <= STALL_TIMEOUT_MS + RECONNECT_MS + 10);
    assert(logged("net: resume window of 15000 ms expired at frame 200"));
    assert(s_resume_sends == 1);
}

/* MELEE_NET_RECONNECT_MS=0: no phase at all, the 7 s drop of before. */
static void case_disabled(void) {
    printf("case: MELEE_NET_RECONNECT_MS=0 keeps the hard drop\n");
    setup();
    s_rc_window_ms = 0;
    s_t0 = s_now;
    assert(!wait_remote(199));
    uint64_t waited = (s_now - s_t0) / 1000000ull;
    assert(s_status == PC_NET_PEER_TIMEOUT);
    assert(waited >= STALL_TIMEOUT_MS && waited <= STALL_TIMEOUT_MS + 10);
    assert(s_rc == RSM_NONE);
    assert(s_resume_sends == 0);
    assert(!logged("reconnecting"));
}

/* The reliable lane was full when the phase opened: the poll retries. */
static void case_queue_full_retries(void) {
    printf("case: a full reliable lane is retried\n");
    setup();
    s_rel_fail = 3;
    s_t0 = s_now;
    assert(!wait_remote(199)); /* nobody answers; the window ends it */
    assert(s_resume_sends == 1);
    assert(s_rc_sent);
    assert(s_status == PC_NET_PEER_TIMEOUT);
}

/* Before the peer's first packet there is nothing to resume, so that wait
 * keeps the connect timeout it always had. */
static void case_connect_timeout(void) {
    printf("case: a session that never heard the peer still times out\n");
    setup();
    s_heard = false;
    s_t0 = s_now;
    assert(!wait_remote(199));
    uint64_t waited = (s_now - s_t0) / 1000000ull;
    assert(s_status == PC_NET_PEER_TIMEOUT);
    assert(waited >= CONNECT_TIMEOUT_MS && waited <= CONNECT_TIMEOUT_MS + 10);
    assert(s_rc == RSM_NONE);
    assert(s_resume_sends == 0);
    assert(!logged("reconnecting"));
}

/* A session whose handshake never finished must fail fast: the lobby reports
 * the failure from the very thread parked here, so a phase would turn a 7 s
 * "connect failed" into a 22 s hang (tools/net_lan_test.py host_dies). */
static void case_handshake_pending_fails_fast(void) {
    printf("case: an unfinished handshake fails fast\n");
    setup();
    net.hs = HS_PENDING;
    s_t0 = s_now;
    assert(!wait_remote(199));
    uint64_t waited = (s_now - s_t0) / 1000000ull;
    assert(s_status == PC_NET_PEER_TIMEOUT);
    assert(waited >= STALL_TIMEOUT_MS && waited <= STALL_TIMEOUT_MS + 10);
    assert(s_rc == RSM_NONE);
    assert(s_resume_sends == 0);
    assert(!logged("reconnecting"));
}

/* The same for the host_dies shape itself: a lobby guest a few frames old,
 * which has adopted the host's session id but not yet been claimed by
 * pc_lan_poll(), so the handshake still reads HS_IDLE. */
static void case_young_session_fails_fast(void) {
    printf("case: a session a few frames old fails fast\n");
    setup();
    net.frame = 3; /* the frame host_dies interrupted at */
    net.tick_frame = 2;
    s_wrote = 4;
    s_remote_have = 0;
    s_last_acked = -1;
    assert(net.hs == HS_IDLE);
    s_t0 = s_now;
    assert(!wait_remote(2));
    uint64_t waited = (s_now - s_t0) / 1000000ull;
    assert(s_status == PC_NET_PEER_TIMEOUT);
    assert(waited >= STALL_TIMEOUT_MS && waited <= STALL_TIMEOUT_MS + 10);
    assert(s_rc == RSM_NONE);
    assert(s_resume_sends == 0);
    assert(!logged("reconnecting"));
}

/* ...but a finished handshake is established however young the session is. */
static void case_handshake_done_resumes_young(void) {
    printf("case: a young session with the handshake done still resumes\n");
    setup();
    net.hs = HS_DONE;
    net.frame = 3;
    net.tick_frame = 2;
    net.start_frame = 120;
    s_wrote = 4;
    s_remote_have = 0;
    s_last_acked = -1;
    s_t0 = s_now;
    assert(!wait_remote(2)); /* nobody answers: the window ends it */
    assert(s_rc == RSM_ACTIVE);
    assert(s_resume_sends == 1);
    assert(logged("net: interrupted at frame 3"));
    assert(logged("net: resume window of 15000 ms expired at frame 3"));
}

/* A RESUME that arrives before the session is established is ignored, not
 * turned into a seed-mismatch failure of a session that never had a seed. */
static void case_resume_before_established_ignored(void) {
    printf("case: a RESUME before the handshake is ignored\n");
    setup();
    net.hs = HS_PENDING;
    net.seed = 0; /* the guest has no agreed seed yet */
    peer_resume(SESSION, SEED, 203, 195);
    assert(s_rc == RSM_NONE);
    assert(s_status == PC_NET_PEER_OK);
    assert(s_resume_sends == 0);
    assert(s_last_acked == ACKED);
    assert(logged("net: RESUME at frame 200 ignored, the session is not established (hs 1)"));
}

/* Through the tick body: an expired window ends the session with the log
 * line and the status this file produced before the phase existed. */
static void case_tick_window_expiry_disconnects(void) {
    printf("case: the tick body disconnects when the window expires\n");
    setup();
    PADStatus head[4];
    memset(head, 0, sizeof head);
    fresh_tick(head, true);
    assert(!net.active && net.sock == SOCK_INVALID);
    assert(pc_net_peer_status() == PC_NET_PEER_TIMEOUT);
    assert(logged("net: peer silent for 7000 ms at frame 200, leaving netplay"));
    assert(logged("net: disconnected at frame"));
    assert(net.frame == FRAME); /* the frame did not advance */
}

/* A refused resume ends it too, without the misleading silence line: the
 * peer was answering, its answer was just unusable. */
static void case_tick_resume_refused_disconnects(void) {
    printf("case: the tick body disconnects on a refused resume\n");
    setup();
    peer_resume(SESSION, SEED, 203, WROTE - RING - 5);
    assert(s_rc == RSM_FAILED);
    PADStatus head[4];
    memset(head, 0, sizeof head);
    fresh_tick(head, true);
    assert(!net.active && net.sock == SOCK_INVALID);
    assert(pc_net_peer_status() == PC_NET_PEER_RESUME);
    assert(!logged("peer silent"));
    assert(logged("net: disconnected at frame 199 (status 5)"));
}

/* The knob as pc_net_connect() reads it: 0 disables, a negative or
 * unparseable value falls back to the default. */
static void case_knob_parse(void) {
    printf("case: MELEE_NET_RECONNECT_MS parse\n");
    static const struct {
        const char* set;
        long want;
    } t[] = { { NULL, RECONNECT_MS },   { "0", 0 },
              { "2500", 2500 },         { "-1", RECONNECT_MS },
              { "", RECONNECT_MS },     { "later", RECONNECT_MS },
              { "2500x", RECONNECT_MS } };
    setenv("MELEE_NET_PORT", "0", 1); /* ephemeral: no collision with a real run */
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        if (t[i].set != NULL) {
            setenv("MELEE_NET_RECONNECT_MS", t[i].set, 1);
        } else {
            unsetenv("MELEE_NET_RECONNECT_MS");
        }
        s_rc_window_ms = -777;
        assert(pc_net_connect("127.0.0.1", 9, 0, SEED));
        printf("  %-8s -> %ld\n", t[i].set != NULL ? t[i].set : "(unset)", s_rc_window_ms);
        assert(s_rc_window_ms == t[i].want);
        pc_net_disconnect();
    }
    unsetenv("MELEE_NET_RECONNECT_MS");
    unsetenv("MELEE_NET_PORT");
}

int main(void) {
    case_resume_inside_ring();
    case_one_way_while_running();
    case_gap_past_ring();
    case_seed_mismatch();
    case_session_mismatch();
    case_window_expires();
    case_disabled();
    case_queue_full_retries();
    case_connect_timeout();
    case_handshake_pending_fails_fast();
    case_young_session_fails_fast();
    case_handshake_done_resumes_young();
    case_resume_before_established_ignored();
    case_tick_window_expiry_disconnects();
    case_tick_resume_refused_disconnects();
    case_knob_parse();
    printf("test_net_resume: ok\n");
    return 0;
}
