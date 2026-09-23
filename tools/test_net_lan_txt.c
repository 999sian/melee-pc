/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the TXT parser and the peer table in src/pc/net_lan.c: the
 * module is included whole and its real on_record() is called with hand-built
 * mDNS answers, so mdns.h's own TXT splitting, parse_txt(), the compatibility
 * test and the s_peers paths are all on the tested path. Every dependency
 * outside the module is stubbed below (the pattern of tools/test_net_reliable.c).
 *
 * The disc identity is hashed from the stubbed boot info + main.dol, so
 * `image` picks which of two images this process pretends to be running; the
 * two invocations must print different disc ids.
 *
 * From the repo root (build/compile_commands.json supplies the real include
 * paths; tools/net_lan_txt_test.py --c-harness builds and runs it):
 *   cc -DTARGET_PC=1 -DXXH_INLINE_ALL -fsanitize=address,undefined \
 *      -I src -I extern/aurora/include -I <sdl include> -I <xxhash include> \
 *      tools/test_net_lan_txt.c -o /tmp/test_net_lan_txt && /tmp/test_net_lan_txt
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stubs, ahead of the module ---------------------------------------- */

/* not <assert.h>: the decomp's debug.h owns __assert, and -DNDEBUG must not blind this */
#define assert(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_mutex.h>
#include <dolphin/dvd.h>

static int g_rejects;      /* "dropped a malformed record" lines */
static int g_found;        /* "lan: found" lines */
static int g_incompat;     /* "theirs: proto ..." detail lines */
static int g_lost;         /* "lan: lost" lines */
static int g_full;         /* "lobby full" lines */
static char g_last[512];   /* last line logged */
static char g_detail[512]; /* last incompatible-detail line */
static char g_refuse[512]; /* last "refusing the match" line */

void pc_log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last, sizeof g_last, fmt, ap);
    va_end(ap);
    g_rejects += strstr(g_last, "dropped a malformed record") != NULL;
    g_found += strncmp(g_last, "lan: found", 10) == 0;
    g_lost += strncmp(g_last, "lan: lost", 9) == 0;
    g_full += strstr(g_last, "lobby full") != NULL;
    if (strstr(g_last, "theirs: proto") != NULL) {
        g_incompat++;
        snprintf(g_detail, sizeof g_detail, "%s", g_last);
    }
    if (strstr(g_last, "refusing the match") != NULL) {
        snprintf(g_refuse, sizeof g_refuse, "%s", g_last);
    }
}

uint64_t pc_install_id(void) {
    return 0x1111222233334444ull;
}
const char* pc_app_rev(void) {
    return "0.1.2-test";
}

void pc_android_multicast_lock_acquire(void) {}
void pc_android_multicast_lock_release(void) {}
const char* pc_android_device_name(void) {
    return NULL;
}

static int g_connections, g_player;
static void (*g_timer_stop_check)(void);
static bool g_barrier_received;
static int32_t g_frame = -1, g_scheduled = -1;
static int g_polls, g_ready_after;
static bool g_handshake_pending;
static bool g_active = true;
static int g_disconnect_after;
int32_t pc_net_frame(void) {
    return g_frame;
}
int32_t pc_net_start_frame(void) {
    return g_scheduled;
}
void pc_net_poll(void) {
    if (++g_polls == g_ready_after)
        g_barrier_received = true;
    if (g_polls == g_disconnect_after)
        g_active = false;
}

bool pc_net_connect(const char* ip, uint16_t port, int player, uint32_t seed) {
    (void)ip;
    (void)port;
    g_connections++;
    g_player = player;
    (void)seed;
    return true;
}
void pc_net_disconnect(void) {}
bool pc_net_active(void) {
    return g_active;
}
int pc_net_peer_status(void) {
    return 0;
}
void pc_net_peer_status_clear(void) {}
int pc_net_handshake_state(void) {
    return 0;
}
int pc_net_quality(void) {
    return 0;
}
static int g_sent_scene = -3; /* payload of the last READY_BARRIER we sent */
bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    if (type == 0x11) {
        g_sent_scene = len == 1 ? (int8_t)((const uint8_t*)payload)[0] : -3;
    }
    return true;
}
static int g_scene = 45, g_peer_scene = 45; /* both in GS_ONLINE_LOBBY */
int scene_kind(void) {
    return g_scene;
}
int pc_net_recv_reliable(uint8_t* type, void* payload, int max) {
    assert(max >= 1);
    if (g_barrier_received) {
        g_barrier_received = false;
        *type = 0x11;
        *(uint8_t*)payload = (uint8_t)g_peer_scene;
        return 1;
    }
    return -1;
}
bool pc_net_host_match(uint32_t seed, int32_t* start_frame) {
    (void)seed;
    if (g_handshake_pending && g_polls >= 2) {
        *start_frame = g_scheduled;
        return true;
    }
    return false;
}
bool pc_net_guest_wait_match(uint32_t* seed, int32_t* start_frame) {
    (void)seed;
    (void)start_frame;
    return false;
}

SDL_Mutex* SDL_CreateMutex(void) {
    return (SDL_Mutex*)1;
}
void SDL_LockMutex(SDL_Mutex* m) {
    (void)m;
}
void SDL_UnlockMutex(SDL_Mutex* m) {
    (void)m;
}

static uint64_t s_now = 1000;
void SDL_DelayNS(Uint64 ns) {
    s_now += ns;
}
Uint64 SDL_GetTicksNS(void) {
    return s_now;
}
Uint64 SDL_GetTicks(void) {
    return s_now / 1000000;
}
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback cb, void* ud) {
    (void)interval;
    (void)cb;
    (void)ud;
    return 1;
}
bool SDL_RemoveTimer(SDL_TimerID id) {
    (void)id;
    if (g_timer_stop_check != NULL) {
        g_timer_stop_check();
    }
    return true;
}

/* Two pretend disc images. `dol` is what DVDGetDOLLocation hands back, so the
 * disc identity has to change with it. */
static int s_image;
static DVDDiskID s_disk;
static uint8_t s_dol[4096];

DVDDiskID* DVDGetCurrentDiskID(void) {
    return &s_disk;
}
const u8* DVDGetDOLLocation(s32* out_size) {
    *out_size = (s32)sizeof s_dol;
    return s_dol;
}
s32 aurora_dvd_base_entry_count(void) {
    return s_image == 0 ? 1233 : 1240;
}

static void set_image(int which) {
    s_image = which;
    memset(&s_disk, 0, sizeof s_disk);
    memcpy(s_disk.gameName, which == 0 ? "GALE" : "GALP", 4);
    memcpy(s_disk.company, "01", 2);
    s_disk.diskNumber = 0;
    s_disk.gameVersion = which == 0 ? 2 : 0;
    for (size_t i = 0; i < sizeof s_dol; i++) {
        s_dol[i] = (uint8_t)(i * 7 + which);
    }
}

#include "../src/pc/net_lan.c"

/* ---- mDNS answers ------------------------------------------------------ */

static uint8_t s_pkt[2048];

static size_t put_name(size_t o, const char* dotted) {
    for (const char* p = dotted; *p != '\0';) {
        size_t len = strcspn(p, ".");
        assert(len > 0 && len < 64);
        s_pkt[o++] = (uint8_t)len;
        memcpy(s_pkt + o, p, len);
        o += len;
        p += len + (p[len] == '.');
    }
    s_pkt[o++] = 0;
    return o;
}

/* One TXT answer straight into the module's record callback: the strings are
 * laid out on the wire exactly as an announce does, so mdns_record_parse_txt
 * does the key/value split. `pairs` are raw TXT strings ("port=42100"); a
 * string with no '=' is a bare key. */
static void feed(
    const char* instance, const char* const* pairs, size_t n, uint32_t ttl, const char* src_ip) {
    memset(s_pkt, 0, sizeof s_pkt);
    s_pkt[2] = 0x84; /* response, authoritative */
    s_pkt[7] = 1;    /* ancount */
    size_t o = put_name(12, instance);
    s_pkt[o++] = 0;
    s_pkt[o++] = MDNS_RECORDTYPE_TXT;
    s_pkt[o++] = 0;
    s_pkt[o++] = MDNS_CLASS_IN;
    for (int i = 0; i < 4; i++) {
        s_pkt[o++] = (uint8_t)(ttl >> (24 - 8 * i));
    }
    size_t lenat = o;
    o += 2;
    size_t rdata = o;
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(pairs[i]);
        assert(len < 256 && o + 1 + len < sizeof s_pkt);
        s_pkt[o++] = (uint8_t)len;
        memcpy(s_pkt + o, pairs[i], len);
        o += len;
    }
    size_t rdlen = o - rdata;
    s_pkt[lenat] = (uint8_t)(rdlen >> 8);
    s_pkt[lenat + 1] = (uint8_t)rdlen;

    /* A ':' makes it an IPv6 source, link-local on scope 3 like a Wi-Fi one. */
    struct sockaddr_storage from;
    memset(&from, 0, sizeof from);
    if (strchr(src_ip, ':') != NULL) {
        struct sockaddr_in6* a6 = (struct sockaddr_in6*)&from;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(5353);
        a6->sin6_scope_id = 3;
        assert(inet_pton(AF_INET6, src_ip, &a6->sin6_addr) == 1);
    } else {
        struct sockaddr_in* a4 = (struct sockaddr_in*)&from;
        a4->sin_family = AF_INET;
        a4->sin_port = htons(5353);
        assert(inet_pton(AF_INET, src_ip, &a4->sin_addr) == 1);
    }
    on_record(0, (const struct sockaddr*)&from, sizeof from, MDNS_ENTRYTYPE_ANSWER, 0,
        MDNS_RECORDTYPE_TXT, MDNS_CLASS_IN, ttl, s_pkt, o, 12, 0, rdata, rdlen, NULL);
}

#define INSTANCE "peer-00000000deadbeef._meleepc._udp.local."
#define NPAIRS(a) (sizeof(a) / sizeof((a)[0]))

/* A well-formed lobby record; cases override one string. */
static const char* good[] = {
    "v=4",
    "rev=0.1.2-test",
    "disc=00000000",
    "id=00000000deadbeef",
    "name=peer",
    "port=42100",
    "state=lobby",
    "gen=7",
};

static char s_v[32], s_disc[32];

static void reset(void) {
    s_n = 0;
    s_full = false;
    s_rj_logged = 0;
    s_state = 0;
    s_peer_id = 0;
    g_rejects = g_found = g_incompat = g_lost = g_full = 0;
    g_last[0] = g_detail[0] = '\0';
}

/* Copy of `good` with the pair whose key is `key` replaced by `repl`
 * (repl == NULL drops it; key == NULL replaces nothing), plus `add`. */
static size_t variant(
    const char** out, const char* key, const char* repl, const char* const* add, size_t n_add) {
    size_t n = 0, klen = key != NULL ? strlen(key) : 0;
    for (size_t i = 0; i < NPAIRS(good); i++) {
        if (key != NULL && strncmp(good[i], key, klen) == 0 && good[i][klen] == '=') {
            if (repl != NULL) {
                out[n++] = repl;
            }
            continue;
        }
        out[n++] = good[i];
    }
    for (size_t i = 0; i < n_add; i++) {
        out[n++] = add[i];
    }
    return n;
}

/* Every rejection case: the record must be dropped, nothing must land in the
 * peer table, and the rejection count must move by exactly one. */
static void must_reject(const char* what, const char** pairs, size_t n) {
    reset();
    feed(INSTANCE, pairs, n, 120, "10.0.0.7");
    if (g_rejects != 1 || s_n != 0 || g_found != 0) {
        fprintf(stderr,
            "%s: expected 1 rejection and an empty table, got rejects=%d peers=%d "
            "found=%d last=\"%s\"\n",
            what, g_rejects, s_n, g_found, g_last);
        exit(1);
    }
    printf("  reject %-34s -> %s\n", what, g_last + strlen("lan: "));
}

/* Accepted records: exactly one peer, no rejection, expected compatibility. */
static void must_accept(const char* what, const char** pairs, size_t n, bool compatible) {
    reset();
    feed(INSTANCE, pairs, n, 120, "10.0.0.7");
    if (g_rejects != 0 || s_n != 1 || g_found != 1 || s_peers[0].p.compatible != compatible) {
        fprintf(stderr,
            "%s: expected 1 %s peer, got rejects=%d peers=%d found=%d compat=%d "
            "last=\"%s\"\n",
            what, compatible ? "compatible" : "incompatible", g_rejects, s_n, g_found,
            s_n > 0 ? s_peers[0].p.compatible : -1, g_last);
        exit(1);
    }
    printf("  accept %-34s -> %s%s\n", what, s_peers[0].p.name,
        compatible ? " (eligible)" : " (incompatible)");
}

/* No net session may open until a guest acknowledges the current proposal.
 * Otherwise both peers can enter the blocking P1 wait on stale lobby data. */
static void election_setup(uint64_t local, uint64_t peer) {
    reset();
    s_started = true;
    s_sock4 = s_sock6 = -1;
    s_id = local;
    s_gen = 6;
    s_state = 4;
    s_hosting = false;
    s_barrier_ns = 0;
    s_scene = g_scene = g_peer_scene = 45; /* cases that preset s_barrier_ns skip the send */
    s_timer = 0;
    s_t0_ns = s_start_ns = s_announce_ns = s_now;
    s_n = 1;
    memset(&s_peers[0], 0, sizeof s_peers[0]);
    s_peers[0].id = peer;
    s_peers[0].gen = 6;
    s_peers[0].seen_ns = s_now;
    /* The scenario is "this peer has been sitting in our lobby": it was
     * observed in a non-starting state long enough ago to auto-join. */
    s_peers[0].lobby_ns = s_now - 3000000000ull;
    s_peers[0].p.compatible = true;
    s_peers[0].p.port = 42100;
    strcpy(s_peers[0].p.ip, "10.0.0.7");
    strcpy(s_peers[0].p.name, "peer");
    g_connections = 0;
    g_player = -1;
}

static void election_record(uint64_t peer, uint64_t chosen, const char* state, const char* offer) {
    char id[32], selected[32], st[32], ack[32];
    snprintf(id, sizeof id, "id=%016llx", (unsigned long long)peer);
    snprintf(selected, sizeof selected, "peer=%016llx", (unsigned long long)chosen);
    snprintf(st, sizeof st, "state=%s", state);
    snprintf(ack, sizeof ack, "offer=%s", offer != NULL ? offer : "");
    const char* p[] = {s_v, "rev=0.1.2-test", s_disc, id, "name=peer", "port=42100", st, "gen=7",
        "host=10.0.0.7:42100", selected, ack};
    feed(INSTANCE, p, offer != NULL ? 11 : 10, 120, "10.0.0.7");
}

static void check_joining_on_timer_stop(void) {
    assert(strcmp(state_txt(), "joining") == 0);
    assert(announce_timer(NULL, 1, 500) == 500);
}

static void case_election(void) {
    puts("\nelection: delayed ready records converge before opening a session");
    election_setup(100, 200);
    elect();
    assert(g_connections == 0 && pc_lan_state(NULL) == 1);
    election_record(200, 100, "starting", NULL);
    pc_lan_poll();
    assert(g_connections == 0 && pc_lan_is_host());
    election_record(200, 100, "joining", "6"); /* previous proposal */
    pc_lan_poll();
    assert(g_connections == 0);
    s_now += 14000000000ull; /* most of the proposal timeout was spent retrying */
    election_record(200, 100, "joining", "7");
    pc_lan_poll();
    assert(g_connections == 1 && g_player == 0);
    s_now += 2000000000ull;
    pc_lan_poll();
    assert(pc_lan_state(NULL) == 1); /* handshake gets its own full timeout */
    pc_lan_poll();
    assert(g_connections == 1); /* duplicate ack cannot reopen the session */
    timer_stop();

    election_setup(200, 100);
    elect();
    assert(g_connections == 0);
    election_record(100, 200, "starting", NULL);
    pc_lan_poll();
    assert(g_connections == 1 && g_player == 1 && !pc_lan_is_host());
    assert(strcmp(state_txt(), "joining") == 0);
    assert(announce_timer(NULL, 1, 500) == 500); /* repeats while P2 waits for P1 */
    s_barrier_ns = s_now;
    g_barrier_received = true;
    g_timer_stop_check = check_joining_on_timer_stop;
    pc_lan_poll();
    g_timer_stop_check = NULL;
    assert(pc_lan_state(NULL) == 2);
    assert(announce_timer(NULL, 1, 500) == 0); /* late callback no longer announces */

    /* A single Start from the higher ID must still be able to host an idle
     * lower-ID peer. Only two competing proposals need the tie-break. */
    election_setup(100, 200);
    s_state = 0;
    election_record(200, 100, "starting", NULL);
    pc_lan_poll();
    assert(g_connections == 1 && g_player == 1);
    timer_stop();

    /* A failed lobby (3) is still a lobby: a proposal naming us is joined and
     * Start retries. Before, only leaving the lobby cleared 3, and that exit's
     * goodbye is what failed the other side's connect. */
    election_setup(100, 200);
    s_state = 3;
    election_record(200, 100, "starting", NULL);
    pc_lan_poll();
    assert(g_connections == 1 && g_player == 1);
    timer_stop();
    election_setup(100, 200);
    s_state = 3;
    assert(pc_lan_start_match() && pc_lan_state(NULL) == 4);
    /* So is a fixture still polling in 2 after its session ended. */
    election_setup(100, 200);
    s_state = 2;
    g_active = false;
    pc_lan_poll();
    g_active = true;
    assert(pc_lan_state(NULL) == 3);

    /* A starting record naming us from a peer never observed in the lobby
     * (spoofed out of nowhere; LAN-SPOOF-AUTOJOIN) must not auto-dial. */
    election_setup(100, 200);
    s_state = 0;
    s_peers[0].lobby_ns = 0;
    election_record(200, 100, "starting", NULL);
    pc_lan_poll();
    assert(g_connections == 0 && g_player == -1 && pc_lan_state(NULL) == 0);
    timer_stop();

    election_setup(100, 200);
    elect();
    s_now += TIMEOUT_NS + 1;
    pc_lan_poll();
    assert(pc_lan_state(NULL) == 3 && g_connections == 0);
    s_started = false;
}

static void case_ready_frame_fence(void) {
    election_setup(100, 200);
    s_state = 1;
    s_offer_pending = false;
    s_barrier_ns = s_now;
    s_start_frame = g_scheduled = 120;
    g_frame = 119;
    g_polls = 0;
    g_ready_after = 3;
    pc_lan_poll();
    assert(s_state == 1 && g_polls == 0);
    g_frame = 120;
    pc_lan_poll();
    assert(s_state == 2 && g_frame == 120 && g_polls == 3);

    /* The host must also park before READY has completed its handshake. */
    s_state = 1;
    s_hosting = true;
    s_barrier_ns = 0;
    s_t0_ns = s_now;
    g_handshake_pending = true;
    g_polls = 0;
    g_ready_after = 3;
    pc_lan_poll();
    assert(s_state == 2 && g_frame == 120 && g_polls == 3);
    assert(g_sent_scene == 45); /* our barrier named our scene */
    g_handshake_pending = false;

    /* A missing barrier times out on this same simulation frame. */
    s_state = 1;
    s_barrier_ns = s_now - TIMEOUT_NS + 2000000;
    g_ready_after = 0;
    pc_lan_poll();
    assert(s_state == 3 && g_frame == 120);

    s_state = 1;
    s_barrier_ns = s_now;
    g_frame = 121;
    g_barrier_received = true;
    pc_lan_poll();
    assert(s_state == 3); /* a late barrier cannot enter on a different frame */
    s_state = 1;
    s_barrier_ns = s_now;
    g_frame = 120;
    g_polls = 0;
    g_disconnect_after = g_ready_after = 2;
    g_barrier_received = false;
    pc_lan_poll();
    assert(s_state == 3); /* a queued barrier cannot override disconnect */
    g_active = true;
    g_disconnect_after = 0;
    g_barrier_received = false;
    g_frame = g_scheduled = -1;
    s_started = false;
}

/* The READY_BARRIER names the sender's scene, and a session whose sides would
 * reach the start frame in different scenes is refused. The first phone<->PC
 * session ran the PC's title (0) against the phone's opening movie (28) and
 * desynced at frame 141. */
static void case_scene(void) {
    election_setup(100, 200);
    s_state = 1;
    s_offer_pending = false;
    s_barrier_ns = s_now;
    s_start_frame = g_scheduled = 120;
    g_frame = 60;
    s_scene = g_scene = 0;
    g_peer_scene = 28;
    g_barrier_received = true;
    pc_lan_poll();
    assert(s_state == 3 && strcmp(s_why, "peer is on another scene") == 0);
    assert(strstr(g_refuse, "peer is on scene 28, we are on scene 0") != NULL);
    printf("  %s\n", g_refuse);

    /* A hand-off between the barriers and the start frame is refused too. */
    election_setup(100, 200);
    s_state = 2;
    s_start_frame = g_scheduled = 120;
    g_frame = 100;
    pc_lan_poll();
    assert(s_state == 2);
    g_scene = 1;
    pc_lan_poll();
    assert(s_state == 3 && strstr(g_refuse, "went 45 -> 1") != NULL);
    printf("  %s\n", g_refuse);

    /* From the start frame on, scenes are the session's (hand-off, checksums). */
    election_setup(100, 200);
    s_state = 2;
    s_start_frame = g_scheduled = 120;
    g_frame = 121;
    g_scene = 1;
    pc_lan_poll();
    assert(s_state == 2);

    g_frame = g_scheduled = -1;
    g_scene = g_peer_scene = 45;
    s_started = false;
}

int main(int argc, char** argv) {
    case_ready_frame_fence();
    case_scene();
    set_image(argc > 1 && strcmp(argv[1], "b") == 0 ? 1 : 0);

    /* The module state pc_lan_start() would have built. */
    s_port = 42000;
    s_id = 0x00000000cafef00dull;
    snprintf(s_proto, sizeof s_proto, "%d", PC_NET_PROTO_VERSION);
    snprintf(s_name, sizeof s_name, "self");

    const char* id = pc_lan_disc_id();
    printf("image %s: disc id %s, rev %s, proto %s\n", s_image == 0 ? "a" : "b", id, pc_app_rev(),
        s_proto);
    assert(strlen(id) == 8);
    for (const char* p = id; *p != '\0'; p++) {
        assert((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
    }
    assert(strcmp(pc_lan_disc_id(), id) == 0); /* cached, stable within a run */

    /* `good` is written for proto 4 and disc 00000000; retarget it at this
     * build's version and this process's image so the suite is self-consistent. */
    snprintf(s_v, sizeof s_v, "v=%s", s_proto);
    snprintf(s_disc, sizeof s_disc, "disc=%s", id);
    good[0] = s_v;
    good[2] = s_disc;

    const char* p[24];
    size_t n;

    printf("\naccepted records:\n");
    must_accept("well-formed lobby record", good, NPAIRS(good), true);

    reset();
    feed(INSTANCE, good, NPAIRS(good), 120, "10.0.0.7");
    assert(s_peers[0].id == 0xdeadbeefull);
    assert(s_peers[0].p.port == 42100);
    assert(s_peers[0].state == ST_LOBBY);
    assert(s_peers[0].gen == 7);
    assert(strcmp(s_peers[0].p.name, "peer") == 0);
    assert(strcmp(s_peers[0].p.ip, "10.0.0.7") == 0);

    n = variant(p, "state", "state=ready", NULL, 0);
    must_accept("state=ready", p, n, true);
    {
        static const char* const add[] = {"host=10.0.0.7:42100", "peer=00000000cafef00d"};
        n = variant(p, "state", "state=starting", add, 2);
        must_accept("state=starting with host/peer", p, n, true);
        assert(s_peers[0].state == ST_STARTING && s_peers[0].p.host);
        assert(s_peers[0].peer_id == 0x00000000cafef00dull);
    }
    n = variant(p, "port", "port=65535", NULL, 0);
    must_accept("port=65535 (top of the range)", p, n, true);
    n = variant(p, "name", "name=fifteencharsxxx", NULL, 0); /* 15 chars: fits with its NUL */
    must_accept("name of 15 chars", p, n, true);

    printf("\nincompatible, but listed (requirement: named before any packet):\n");
    n = variant(p, "rev", "rev=0.9.9-other", NULL, 0);
    must_accept("another build (rev=)", p, n, false);
    assert(g_incompat == 1 && strstr(g_detail, "rev 0.9.9-other") != NULL);
    printf("    %s\n", g_detail + strlen("lan:   "));

    n = variant(p, "disc", "disc=1234abcd", NULL, 0);
    must_accept("another game image (disc=)", p, n, false);
    assert(g_incompat == 1 && strstr(g_detail, "disc 1234abcd") != NULL &&
           strstr(g_detail, id) != NULL);
    printf("    %s\n", g_detail + strlen("lan:   "));

    { /* another protocol version: identity only, unknown keys tolerated */
        static const char* const add[] = {"future=yes"};
        n = variant(p, "v", "v=99", add, 1);
        must_accept("another protocol version + unknown key", p, n, false);
        assert(g_incompat == 1 && strstr(g_detail, "proto 99") != NULL);
        printf("    %s\n", g_detail + strlen("lan:   "));
    }

    printf("\nrejected records:\n");
    {
        static const char* const add[] = {"future=yes"};
        n = variant(p, NULL, NULL, add, 1);
        must_reject("unknown key, our own version", p, n);
    }
    {
        static const char* const add[] = {"id=00000000deadbee0"};
        n = variant(p, NULL, NULL, add, 1);
        must_reject("duplicate id=", p, n);
    }
    n = variant(p, "v", NULL, NULL, 0);
    must_reject("no v=", p, n);
    n = variant(p, "id", NULL, NULL, 0);
    must_reject("no id=", p, n);
    n = variant(p, "name", NULL, NULL, 0);
    must_reject("no name=", p, n);
    n = variant(p, "port", NULL, NULL, 0);
    must_reject("no port=", p, n);
    n = variant(p, "rev", NULL, NULL, 0);
    must_reject("no rev=", p, n);
    n = variant(p, "disc", NULL, NULL, 0);
    must_reject("no disc=", p, n);
    n = variant(p, "state", NULL, NULL, 0);
    must_reject("no state=", p, n);
    n = variant(p, "gen", NULL, NULL, 0);
    must_reject("no gen=", p, n);

    n = variant(p, "name", "name", NULL, 0);
    must_reject("bare key, no '='", p, n);
    n = variant(p, "name", "name=", NULL, 0);
    must_reject("empty name=", p, n);
    n = variant(p, "name", "name=sixteencharsxxxx", NULL, 0); /* 16: no room for the NUL */
    must_reject("name of 16 chars", p, n);
    n = variant(p, "rev",
        "rev=0123456789012345678901234567890123456789012345678901234567890123456789", NULL, 0);
    must_reject("value over 63 bytes", p, n);
    n = variant(p, "rev", "rev=0123456789012345678901234567890123", NULL, 0);
    must_reject("rev over 31 bytes", p, n);

    n = variant(p, "id", "id=0000000000000000", NULL, 0);
    must_reject("id=0", p, n);
    n = variant(p, "id", "id=00000000deadbeeg", NULL, 0);
    must_reject("non-hex id=", p, n);
    n = variant(p, "id", "id=00000000deadbeef0", NULL, 0); /* 17 digits */
    must_reject("id over 64 bits", p, n);

    n = variant(p, "port", "port=0", NULL, 0);
    must_reject("port=0", p, n);
    n = variant(p, "port", "port=65536", NULL, 0);
    must_reject("port above 65535", p, n);
    n = variant(p, "port", "port=42100x", NULL, 0);
    must_reject("port with trailing junk", p, n);
    n = variant(p, "port", "port=+42100", NULL, 0);
    must_reject("signed port", p, n);
    n = variant(p, "port", "port= 42100", NULL, 0);
    must_reject("port with leading space", p, n);

    n = variant(p, "state", "state=hosting", NULL, 0);
    must_reject("unknown state=", p, n);
    n = variant(p, "state", "state=starting", NULL, 0);
    must_reject("starting without host/peer", p, n);
    {
        static const char* const add[] = {"host=10.0.0.7:42100", "peer=00000000cafef00d"};
        n = variant(p, NULL, NULL, add, 2);
        must_reject("host/peer in a lobby record", p, n);
    }
    {
        static const char* const add[] = {"host=10.0.0.7:42100", "peer=zz"};
        n = variant(p, "state", "state=starting", add, 2);
        must_reject("non-hex peer=", p, n);
    }

    n = variant(p, "gen", "gen=7x", NULL, 0);
    must_reject("non-numeric gen=", p, n);
    n = variant(p, "gen", "gen=4294967296", NULL, 0);
    must_reject("gen above 2^32-1", p, n);
    n = variant(p, "disc", "disc=zzzz", NULL, 0);
    must_reject("non-hex disc=", p, n);

    { /* 12 pairs: mdns_record_parse_txt can no longer prove it saw them all */
        static const char* const add[] = {"host=x", "peer=1", "a=1", "b=2"};
        n = variant(p, NULL, NULL, add, 4);
        must_reject("12 or more TXT keys", p, n);
    }

    /* mdns.h drops a string with a control byte outright (DNS-SD requires
     * printable US-ASCII), so the key simply goes missing on the wire. Drive
     * parse_txt directly to cover txt_val's own check. */
    {
        reset();
        mdns_record_txt_t raw[8];
        const char* keys[8] = {"v", "rev", "disc", "id", "name", "port", "state", "gen"};
        const char* vals[8] = {
            s_proto, "0.1.2-test", id, "00000000deadbeef", "pe\x01r", "42100", "lobby", "7"};
        for (size_t i = 0; i < 8; i++) {
            raw[i].key.str = keys[i];
            raw[i].key.length = strlen(keys[i]);
            raw[i].value.str = vals[i];
            raw[i].value.length = strlen(vals[i]);
        }
        Txt t;
        assert(!parse_txt(raw, 8, &t));
        assert(g_rejects == 1 && s_n == 0);
        printf("  reject %-34s -> %s\n", "control byte in a value", g_last + strlen("lan: "));
    }

    printf("\npeer table:\n");
    /* Fills the table, then one more: the extra is refused, the eight stand. */
    reset();
    for (int i = 0; i < PC_LAN_MAX_PEERS + 1; i++) {
        char inst[128], idv[32], namev[32];
        snprintf(inst, sizeof inst, "p%d-000000000000000%x._meleepc._udp.local.", i, i + 1);
        snprintf(idv, sizeof idv, "id=000000000000000%x", i + 1);
        snprintf(namev, sizeof namev, "name=p%d", i);
        n = variant(p, "id", idv, NULL, 0);
        for (size_t j = 0; j < n; j++) {
            if (strncmp(p[j], "name=", 5) == 0) {
                p[j] = namev;
            }
        }
        feed(inst, p, n, 120, "10.0.0.7");
    }
    assert(s_n == PC_LAN_MAX_PEERS);
    assert(g_found == PC_LAN_MAX_PEERS);
    assert(g_full == 1 && pc_lan_full());
    printf("  %d peers, the 9th refused once: %s\n", s_n, g_last);

    /* pc_lan_peers() never writes past the caller's array. */
    {
        PcLanPeer out[3];
        memset(out, 0, sizeof out);
        assert(pc_lan_peers(out, 3) == 3);
        assert(pc_lan_peers(out, 0) == 0);
        printf("  pc_lan_peers(out, 3) = 3 of %d, pc_lan_peers(out, 0) = 0\n", s_n);
    }

    /* A goodbye (ttl 0) drops its peer and nothing else; an unknown one is a
     * no-op, and a full table takes goodbyes (they are handled before the
     * capacity check). */
    {
        int before = s_n;
        n = variant(p, "id", "id=0000000000000003", NULL, 0);
        feed("p2-0000000000000003._meleepc._udp.local.", p, n, 0, "10.0.0.7");
        assert(s_n == before - 1 && g_lost == 1);
        feed("p2-0000000000000003._meleepc._udp.local.", p, n, 0, "10.0.0.7");
        assert(s_n == before - 1 && g_lost == 1); /* already gone: no second drop */
        printf("  goodbye dropped one peer, %d left; a repeat is a no-op\n", s_n);
    }

    /* A goodbye from the peer we are connecting to fails the lobby. */
    {
        reset();
        feed(INSTANCE, good, NPAIRS(good), 120, "10.0.0.7");
        s_state = 1;
        s_peer_id = 0xdeadbeefull;
        feed(INSTANCE, good, NPAIRS(good), 0, "10.0.0.7");
        assert(s_n == 0 && s_state == 3 && strcmp(s_why, "peer left lobby") == 0);
        printf("  goodbye from our chosen peer -> state 3 \"%s\"\n", s_why);
    }

    /* Seen on both families: the IPv4 address is kept. */
    {
        reset();
        feed(INSTANCE, good, NPAIRS(good), 120, "10.0.0.7");
        feed(INSTANCE, good, NPAIRS(good), 120, "fe80::7");
        assert(s_n == 1 && g_found == 1 && strcmp(s_peers[0].p.ip, "10.0.0.7") == 0);
        struct sockaddr_in6 v6;
        memset(&v6, 0, sizeof v6);
        v6.sin6_family = AF_INET6;
        assert(inet_pton(AF_INET6, "fe80::1", &v6.sin6_addr) == 1);
        v6.sin6_scope_id = 3;
        char text[46];
        net_addr_text((const struct sockaddr*)&v6, text, sizeof text);
        assert(strcmp(text, "fe80::1%3") == 0);
        printf("  net_addr_text(fe80::1 scope 3) = %s\n", text);
    }

    /* A peer re-entering its lobby sends goodbye then announce on both
     * families within a millisecond, and the two sockets are drained v4
     * first. The old run's IPv6 goodbye, read after the new run's IPv4
     * announce, must not evict the peer (the phone<->PC logs' "lost
     * 192.168.1.160 / found / lost / found fe80::...%47"), nor fail a
     * connect to it; a goodbye of the gen we hold still does both. */
    {
        const char* next[NPAIRS(good)];
        size_t nn = variant(next, "gen", "gen=8", NULL, 0);
        reset();
        feed(INSTANCE, good, NPAIRS(good), 120, "10.0.0.7");
        feed(INSTANCE, good, NPAIRS(good), 120, "fe80::7");
        feed(INSTANCE, good, NPAIRS(good), 0, "10.0.0.7"); /* goodbye, gen 7 */
        feed(INSTANCE, next, nn, 120, "10.0.0.7");         /* new run, gen 8 */
        feed(INSTANCE, good, NPAIRS(good), 0, "fe80::7");  /* the gen 7 goodbye's twin */
        feed(INSTANCE, next, nn, 120, "fe80::7");
        assert(g_lost == 1 && g_found == 2 && s_n == 1 && s_peers[0].gen == 8);
        assert(strcmp(s_peers[0].p.ip, "10.0.0.7") == 0);
        s_state = 1;
        s_peer_id = 0xdeadbeefull;
        feed(INSTANCE, good, NPAIRS(good), 0, "fe80::7"); /* delivered late again */
        assert(s_n == 1 && s_state == 1);
        feed(INSTANCE, next, nn, 0, "fe80::7");
        assert(s_n == 0 && s_state == 3 && strcmp(s_why, "peer left lobby") == 0);
        printf("  lobby re-entry on two families: one lost, connect kept until its own gen "
               "says goodbye\n");
    }

    case_election();
    printf("\nOK\n");
    return 0;
}
