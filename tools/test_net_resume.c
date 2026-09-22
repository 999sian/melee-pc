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
#include "../src/sysdolphin/baselib/rumble.c"

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
#define FRAME 200 /* the frame the parked game thread is on */
#define WROTE 201 /* newest local frame in our ring */
#define HAVE 190  /* newest contiguous remote frame we hold */
#define ACKED 185 /* newest local frame we know the peer holds */

/* ---- harness state ---------------------------------------------------- */

static uint64_t s_now = 1000000000ull; /* virtual clock */
static void (*s_step)(void);           /* one scripted network step per wait-loop turn */
static char s_log[128 * 1024];
static size_t s_log_n;
static int s_resume_sends;  /* REL_RESUME messages we queued */
static int s_rel_fail;      /* fail that many pc_net_send_reliable calls */
static Resume s_resume_out; /* decoded payload of the last one */
static uint8_t s_resume_raw[sizeof(Resume)];
static Packet s_tx_pkt; /* last input packet handed to tx() */
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
    s_log_n += (size_t)snprintf(s_log + s_log_n, sizeof s_log - s_log_n, "%s\n", line);
}

uint64_t pc_sim_period_ns(void) {
    return 16666667ull;
}

Uint64 SDL_GetTicksNS(void) {
    return s_now;
}

void SDL_DelayNS(Uint64 ns) {
    (void)ns;
    s_now += 1000000ull; /* 1 ms per wait-loop turn */
    if (s_step != NULL) {
        s_step();
    }
}

/* The load-stall fixture's sleep; never armed here (no MELEE_NET_STALL_TEST),
 * but net.c references it. */
void SDL_Delay(Uint32 ms) {
    s_now += (Uint64)ms * 1000000ull;
}

SDL_Mutex* SDL_CreateMutex(void) {
    return NULL;
}
void SDL_LockMutex(SDL_Mutex* m) {
    (void)m;
}
void SDL_UnlockMutex(SDL_Mutex* m) {
    (void)m;
}
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback cb, void* ud) {
    (void)interval;
    (void)cb;
    (void)ud;
    return 1;
}
bool SDL_RemoveTimer(SDL_TimerID id) {
    (void)id;
    return true;
}
SDL_ThreadID SDL_GetCurrentThreadID(void) {
    return 1;
}
Uint64 SDL_GetPerformanceCounter(void) {
    return 424242;
}

/* Exercise the shipping wire codecs and address comparison. */
#include "../src/pc/net_wire.c"

/* The tick's disc drain; no disc in this harness, so always idle. */
/* The freeze watchdog; no timer thread in this harness. */
void net_watchdog_arm(void) {}
void net_watchdog_tick(int32_t frame) {
    (void)frame;
}
void net_watchdog_heartbeat(void) {}
int aurora_dvd_inflight(void) {
    return 0;
}
int aurora_arq_inflight(void) {
    return 0;
}
void net_addr_text(const struct sockaddr* sa, char* out, size_t cap) {
    (void)sa;
    snprintf(out, cap, "peer");
}
/* net_sim.c */
void tx(const void* buf, size_t len) {
    const uint8_t* p = buf;
    net.tx_pkts++;
    if (p[0] == 'M' && len >= offsetof(Packet, pads)) {
        memcpy(&s_tx_pkt, buf, len < sizeof s_tx_pkt ? len : sizeof s_tx_pkt);
        wire_packet(&s_tx_pkt);
        s_tx_pkt_valid = true;
        net.tx_inputs++;
    }
}
void tx_flush(void) {}
int held_put(Held* held, const void* buf, size_t len, uint64_t release_ns) {
    (void)held;
    (void)buf;
    (void)len;
    (void)release_ns;
    return -1;
}
Held* held_due(Held* held, uint64_t now) {
    (void)held;
    (void)now;
    return NULL;
}
void sim_env(uint16_t bind_port) {
    (void)bind_port;
}
void sim_reset(void) {}

/* net_reliable.c: the transmit side is what the resume machine drives. */
bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    assert(type == REL_RESUME);
    assert(len == (int)sizeof(Resume));
    if (s_rel_fail > 0) {
        s_rel_fail--;
        return false; /* lane queue full */
    }
    memcpy(s_resume_raw, payload, (size_t)len);
    memcpy(&s_resume_out, payload, sizeof s_resume_out);
    wire_resume(&s_resume_out);
    s_resume_sends++;
    return true;
}
int pc_net_recv_reliable(uint8_t* type, void* payload, int max) {
    (void)type;
    (void)payload;
    (void)max;
    return -1;
}
void rel_service(void) {}
void on_rel(const Rel* r, int n) {
    (void)r;
    (void)n;
}
void on_rel_ack(const RelAck* k) {
    (void)k;
}
void rel_reset(void) {}

/* net_handshake.c / net_sync.c */
void rules_restore(void) {}
void handshake_direct(void) {}
void adv_note(int remote_adv, int local_adv) {
    (void)remote_adv;
    (void)local_adv;
}
void jitter_note(uint32_t rtt) {
    (void)rtt;
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
    (void)i;
    return false;
}
const char* state_line(int32_t frame) {
    (void)frame;
    return "";
}

/* net_snapshot.c */
static Snapshot s_snaps[SNAPS];
static uint8_t s_snap_storage[SNAPS];
static int32_t s_snapshot_fail_at = -1;
static int32_t s_restored = -1;
static bool s_state_missing;
static bool s_restore_rumble_fixture;
static HSD_PadRumbleListData s_rumble_nodes[2];
static HSD_PadRumbleListData s_saved_rumble_nodes[2];
static HSD_RumbleData s_saved_rumble_heads[4];
static RumbleInfo s_saved_rumble_info;
bool snapshot_take(Snapshot* s, int32_t frame) {
    s->frame = -1;
    if (s_state_missing || frame == s_snapshot_fail_at) {
        return false;
    }
    if (s_restore_rumble_fixture) {
        memcpy(s_saved_rumble_nodes, s_rumble_nodes, sizeof s_rumble_nodes);
        memcpy(s_saved_rumble_heads, HSD_Rumble_804C22E0, sizeof s_saved_rumble_heads);
        s_saved_rumble_info = HSD_PadLibData.rumble_info;
    }
    s->buf = &s_snap_storage[frame % SNAPS];
    s->frame = frame;
    return true;
}
const char* snapshot_unusable(const Snapshot* s) {
    (void)s;
    return NULL;
}
void snapshot_restore(const Snapshot* s) {
    s_restored = s->frame;
    if (s_restore_rumble_fixture) {
        /* Same lifetime as gmmain.c's static pool and rumble.c's active heads. */
        memcpy(s_rumble_nodes, s_saved_rumble_nodes, sizeof s_rumble_nodes);
        memcpy(HSD_Rumble_804C22E0, s_saved_rumble_heads, sizeof s_saved_rumble_heads);
        HSD_PadLibData.rumble_info = s_saved_rumble_info;
    }
}
Snapshot* snap_slot(int32_t f) {
    return &s_snaps[f % SNAPS];
}
void snaps_free(void) {
    memset(s_snaps, 0, sizeof s_snaps);
    for (int i = 0; i < SNAPS; i++) {
        s_snaps[i].frame = -1;
    }
}
void snap_stats_report(void) {}
const char* snapshot_state_region_missing(void) {
    return s_state_missing ? "no state region" : NULL;
}
uint32_t frame_checksum(const PADStatus* head) {
    (void)head;
    return 0;
}
void record_state(const PADStatus* head, int32_t frame) {
    (void)head;
    (void)frame;
}
void dump_states_around(int32_t frame) {
    (void)frame;
}
void record_open(void) {}
bool record_active(void) {
    return false;
}
void replay_feed(PADStatus* head) {
    (void)head;
}
void record_frame(const PADStatus* head, uint32_t ck, int32_t f) {
    (void)head;
    (void)ck;
    (void)f;
}
void record_confirm(int32_t upto) {
    (void)upto;
}
bool record_replay_scene_hold(int32_t frame) {
    (void)frame;
    return false;
}
void synctest_before_tick(void) {}
bool synctest_after_tick(void) {
    return false;
}
void resim_note(int ticks) {
    (void)ticks;
}
const char* snapshot_describe(const Snapshot* s, char* buf, size_t n) {
    (void)s;
    (void)n;
    return buf;
}

/* the game */
struct GameSceneInfo* gm_804D6720;
/* net.c reads rules.game_speed to decide whether rollback is sound; the
 * harness never runs a match, so a default-constructed one at speed 1.0 is
 * what the tests want. */
StartMeleeData gmVsMelee_StartData = {.rules = {.game_speed = 1.0F}};
PadLibData HSD_PadLibData;
static u32 s_seed_val;
u32* HSD_RandSeedPtr = &s_seed_val;
BOOL OSDisableInterrupts(void) {
    return 1;
}
BOOL OSRestoreInterrupts(BOOL level) {
    return level;
}

u32 PADRead(PADStatus* pads) {
    memset(pads, 0, 4 * sizeof(*pads));
    return 0;
}

void PADControlMotor(u32 chan, u32 cmd) {
    (void)chan;
    (void)cmd;
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
    assert(bind(sock, (struct sockaddr*)&a, sizeof a) == 0);
    assert(sock_nonblock(sock));
    s_snapshot_fail_at = s_restored = -1;
    s_state_missing = false;
    gm_804D6720 = NULL;
    session_reset();
    net.sock = sock;
    net.active = true;
    net.local = 0;
    net.remote = 1;
    net.session = SESSION;
    net.seed = SEED;
    /* Mid-match means the match was agreed: session_established() is the
     * handshake state alone now, for a direct session as much as a lobby
     * one, and the resume phase only opens for an established session. */
    net.hs = HS_DONE;
    net.start_frame = 120;
    net.delay = net.delay_next = 2;
    net.frame = FRAME;
    net.tick_frame = FRAME - 1;
    /* The peer's address: loopback discard, so sendto() succeeds and nothing
     * ever answers. */
    struct sockaddr_in* p = (struct sockaddr_in*)&net.peer;
    memset(p, 0, sizeof *p);
    p->sin_family = AF_INET;
    p->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    p->sin_port = htons(9);
    net.peer_len = sizeof *p;
    s_wrote = WROTE;
    s_remote_have = HAVE;
    s_last_acked = ACKED;
    s_heard = true;
    /* wait_remote() times silence from the last datagram, not from the start
     * of the wait, so an established session has to carry both halves of
     * "we have heard from this peer" -- rx_dispatch() sets them together. */
    s_last_rx_ns = s_now;
    s_rc_window_ms = RECONNECT_MS;
    for (int32_t f = 0; f <= WROTE; f++) {
        s_local_ring[f & (RING - 1)].button = (uint16_t)(0x1000 + f);
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

/* Both helpers below stand in for a datagram off the socket, so they carry
 * rx_dispatch()'s tail: the peer has been heard from, now. wait_remote()
 * reads that clock to tell a loading peer from a lost one. */
static void peer_heard(void) {
    s_heard = true;
    s_last_rx_ns = s_now;
}

/* The peer's half of the exchange. */
static void peer_resume(uint32_t session, uint32_t seed, int32_t newest, int32_t have) {
    Resume r = {session, seed, newest, have, newest - 2};
    wire_resume(&r);
    peer_heard();
    net_resume_rel(&r, (int)sizeof r);
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
    pk.count = (uint8_t)(last - first + 1);
    for (int i = 0; i < pk.count; i++) {
        pk.pads[i].button = (uint16_t)(0x2000 + first + i);
    }
    peer_heard();
    on_inputs(&pk, (int)(offsetof(Packet, pads) + (size_t)pk.count * sizeof(WirePad)));
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
    } else if (s_did_exchange && !s_did_pads &&
               (s_now - s_t0) / 1000000ull >= (STALL_TIMEOUT_MS + 1000))
    {
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
    assert(pc_net_quality() == 2);           /* the stall itself, as before */
    assert(s_red_floor == REDUNDANCY_FLOOR); /* back to the ordinary cadence */
    assert(s_remote_have == 199);
    /* Their ring refilled into ours, frame by frame. */
    for (int32_t f = HAVE + 1; f <= 199; f++) {
        assert(s_remote_ring[f & (RING - 1)].button == (uint16_t)(0x2000 + f));
    }
    /* Ours refilled toward them: the reconnect send starts above the frame
     * the peer reported and carries every frame we hold, up to REDUNDANCY. */
    assert(s_tx_pkt_valid);
    assert(s_tx_pkt.first == 196 && s_tx_pkt.newest == WROTE);
    assert(s_tx_pkt.count == WROTE - 196 + 1);
    for (int i = 0; i < s_tx_pkt.count; i++) {
        assert(s_tx_pkt.pads[i].button == (uint16_t)(0x1000 + 196 + i));
    }
    assert(logged("net: interrupted at frame 200 (peer silent 3000 ms), reconnecting"));
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
    assert(logged("net: resume window of 3000 ms expired at frame 200"));
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

/* A peer whose game thread is inside a load keeps its sender running (net.c
 * tx_timer, every 7 ms) while no new frame is ever written. That is not a
 * lost peer and must not open a reconnect phase, however long it lasts:
 * timing it from the start of the wait instead of from the last datagram is
 * what dropped phone<->PC sessions at the CSS->match hand-off, after a 7 s
 * freeze and a resume window that could not be answered. */
static void step_peer_talks_without_advancing(void) {
    /* Re-deliver a frame we already hold: liveness, no progress. */
    peer_pads(HAVE, HAVE);
    if ((s_now - s_t0) / 1000000ull >= 40000) {
        peer_pads(HAVE + 1, 199); /* the load finished; the wait can end */
    }
}

static void case_loading_peer_is_not_silent(void) {
    printf("case: a peer that talks but does not advance is not silent\n");
    setup();
    s_t0 = s_now;
    s_step = step_peer_talks_without_advancing;
    assert(wait_remote(199)); /* the session survives a 40 s load */
    assert((s_now - s_t0) / 1000000ull >= 40000);
    assert(s_rc == RSM_NONE);
    assert(s_resume_sends == 0);
    assert(!logged("reconnecting"));
    assert(!logged("peer silent"));
    assert(s_status == PC_NET_PEER_OK);
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

/* The same for the host_dies shape itself: a session a few frames old that
 * has adopted the host's session id but agreed nothing yet -- a lobby guest
 * before pc_lan_poll() claims the handshake, or a direct session still
 * waiting for the game to fill its rules in. Both read HS_IDLE. */
static void case_young_session_fails_fast(void) {
    printf("case: a session a few frames old fails fast\n");
    setup();
    net.hs = HS_IDLE;
    net.frame = 3; /* the frame host_dies interrupted at */
    net.tick_frame = 2;
    s_wrote = 4;
    s_remote_have = 0;
    s_last_acked = -1;
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
    assert(logged("net: resume window of 3000 ms expired at frame 3"));
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
    assert(logged("net: peer silent for 3000 ms at frame 200, leaving netplay"));
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
    } t[] = {{NULL, RECONNECT_MS}, {"0", 0}, {"2500", 2500}, {"-1", RECONNECT_MS},
        {"", RECONNECT_MS}, {"later", RECONNECT_MS}, {"2500x", RECONNECT_MS}};
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

/* One datagram out of the probe socket, tagged the way tx() tags ours
 * (net_sim.c): the receiver measures the message as the datagram minus
 * NET_MAC_LEN, so an untagged one is not even the right shape. */
static void send_dg(sock_t s, const struct sockaddr_in* dst, const void* body, size_t len) {
    uint8_t dg[sizeof(Rel) + NET_MAC_LEN];
    memcpy(dg, body, len);
    net_mac_stamp(dg, len);
    assert(sendto(s, (const char*)dg, len + NET_MAC_LEN, 0, (const struct sockaddr*)dst,
               sizeof *dst) == (int)(len + NET_MAC_LEN));
}

/* The probe socket: a second socket on loopback whose address the fixture
 * can install as the peer's, so a test can put a real datagram through the
 * real recvfrom path. */
static sock_t probe_open(struct sockaddr_in* dst, struct sockaddr_in* src) {
    socklen_t size = sizeof *dst;
    assert(getsockname(net.sock, (struct sockaddr*)dst, &size) == 0);
    sock_t sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sender != SOCK_INVALID);
    memset(src, 0, sizeof *src);
    src->sin_family = AF_INET;
    src->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(sender, (struct sockaddr*)src, sizeof *src) == 0);
    size = sizeof *src;
    assert(getsockname(sender, (struct sockaddr*)src, &size) == 0);
    return sender;
}

static void case_receive_identity(void) {
    printf("case: only the selected peer can fail compatibility\n");
    setup();
    struct sockaddr_in dst, src;
    sock_t sender = probe_open(&dst, &src);
    Ack a = {{'A', 255, SESSION, 1}, 0, -1};
    wire_hdr(&a.h);
    wire_ack(&a);
    send_dg(sender, &dst, &a, sizeof a);
    recv_inputs();
    assert(!s_peer_left && net.session == SESSION);

    memcpy(&net.peer, &src, sizeof src);
    a.h.magic = '?';
    send_dg(sender, &dst, &a, sizeof a);
    recv_inputs();
    assert(!s_peer_left);
    a.h.magic = 'A';
    a.h.session = htonl(SESSION + 1);
    send_dg(sender, &dst, &a, sizeof a);
    recv_inputs();
    assert(!s_peer_left);
    a.h.session = htonl(SESSION);
    send_dg(sender, &dst, &a, sizeof a);
    recv_inputs();
    assert(s_peer_left && s_status == PC_NET_PEER_INCOMPATIBLE);
    sock_close(sender);
    pc_net_disconnect();
}

/* The session id is the guest's one piece of identity it cannot be told in
 * advance, and it used to be learned from the first well-shaped datagram of
 * ANY type. The MAC gate above is a grace window while no key exists (the
 * default), so one forged 16-byte RelAck from anywhere won the race:
 * net.session became the attacker's, every genuine host datagram then failed
 * the session check for the rest of the session (learn_session requires
 * net.session == 0, so there was no second chance), and the match died on the
 * handshake timeout. Only the message that carries the session in the first
 * place -- a RULES -- may establish it. */
static void case_session_learned_only_from_rules(void) {
    printf("case: only a RULES teaches the guest its session id\n");
    setup();
    net.local = 1; /* the guest */
    net.remote = 0;
    net.session = 0; /* not told one yet: this is the learning window */
    net.hs = HS_PENDING;
    struct sockaddr_in dst, src;
    sock_t sender = probe_open(&dst, &src);
    memcpy(&net.peer, &src, sizeof src);

    Ack a = {{'A', WIRE_VERSION, SESSION, 0}, 0, -1};
    wire_hdr(&a.h);
    wire_ack(&a);
    send_dg(sender, &dst, &a, sizeof a);
    recv_inputs();
    assert(net.session == 0);

    Rel r = {{'R', WIRE_VERSION, SESSION, 0}, 0, REL_RULES, 0, {0}};
    r.len = (uint16_t)sizeof(Rules); /* learning requires a full RULES shape */
    wire_hdr(&r.h);
    wire_rel(&r);
    send_dg(sender, &dst, &r, offsetof(Rel, payload) + sizeof(Rules));
    recv_inputs();
    assert(net.session == SESSION);
    sock_close(sender);
    pc_net_disconnect();
}

/* The destination guard for issue #87: a pairing dialled a /8 network base
 * -- the peer's first octet with the rest zeroed -- instead of the address
 * its datagrams had arrived from. The session then parked the render thread
 * for the whole connect wait, which the reporter saw as a crash. Addresses
 * here are RFC 5737 documentation range, same shape. */
static void case_connect_destination(void) {
    printf("case: only a unicast host address may be dialled\n");
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(16122);
    a.sin_addr.s_addr = htonl(0xC0000205u); /* 192.0.2.5: a real host */
    assert(addr_is_host((struct sockaddr*)&a));
    a.sin_addr.s_addr = htonl(0xC0A80105u); /* 192.168.1.5: a LAN peer is fine */
    assert(addr_is_host((struct sockaddr*)&a));
    a.sin_addr.s_addr = htonl(0xC0000000u); /* 192.0.0.0: the /8 base shape */
    assert(!addr_is_host((struct sockaddr*)&a));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    assert(!addr_is_host((struct sockaddr*)&a));
    a.sin_addr.s_addr = htonl(0xFFFFFFFFu); /* broadcast */
    assert(!addr_is_host((struct sockaddr*)&a));
    a.sin_addr.s_addr = htonl(0xE0000001u); /* 224.0.0.1 multicast */
    assert(!addr_is_host((struct sockaddr*)&a));
    a.sin_addr.s_addr = htonl(0xF0000001u); /* 240.0.0.1 reserved */
    assert(!addr_is_host((struct sockaddr*)&a));
    a.sin_addr.s_addr = htonl(0xC0000205u);
    a.sin_port = 0;
    assert(!addr_is_host((struct sockaddr*)&a));
}

static void case_old_protocol(void) {
    printf("case: protocol 5 scene peers are incompatible\n");
    setup();
    struct sockaddr_in dst, src;
    sock_t sender = probe_open(&dst, &src);
    memcpy(&net.peer, &src, sizeof src);
    Ack a = {{'A', 5, SESSION, 1}, 0, -1};
    wire_hdr(&a.h);
    wire_ack(&a);
    send_dg(sender, &dst, &a, sizeof a);
    recv_inputs();
    assert(s_peer_left && s_status == PC_NET_PEER_INCOMPATIBLE);
    sock_close(sender);
    pc_net_disconnect();
}

/* The authentication gate, through the real socket: a datagram whose tag is
 * wrong, or which carries none at all, must be counted and dropped before
 * anything downstream of it moves. Only the ack path is used, because its
 * effect on session state (s_last_acked) is one number to read back. */
static void case_bad_mac_rejected(void) {
    printf("case: a datagram with a wrong or absent MAC is refused and counted\n");
    setup();
    struct sockaddr_in dst, src;
    sock_t sender = probe_open(&dst, &src);
    memcpy(&net.peer, &src, sizeof src);
    net_key_direct("shared secret");
    assert(net_key_ready() && !s_mac_seen);
    /* A pinned key refuses from the very first datagram: it is not waiting
     * for a handshake leg, so nothing is owed the benefit of the doubt.
     * Without that, an attacker who never sends a valid tag would hold the
     * bootstrap grace open for the whole session. */
    Ack forged = {{'A', WIRE_VERSION, SESSION, 1}, 0, ACKED + 2};
    wire_hdr(&forged.h);
    wire_ack(&forged);
    uint8_t first[sizeof(Ack) + NET_MAC_LEN];
    memcpy(first, &forged, sizeof forged);
    memset(first + sizeof forged, 0, NET_MAC_LEN);
    assert(sendto(sender, (const char*)first, sizeof first, 0, (struct sockaddr*)&dst,
               sizeof dst) == (int)sizeof first);
    recv_inputs();
    assert(s_rx_bad_mac == 1 && !s_mac_seen && s_last_acked == ACKED);
    Ack a = {{'A', WIRE_VERSION, SESSION, 1}, 0, ACKED + 4};
    wire_hdr(&a.h);
    wire_ack(&a);
    send_dg(sender, &dst, &a, sizeof a);
    recv_inputs();
    /* Authenticated: accepted, and the session will not take an unsigned
     * datagram again. */
    assert(s_mac_seen && s_rx_bad_mac == 1 && s_last_acked == ACKED + 4);

    uint8_t dg[sizeof(Ack) + NET_MAC_LEN];
    Ack b = {{'A', WIRE_VERSION, SESSION, 1}, 0, ACKED + 9};
    wire_hdr(&b.h);
    wire_ack(&b);
    memcpy(dg, &b, sizeof b);
    net_mac_stamp(dg, sizeof b);
    dg[sizeof b] ^= 0x40; /* one bit of the tag */
    assert(sendto(sender, (const char*)dg, sizeof dg, 0, (struct sockaddr*)&dst, sizeof dst) ==
           (int)sizeof dg);
    recv_inputs();
    assert(s_rx_bad_mac == 2 && s_last_acked == ACKED + 4);

    /* No tag at all: the wrong shape for its magic, so it dies one check
     * earlier, but it must still never be taken. */
    assert(sendto(sender, (const char*)&b, sizeof b, 0, (struct sockaddr*)&dst, sizeof dst) ==
           (int)sizeof b);
    recv_inputs();
    assert(s_rx_bad_mac == 2 && s_rx_malformed == 1 && s_last_acked == ACKED + 4);

    /* Known-positive: the same ack with the tag left alone lands, so the two
     * rejections above are the tag and nothing else about the datagram. */
    send_dg(sender, &dst, &b, sizeof b);
    recv_inputs();
    assert(s_last_acked == ACKED + 9 && s_rx_bad_mac == 2);

    /* A tag from another session's key is a forgery like any other. */
    net_key_clear();
    net_key_direct("another secret");
    Ack c = {{'A', WIRE_VERSION, SESSION, 1}, 0, ACKED + 14};
    wire_hdr(&c.h);
    wire_ack(&c);
    memcpy(dg, &c, sizeof c);
    net_mac_stamp(dg, sizeof c);
    net_key_clear();
    net_key_direct("shared secret");
    assert(sendto(sender, (const char*)dg, sizeof dg, 0, (struct sockaddr*)&dst, sizeof dst) ==
           (int)sizeof dg);
    recv_inputs();
    assert(s_rx_bad_mac == 3 && s_last_acked == ACKED + 9);
    assert(logged_count("bad MAC") == 1); /* one line per session, not one per datagram */
    sock_close(sender);
    pc_net_disconnect();
    assert(!net_key_ready()); /* the key does not outlive the session */
}

static HSD_PadData s_test_queue[8];
static GameSceneInfo s_test_scene;
static void fight_setup(void) {
    setup();
    s_test_scene.scene_kind = GS_VS;
    gm_804D6720 = &s_test_scene;
    s_scene_last = GS_VS;
    memset(s_test_queue, 0, sizeof s_test_queue);
    HSD_PadLibData = (PadLibData){0};
    HSD_PadLibData.queue = s_test_queue;
    HSD_PadLibData.qnum = 8;
    HSD_PadLibData.qcount = 1;
}

static void deliver_changed_input(void) {
    Packet pk = {0};
    pk.first = s_remote_have + 1;
    pk.newest = FRAME;
    pk.count = FRAME - pk.first + 1;
    pk.ck_frame = -1;
    for (int i = 0; i < pk.count; i++) {
        pk.pads[i].button = 0x100;
    }
    on_inputs(&pk, offsetof(Packet, pads) + pk.count * sizeof(WirePad));
    s_step = NULL;
}

static void case_snapshot_failure(void) {
    printf("case: snapshot failure waits for real input and retains earlier rollback\n");
    fight_setup();
    s_remote_have = FRAME - 2;
    assert(snapshot_take(snap_slot(FRAME - 1), FRAME - 1));
    s_snapshot_fail_at = FRAME;
    s_step = deliver_changed_input;
    fresh_tick(s_test_queue[0].stat, true);
    assert(net.active && net.frame == FRAME + 1);
    assert(s_remote_have == FRAME);
    assert(s_test_queue[0].stat[1].button == 0x100);
    assert(s_rb_frame == FRAME - 1);
    assert(rollback_to(s_rb_frame));
    assert(s_restored == FRAME - 1 && s_rb_lost == 0);
    pc_net_disconnect();
}

static void case_snapshot_missing(void) {
    printf("case: platforms without snapshots start in lockstep\n");
    fight_setup();
    s_state_missing = true;
    session_reset();
    net.frame = FRAME;
    net.tick_frame = FRAME - 1;
    s_scene_last = GS_VS;
    s_wrote = WROTE;
    s_remote_have = FRAME - 1;
    s_step = deliver_changed_input;
    fresh_tick(s_test_queue[0].stat, true);
    assert(net.active && net.frame == FRAME + 1);
    assert(s_remote_have == FRAME);
    assert(s_test_queue[0].stat[1].button == 0x100);
    assert(s_rb_lost == 0 && s_rb_frame == -1);
    pc_net_disconnect();
    s_state_missing = false;
}

static void case_resim_snapshot_failure(void) {
    printf("case: a failed re-simulation snapshot also waits for real input\n");
    fight_setup();
    net.resim = true;
    net.frame = FRAME + 1;
    net.tick_frame = FRAME - 1;
    s_remote_have = FRAME - 1;
    s_snapshot_fail_at = FRAME;
    s_step = deliver_changed_input;
    assert(resim_prepare(FRAME));
    assert(s_remote_have == FRAME && net.tick_frame == FRAME);
    assert(pad_head()[1].button == 0x100);
    pc_net_disconnect();

    fight_setup();
    net.resim = true;
    net.frame = FRAME + 1;
    net.tick_frame = FRAME - 1;
    s_remote_have = FRAME - 1;
    s_snapshot_fail_at = FRAME;
    assert(!resim_prepare(FRAME)); /* absent peer: timeout, no speculative re-run */
    assert(!net.active && !net.resim);
    assert(pc_net_peer_status() == PC_NET_PEER_TIMEOUT);
}

static void case_rollback_rumble_ownership(void) {
    printf("case: rollback keeps the rumble free list with rewound nodes\n");
    fight_setup();
    net.frame = FRAME + 1;
    net.tick_frame = FRAME;
    s_remote_have = FRAME;
    HSD_PadRumbleInit(2, s_rumble_nodes);
    assert(HSD_PadRumbleAdd(0, 1, -2, 0, NULL));
    assert(HSD_Rumble_804C22E0[0].listdatap == &s_rumble_nodes[0]);
    s_restore_rumble_fixture = true;
    assert(snapshot_take(snap_slot(FRAME), FRAME));
    /* A real free moves the discarded timeline's free head onto node 0. */
    HSD_PadRumbleRemove(0);
    assert(HSD_PadLibData.rumble_info.listdatap == &s_rumble_nodes[0]);
    assert(rollback_to(FRAME));
    int mismatches = HSD_PadLibData.rumble_info.listdatap != &s_rumble_nodes[1];
    printf("  rumble ownership mismatches: %d\n", mismatches);
    assert(mismatches == 0);
    /* Actual add must allocate node 1 and leave node 0's old command intact. */
    assert(HSD_PadRumbleAdd(1, 2, -2, 0, NULL));
    assert(HSD_Rumble_804C22E0[1].listdatap == &s_rumble_nodes[1]);
    assert(HSD_Rumble_804C22E0[0].listdatap->id == 1);
    /* Known-positive: inject the old head, then show one node owned twice. */
    snapshot_restore(snap_slot(FRAME));
    HSD_PadLibData.rumble_info.listdatap = &s_rumble_nodes[0];
    assert(HSD_PadRumbleAdd(1, 2, -2, 0, NULL));
    int injected = HSD_Rumble_804C22E0[0].listdatap == HSD_Rumble_804C22E0[1].listdatap;
    assert(injected - mismatches == 1);
    snapshot_restore(snap_slot(FRAME));
    s_restore_rumble_fixture = false;
    printf("  rumble ownership injection: mismatch count delta exactly 1\n");
    pc_net_disconnect();
}

static void case_seed_reset(void) {
    printf("case: reconnect preserves the new session seed\n");
    setup();
    s_seed_have = true;
    s_seed_after_tick = 123;
    setenv("MELEE_NET_PORT", "0", 1);
    assert(pc_net_connect("127.0.0.1", 9, 0, 456));
    seed_out_of_tick_check();
    assert(*HSD_RandSeedPtr == 456);
    pc_net_disconnect();
    unsetenv("MELEE_NET_PORT");
}

int main(int argc, char** argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "identity") == 0)
            case_receive_identity();
        else if (strcmp(argv[1], "protocol") == 0)
            case_old_protocol();
        else if (strcmp(argv[1], "snapshot") == 0)
            case_snapshot_failure();
        else if (strcmp(argv[1], "missing") == 0)
            case_snapshot_missing();
        else if (strcmp(argv[1], "rumble") == 0)
            case_rollback_rumble_ownership();
        else if (strcmp(argv[1], "seed") == 0)
            case_seed_reset();
        else if (strcmp(argv[1], "mac") == 0)
            case_bad_mac_rejected();
        else if (strcmp(argv[1], "learn") == 0)
            case_session_learned_only_from_rules();
        else if (strcmp(argv[1], "dest") == 0)
            case_connect_destination();
        else
            return 2;
        return 0;
    }
    case_resume_inside_ring();
    case_one_way_while_running();
    case_gap_past_ring();
    case_bad_mac_rejected();
    case_seed_mismatch();
    case_session_mismatch();
    case_window_expires();
    case_disabled();
    case_queue_full_retries();
    case_connect_timeout();
    case_loading_peer_is_not_silent();
    case_handshake_pending_fails_fast();
    case_young_session_fails_fast();
    case_handshake_done_resumes_young();
    case_resume_before_established_ignored();
    case_tick_window_expiry_disconnects();
    case_tick_resume_refused_disconnects();
    case_knob_parse();
    case_receive_identity();
    case_session_learned_only_from_rules();
    case_connect_destination();
    case_old_protocol();
    case_snapshot_failure();
    case_snapshot_missing();
    case_resim_snapshot_failure();
    case_seed_reset();
    case_rollback_rumble_ownership();
    printf("test_net_resume: ok\n");
    return 0;
}
