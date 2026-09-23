/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LAN lobby: mDNS/DNS-SD discovery of other melee-pc instances and the
 * host election (docs/netcode-plan.md §8).
 *
 * Every instance announces `<name>-<id>._meleepc._udp.local.` once a second
 * as an unsolicited multicast answer (PTR + SRV + TXT + A/AAAA for
 * `<name>.local.`) on IPv4 and IPv6 (whichever sockets opened) and answers
 * PTR queries for the service, so a fresh instance sees everyone at once.
 * TXT:
 *   v=<protocol> rev=<build> disc=<game image id, 32-bit hex>
 *   id=<install id, 64-bit hex> name=<hostname> port=<game udp port>
 *   state=lobby|ready|starting|joining gen=<start attempt>
 *   host=<our ip:port> peer=<chosen peer id>             (starting/joining)
 *   offer=<host generation>                            (joining only)
 * Every value is validated into a scratch record first (parse_txt), so a
 * malformed or crafted announce is dropped whole rather than applied in part.
 * Peers are keyed by install id and connected to at the datagram's source
 * address (IPv4 preferred when a peer is seen on both families; link-local
 * IPv6 carries its %scope). Our own looped-back record is skipped; a peer
 * silent for 5 s, or one that sent a goodbye (ttl 0), is dropped, and a
 * record with an older gen= than the one held for its id is stale (a late
 * copy through the other family's socket) and ignored. A peer on
 * another protocol version, build (rev=) or game image (disc=) is listed as
 * incompatible and never picked. The address in our A/AAAA/host= is the
 * route to the mDNS group (connected UDP probe + getsockname), unless that
 * is a container/VPN one and a plain one exists.
 *
 * Match start: Start flips us to state=ready. ELECTION_NS later, if a ready
 * compatible peer with a lower id is visible it will host (it sees us too),
 * so we wait; otherwise we host: the record flips to state=starting
 * peer=<guest id> (the lowest ready id, else the lowest compatible id) and
 * the host waits for a state=joining acknowledgement of that generation
 * before opening P1. Competing proposals converge on the lower ID while
 * both game threads can still poll discovery. The chosen guest acknowledges
 * and connects as P2; a timer repeats its joining record while the first
 * tick waits for the host. gen= keeps stale attempts from being reused.
 * Both then poll the RULES/READY handshake in net.c once per frame, then
 * exchange one READY_BARRIER (reliable 0x11, carrying the sender's scene)
 * so state 2 means both sides are through the handshake and in the same
 * scene (s_scene). Every failure goes through fail(): session
 * closed, timer gone, goodbye sent, state 3 with the reason for the menu;
 * 3 is still the lobby (proposals are joined, Start retries).
 * A goodbye from the peer we are connecting to fails us the same way, and
 * a goodbye also goes out from atexit() so closing the window in the lobby
 * announces leaving.
 *
 * Multicast that is blocked (no socket, or nothing at all heard in 5 s, not
 * even our own loop-back) is reported by pc_lan_discovery_unavailable();
 * pc_lan_connect_direct() still works without it.
 *
 * MELEE_LAN_TEST=1|host runs this without the menu (os.c/vi.c); two
 * instances need distinct MELEE_NET_PORT and MELEE_CACHE_DIR.
 *
 * ponytail: only service PTR questions are answered (third-party browsers
 * list us and see our addresses but get no unicast replies); one address
 * per family. */
#include "pc/net_lan.h"
#include "pc/android_hooks.h"
#include "pc/net.h"
#include "pc/pc.h"

#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <xxhash.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_timer.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#define sock_close close
#endif
#include "mdns/mdns.h"

#define SERVICE "_meleepc._udp.local."
#define ANNOUNCE_NS 1000000000ull
#define LOST_NS 5000000000ull
#define TIMEOUT_NS 15000000000ull
#define ELECTION_NS 100000000ull /* ready -> host decision: a simultaneous Start is seen first */
/* A starting proposal is only honored from a peer whose lobby/ready record we
 * saw at least this long ago (2 announce periods): a genuine host always
 * broadcasts lobby state before it claims to be starting, so a record that
 * appears out of nowhere already starting with our peer id is a spoof
 * (LAN-SPOOF-AUTOJOIN). ponytail: trust-on-observation only; a determined
 * attacker with a longer presence still gets through — the real fix is a
 * signed proposal keyed by the peer's ed25519 identity. */
#define LOBBY_DWELL_NS (2 * ANNOUNCE_NS)
#define REL_READY_BARRIER 0x11 /* reliable type: "my handshake is done" (net_lan.h) */
/* Ubuntu's clang-format (what CI installs) and 22.x disagree on the spacing
 * of a braced-list macro body and neither accepts the other's output, so the
 * two macros below are pinned. The marker comment must be exactly this, with
 * nothing else on the line, or clang-format ignores it. */
/* clang-format off */
#define MSTR(s) { (s), strlen(s) }
/* clang-format on */
#define MAX_IFACES 16

enum { ST_LOBBY, ST_READY, ST_STARTING, ST_JOINING };

typedef struct Entry {
    uint64_t id;
    uint64_t peer_id; /* guest chosen by a starting host */
    int state;
    uint32_t offer;    /* host generation acknowledged by a joining guest */
    uint32_t gen;      /* gen= of the latest record */
    uint32_t last_gen; /* gen= of the starting record we last joined on */
    uint64_t seen_ns;
    uint64_t lobby_ns; /* first sighting in a lobby/ready state; 0 = never seen so */
    PcLanPeer p;
} Entry;

/* Up, non-loopback unicast addresses of one interface (one per family). */
typedef struct Iface {
    char name[32];
    unsigned index;       /* for IPV6_MULTICAST_IF; 0 = unknown */
    struct in_addr a;     /* 0 = none */
    struct in6_addr aaaa; /* :: = none; a global/ULA one beats link-local */
    bool link_local6;
} Iface;

static bool s_started;
static int s_sock4 = -1;
static int s_sock6 = -1;
static uint64_t s_id;
static char s_name[PC_LAN_NAME_LEN];
static char s_instance[64]; /* <name>-<id>._meleepc._udp.local. */
static char s_hostname[32]; /* <name>.local. */
static struct in_addr s_self4;
static struct in6_addr s_self6;
static char s_self_ip[46]; /* host= TXT: s_self4, else s_self6, else 0.0.0.0 */
static char s_proto[8];    /* PC_NET_PROTO_VERSION as text */
static uint16_t s_port;
static uint32_t s_gen;
static Entry s_peers[PC_LAN_MAX_PEERS];
static int s_n;
static bool s_full;
static bool s_heard;    /* a record of our service arrived (ours looped back counts) */
static bool s_no_mcast; /* pc_lan_discovery_unavailable() */
static bool s_atexit;
static uint64_t s_start_ns;
static uint64_t s_announce_ns;
static uint32_t s_rx[512]; /* mdns.h wants 32-bit aligned buffers */
static uint32_t s_tx[512];
static SDL_TimerID s_timer;
static SDL_Mutex* s_announce_lock;
static bool s_timer_live; /* timer_stop waits out callbacks before state changes */

/* Lobby state machine (pc_lan_state()): 0 idle, 1 connecting, 2 in match,
 * 3 failed, 4 ready. Within 1, s_barrier_ns != 0 once the handshake is
 * done and we wait for the peer's READY_BARRIER. */
static int s_state;
static const char* s_why;
static bool s_offer_pending; /* no game connection until the guest acknowledges */
static uint32_t s_offer_gen; /* guest: the host proposal we acknowledged */
static bool s_hosting;       /* not s_host: a struct in_addr member macro on winsock */
static uint64_t s_peer_id;
static uint64_t s_t0_ns;
static uint64_t s_barrier_ns;
static uint32_t s_seed;
static int32_t s_start_frame;
/* scene_kind() when our READY_BARRIER went out; the barrier carries it. The
 * frame checksums start at s_start_frame, and nothing before made the two
 * sides be in the same scene there: the menu lobby only ever polls from
 * GS_ONLINE_LOBBY, but the MELEE_LAN_TEST/MELEE_LAN_DIRECT fixtures start a
 * session from wherever the game is. The first phone<->PC LAN session did
 * exactly that, the PC on the title (scene 0) and the phone on the opening
 * movie (28): the hand-off agreed the exit frames (both left at 51 and 308),
 * the two entered different scenes, and "net: DESYNC at frame 141". So the
 * peer's barrier must name our scene, and ours must not change until
 * s_start_frame (pc_lan_poll), or the match is refused. */
static int s_scene;

static const char* state_txt(void) {
    if (s_state == 4) {
        return "ready";
    }
    if (!s_hosting && s_state == 1 && s_peer_id != 0) {
        return "joining";
    }
    return s_hosting && (s_state == 1 || s_state == 2) ? "starting" : "lobby";
}

/* Identity of the game image, so two peers on different discs (region,
 * revision, or a modified ISO) are incompatible in the lobby instead of
 * desyncing in the match. XXH3 over the boot info the DVD layer read out of
 * the image (game id, disc number, disc version, streaming flags), the base
 * FST entry count and the disc's main.dol, folded to 32 bits because the
 * announce already carries eight TXT keys and an mDNS answer is size
 * constrained. Both blobs are already in memory (aurora keeps the partition
 * metadata for the life of the disc), so nothing is read off the image here;
 * computed once and cached.
 *
 * ponytail: pins the code and the file-table shape, not every file's bytes,
 * so a data-only mod that leaves the FST shape alone (a swapped Pl*.dat of
 * the same size) still matches. Fold in every base FST entry's name and
 * size when that shows up. */
const char* pc_lan_disc_id(void) {
    static char id[9];
    if (id[0] != '\0') {
        return id;
    }
    const DVDDiskID* disk = DVDGetCurrentDiskID();
    s32 dol_size = 0;
    const u8* dol = DVDGetDOLLocation(&dol_size);
    if (disk == NULL || dol == NULL || dol_size <= 0) {
        return "0"; /* no disc open: only before the launcher hands one over */
    }
    uint32_t entries = (uint32_t)aurora_dvd_base_entry_count();
    uint32_t bytes = (uint32_t)dol_size;
    uint8_t meta[18];
    memcpy(meta, disk->gameName, sizeof disk->gameName);
    memcpy(meta + 4, disk->company, sizeof disk->company);
    meta[6] = disk->diskNumber;
    meta[7] = disk->gameVersion;
    meta[8] = disk->streaming;
    meta[9] = disk->streamingBufSize;
    for (int i = 0; i < 4; i++) { /* spelled out: the peers can be x86 and ARM */
        meta[10 + i] = (uint8_t)(entries >> (24 - 8 * i));
        meta[14 + i] = (uint8_t)(bytes >> (24 - 8 * i));
    }
    uint64_t h = XXH3_64bits_withSeed(dol, (size_t)dol_size, XXH3_64bits(meta, sizeof meta));
    snprintf(id, sizeof id, "%08x", (uint32_t)(h ^ h >> 32));
    return id;
}

static void announce_on(int sock, void* buf, size_t cap, bool goodbye) {
    char port[8], id[20], peer[20], host[64], gen[12];
    snprintf(port, sizeof port, "%u", s_port);
    snprintf(id, sizeof id, "%016llx", (unsigned long long)s_id);
    snprintf(peer, sizeof peer, "%016llx", (unsigned long long)s_peer_id);
    snprintf(host, sizeof host, "%s:%u", s_self_ip, s_port);
    snprintf(gen, sizeof gen, "%u", s_gen);
    char offer[12];
    snprintf(offer, sizeof offer, "%u", s_offer_gen);
    const char* state = state_txt();
    mdns_record_t ptr = {
        .name = MSTR(SERVICE), .type = MDNS_RECORDTYPE_PTR, .data.ptr.name = MSTR(s_instance)};
#define TXT(k, v)                                                                                  \
    {                                                                                              \
        .name = MSTR(s_instance), .type = MDNS_RECORDTYPE_TXT, .data.txt = { MSTR(k), MSTR(v) }    \
    }
    mdns_record_t extra[14] = {
        /* SRV + 8 TXT + host/peer/offer + A + AAAA */
        {.name = MSTR(s_instance),
            .type = MDNS_RECORDTYPE_SRV,
            .data.srv = {0, 0, s_port, MSTR(s_hostname)}},
        TXT("v", s_proto),
        TXT("rev", pc_app_rev()),
        TXT("disc", pc_lan_disc_id()),
        TXT("id", id),
        TXT("name", s_name),
        TXT("port", port),
        TXT("state", state),
        TXT("gen", gen),
    };
    size_t n = 9;
    if (strcmp(state, "starting") == 0 || strcmp(state, "joining") == 0) {
        extra[n++] = (mdns_record_t)TXT("host", host);
        extra[n++] = (mdns_record_t)TXT("peer", peer);
    }
    if (strcmp(state, "joining") == 0) {
        extra[n++] = (mdns_record_t)TXT("offer", offer);
    }
#undef TXT
    if (s_self4.s_addr != 0) {
        extra[n].name = (mdns_string_t)MSTR(s_hostname);
        extra[n].type = MDNS_RECORDTYPE_A;
        extra[n].data.a.addr.sin_family = AF_INET;
        extra[n++].data.a.addr.sin_addr = s_self4;
    }
    if (!IN6_IS_ADDR_UNSPECIFIED(&s_self6)) {
        extra[n].name = (mdns_string_t)MSTR(s_hostname);
        extra[n].type = MDNS_RECORDTYPE_AAAA;
        extra[n].data.aaaa.addr.sin6_family = AF_INET6;
        extra[n++].data.aaaa.addr.sin6_addr = s_self6;
    }
    (goodbye ? mdns_goodbye_multicast : mdns_announce_multicast)(
        sock, buf, cap, ptr, NULL, 0, extra, n);
}

/* On every open socket (mdns.h picks the group from the socket's family). */
static void announce(void* buf, size_t cap, bool goodbye) {
    if (s_sock4 >= 0) {
        announce_on(s_sock4, buf, cap, goodbye);
    }
    if (s_sock6 >= 0) {
        announce_on(s_sock6, buf, cap, goodbye);
    }
}

/* Both host and guest may block in their first input wait. Repeat the
 * immutable election record until the session handshake is complete. */
static Uint32 SDLCALL announce_timer(void* ud, SDL_TimerID id, Uint32 interval) {
    uint32_t buf[512];
    (void)ud;
    (void)id;
    SDL_LockMutex(s_announce_lock);
    if (!s_timer_live) {
        SDL_UnlockMutex(s_announce_lock);
        return 0;
    }
    announce(buf, sizeof buf, false);
    SDL_UnlockMutex(s_announce_lock);
    return interval;
}

static void timer_stop(void) {
    if (s_timer) {
        SDL_RemoveTimer(s_timer);
        s_timer = 0;
    }
    if (s_announce_lock != NULL) {
        SDL_LockMutex(s_announce_lock);
        s_timer_live = false;
        SDL_UnlockMutex(s_announce_lock);
    }
}

static void timer_start(void) {
    if (s_announce_lock == NULL) {
        s_announce_lock = SDL_CreateMutex();
    }
    SDL_LockMutex(s_announce_lock);
    s_timer_live = true;
    s_timer = SDL_AddTimer(500, announce_timer, NULL);
    SDL_UnlockMutex(s_announce_lock);
}

/* Every failure path: no session, no "starting" record (the goodbye takes it
 * back at once, so the peer fails with "peer left lobby" instead of waiting
 * out its timeout), state 3 with why. */
static void fail(const char* why) {
    timer_stop();
    pc_net_disconnect();
    s_state = 3;
    s_why = why;
    s_hosting = false;
    s_offer_pending = false;
    s_offer_gen = 0;
    s_peer_id = 0;
    s_barrier_ns = 0;
    announce(s_tx, sizeof s_tx, true);
    s_gen++; /* the goodbye ends its gen: a late copy of it is then stale (on_record) */
    pc_log_line("lan: failed: %s", why);
}

static void drop(int i) {
    pc_log_line("lan: lost %s %s:%u", s_peers[i].p.name, s_peers[i].p.ip, s_peers[i].p.port);
    s_peers[i] = s_peers[--s_n];
}

/* ---- TXT record -------------------------------------------------------
 * Announces arrive once a second from anything on the link, so this is the
 * module's whole attack surface. Every value is bounded and validated into a
 * scratch Txt before one field of it reaches the peer table, and a record
 * that breaks any rule is dropped whole: an existing entry is never left
 * half-overwritten by a crafted follow-up.
 *
 * A peer that claims another protocol version is held to the identity keys
 * only (v/id/name/port) and listed as incompatible on v= alone, so a future
 * build that adds a key still shows up in the lobby with a reason instead of
 * vanishing from it (RFC 6763 §6.6). A peer claiming our version must send
 * exactly our key set: the TXT layout is part of PC_NET_PROTO_VERSION. */
enum { K_V, K_REV, K_DISC, K_ID, K_NAME, K_PORT, K_STATE, K_GEN, K_HOST, K_PEER, K_OFFER, K_N };
/* Pinned: see the note on MSTR above. */
/* clang-format off */
#define KEY(s) { s, sizeof s - 1 }
/* clang-format on */
static const struct {
    const char* k;
    size_t len;
} k_txt[K_N] = {
    KEY("v"),
    KEY("rev"),
    KEY("disc"),
    KEY("id"),
    KEY("name"),
    KEY("port"),
    KEY("state"),
    KEY("gen"),
    KEY("host"),
    KEY("peer"),
    KEY("offer"),
};
#undef KEY
#define KBIT(k) (1u << (k))
#define TXT_MAX_PAIRS 12 /* 8 keys in lobby, 10 starting, 11 joining */

/* Rejection classes, one per reason: a bad announcer repeats once a second,
 * so each is logged once per session (pc_lan_start clears them) and each is
 * separately observable by tools/net_lan_txt_test.py. */
enum {
    RJ_PAIRS,
    RJ_UNKNOWN,
    RJ_DUP,
    RJ_VALUE,
    RJ_NO_ID,
    RJ_V,
    RJ_ID,
    RJ_NAME,
    RJ_PORT,
    RJ_NO_OURS,
    RJ_DISC,
    RJ_STATE,
    RJ_GEN,
    RJ_NO_START,
    RJ_STRAY,
    RJ_PEER,
    RJ_OFFER,
    RJ_N
};
static const char* const k_rj[RJ_N] = {
    "too many TXT keys",
    "unknown key on our own protocol version",
    "duplicate key",
    "empty, over-long or unprintable value",
    "no v/id/name/port",
    "bad protocol version",
    "bad install id",
    "over-long name",
    "bad game port",
    "no rev/disc/state/gen",
    "bad disc id",
    "unknown state",
    "bad gen",
    "starting without host/peer",
    "host/peer outside a starting record",
    "bad peer id",
    "joining without a valid offer generation",
};
_Static_assert(RJ_N <= 32, "s_rj_logged is a 32-bit mask");
static uint32_t s_rj_logged;

/* Always false, so a rule reads `return reject(RJ_...)`. */
static bool reject(int cls) {
    if (!(s_rj_logged & KBIT(cls))) {
        s_rj_logged |= KBIT(cls);
        pc_log_line("lan: dropped a malformed record: %s", k_rj[cls]);
    }
    return false;
}

/* value -> NUL-terminated copy. False when it is absent (a bare key with no
 * '='), empty, too long for dst to hold with its NUL, or not printable
 * US-ASCII (mdns.h already drops such a string, but the parser is also
 * driven directly by tools/test_net_lan_txt.c). */
static bool txt_val(char* dst, size_t cap, mdns_string_t v) {
    if (v.str == NULL || v.length == 0 || v.length >= cap) {
        return false;
    }
    for (size_t i = 0; i < v.length; i++) {
        if ((unsigned char)v.str[i] < 0x20 || (unsigned char)v.str[i] > 0x7E) {
            return false;
        }
    }
    memcpy(dst, v.str, v.length);
    dst[v.length] = '\0';
    return true;
}

/* Whole-string decimal, no sign, no space, no trailing junk, no overflow. */
static bool dec_u32(const char* s, uint32_t* out) {
    uint64_t v = 0;
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        if (n >= 10 || s[n] < '0' || s[n] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(s[n] - '0');
    }
    if (n == 0 || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* Whole-string hex, either case, at most 16 digits (no 0x, no sign). */
static bool hex_u64(const char* s, uint64_t* out) {
    static const char digits[] = "0123456789abcdef";
    uint64_t v = 0;
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char* d = memchr(digits, s[n] | 0x20, sizeof digits - 1);
        if (n >= 16 || d == NULL) {
            return false;
        }
        v = v << 4 | (uint64_t)(d - digits);
    }
    if (n == 0) {
        return false;
    }
    *out = v;
    return true;
}

/* A validated announce: the entry it yields plus the three identity strings
 * the incompatible-peer log line names ("?" when the peer sent none). */
typedef struct Txt {
    char v[12], rev[64] /* = val[] cap; git describe output */, disc[12];
    Entry e;
} Txt;

static bool parse_txt(const mdns_record_txt_t* txt, size_t n, Txt* out) {
    char val[K_N][64];
    unsigned seen = 0;
    bool unknown = false;
    uint32_t u;
    uint64_t h;
    memset(out, 0, sizeof *out);
    strcpy(out->rev, "?");
    strcpy(out->disc, "?");
    if (n >= TXT_MAX_PAIRS) {
        return reject(RJ_PAIRS);
    }
    for (size_t i = 0; i < n; i++) {
        int k = 0;
        while (k < K_N && (txt[i].key.length != k_txt[k].len ||
                              memcmp(txt[i].key.str, k_txt[k].k, k_txt[k].len) != 0))
        {
            k++;
        }
        if (k == K_N) {
            unknown = true;
            continue;
        }
        if (seen & KBIT(k)) {
            return reject(RJ_DUP);
        }
        seen |= KBIT(k);
        if (!txt_val(val[k], sizeof val[k], txt[i].value)) {
            return reject(RJ_VALUE);
        }
    }
    const unsigned id_keys = KBIT(K_V) | KBIT(K_ID) | KBIT(K_NAME) | KBIT(K_PORT);
    if ((seen & id_keys) != id_keys) {
        return reject(RJ_NO_ID);
    }
    if (!dec_u32(val[K_V], &u) || strlen(val[K_V]) >= sizeof out->v) {
        return reject(RJ_V);
    }
    if (!hex_u64(val[K_ID], &out->e.id) || out->e.id == 0) {
        return reject(RJ_ID);
    }
    if (strlen(val[K_NAME]) >= sizeof out->e.p.name) {
        return reject(RJ_NAME);
    }
    if (!dec_u32(val[K_PORT], &u) || u == 0 || u > 65535) {
        return reject(RJ_PORT);
    }
    out->e.p.port = (uint16_t)u;
    strcpy(out->e.p.name, val[K_NAME]);
    strcpy(out->v, val[K_V]);
    /* Truncated to fit rather than rejected while the version differs: only
     * the incompatible-peer log line reads them then. */
    if (seen & KBIT(K_REV)) {
        snprintf(out->rev, sizeof out->rev, "%.*s", (int)sizeof out->rev - 1, val[K_REV]);
    }
    if (seen & KBIT(K_DISC)) {
        snprintf(out->disc, sizeof out->disc, "%.*s", (int)sizeof out->disc - 1, val[K_DISC]);
    }
    if (strcmp(out->v, s_proto) != 0) {
        return true; /* incompatible; e.state stays lobby, nothing else read */
    }
    if (unknown) {
        return reject(RJ_UNKNOWN);
    }
    const unsigned our_keys = KBIT(K_REV) | KBIT(K_DISC) | KBIT(K_STATE) | KBIT(K_GEN);
    if ((seen & our_keys) != our_keys) {
        return reject(RJ_NO_OURS);
    }
    if (!hex_u64(val[K_DISC], &h) || strlen(val[K_DISC]) >= sizeof out->disc) {
        return reject(RJ_DISC);
    }
    out->e.state = strcmp(val[K_STATE], "lobby") == 0    ? ST_LOBBY :
                   strcmp(val[K_STATE], "ready") == 0    ? ST_READY :
                   strcmp(val[K_STATE], "starting") == 0 ? ST_STARTING :
                   strcmp(val[K_STATE], "joining") == 0  ? ST_JOINING :
                                                           -1;
    if (out->e.state < 0) {
        return reject(RJ_STATE);
    }
    if (!dec_u32(val[K_GEN], &out->e.gen)) {
        return reject(RJ_GEN);
    }
    /* Starting proposals and joining acknowledgements both identify the
     * chosen peer. The acknowledgement echoes the host's generation. */
    const unsigned start_keys = KBIT(K_HOST) | KBIT(K_PEER);
    if (out->e.state == ST_STARTING || out->e.state == ST_JOINING) {
        if ((seen & start_keys) != start_keys) {
            return reject(RJ_NO_START);
        }
        if (!hex_u64(val[K_PEER], &out->e.peer_id)) { /* 0 is legal: nobody */
            return reject(RJ_PEER);
        }
    } else if (seen & start_keys) {
        return reject(RJ_STRAY);
    }
    if (out->e.state == ST_JOINING) {
        if (!(seen & KBIT(K_OFFER)) || !dec_u32(val[K_OFFER], &out->e.offer)) {
            return reject(RJ_OFFER);
        }
    } else if (seen & KBIT(K_OFFER)) {
        return reject(RJ_STRAY);
    }
    static int ignore_rev = -1;
    if (ignore_rev < 0) {
        const char* e = getenv("MELEE_LAN_IGNORE_REV");
        ignore_rev = e != NULL && (e[0] == '1' || strcmp(e, "true") == 0);
    }
    bool rev_ok = (ignore_rev > 0) || (strcmp(out->rev, pc_app_rev()) == 0);
    out->e.p.compatible = rev_ok && strcmp(out->disc, pc_lan_disc_id()) == 0;
    return true;
}

/* Datagram source as text net.c can getaddrinfo(): IPv4, a v4-mapped v6 as
 * IPv4, IPv6 with %scope when link-local. Shared with net.c, which names
 * the source of a datagram it rejects. */
void net_addr_text(const struct sockaddr* sa, char* out, size_t cap) {
    if (sa->sa_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in*)sa)->sin_addr, out, (socklen_t)cap);
        return;
    }
    const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)sa;
    if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
        inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], out, (socklen_t)cap);
        return;
    }
    inet_ntop(AF_INET6, &a6->sin6_addr, out, (socklen_t)cap);
    if (IN6_IS_ADDR_LINKLOCAL(&a6->sin6_addr) && a6->sin6_scope_id != 0) {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%%%u", (unsigned)a6->sin6_scope_id);
    }
}

/* Two printed addresses from the same family: a ':' means IPv6, and
 * net_addr_text() prints a v4-mapped v6 source as plain IPv4. */
static bool ip_same_family(const char* a, const char* b) {
    return (strchr(a, ':') != NULL) == (strchr(b, ':') != NULL);
}

static int on_record(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry,
    uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl, const void* data, size_t size,
    size_t name_offset, size_t name_length, size_t record_offset, size_t record_length, void* ud) {
    (void)addrlen;
    (void)query_id;
    (void)rclass;
    (void)name_length;
    (void)ud;
    char namebuf[256];
    mdns_string_t name = mdns_string_extract(data, size, &name_offset, namebuf, sizeof namebuf);
    const size_t slen = sizeof SERVICE - 1;
    if (entry == MDNS_ENTRYTYPE_QUESTION) {
        if ((rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY) && name.length == slen &&
            memcmp(name.str, SERVICE, slen) == 0)
        {
            announce_on(sock, s_tx, sizeof s_tx, false);
        }
        return 0;
    }
    if (rtype != MDNS_RECORDTYPE_TXT ||
        (from->sa_family != AF_INET && from->sa_family != AF_INET6) || name.length <= slen ||
        memcmp(name.str + name.length - slen, SERVICE, slen) != 0)
    {
        return 0;
    }
    s_heard = true;
    if (s_no_mcast) {
        s_no_mcast = false;
        pc_log_line("lan: multicast works after all");
    }
    mdns_record_txt_t txt[TXT_MAX_PAIRS];
    Txt t;
    size_t n = mdns_record_parse_txt(data, size, record_offset, record_length, txt, TXT_MAX_PAIRS);
    if (!parse_txt(txt, n, &t)) {
        return 0;
    }
    Entry e = t.e;
    if (e.id == s_id) {
        return 0; /* our own announce, looped back */
    }
    net_addr_text(from, e.p.ip, sizeof e.p.ip);
    int i = 0;
    while (i < s_n && s_peers[i].id != e.id) {
        i++;
    }
    /* The id is whatever the announcer claims, so once an id has been seen
     * from an address, records for it arriving from a DIFFERENT address of
     * the same family are someone else using that name: they would otherwise
     * repoint the address the lobby is about to dial, or (as a goodbye) evict
     * a peer mid-handshake. A second address family is the same machine
     * announcing twice and is still accepted. */
    if (i < s_n && !ip_same_family(s_peers[i].p.ip, e.p.ip)) {
        /* other family: handled below, the IPv4 address stays preferred */
    } else if (i < s_n && strcmp(s_peers[i].p.ip, e.p.ip) != 0) {
        pc_log_line(
            "lan: ignoring a record for %s from %s (it is %s)", e.p.name, e.p.ip, s_peers[i].p.ip);
        return 0;
    }
    /* A record older than the one we hold is stale: gen= only rises within
     * one run of a lobby and a restarted lobby starts above it (pc_lan_start).
     * The two families arrive on two sockets drained one after the other, so
     * a peer re-entering its lobby (goodbye then announce, both families, in
     * the same millisecond) reads here as goodbye4, announce4, goodbye6,
     * announce6: the old incarnation's IPv6 goodbye used to evict the entry
     * its new IPv4 announce had just made, and a stale "starting" record
     * could reinstate a proposal nobody is making any more. Seen on a phone
     * and a PC on one Wi-Fi as "lost 192.168.1.160 / found / lost / found
     * fe80::...%47" on every lobby entry of the other side. */
    if (i < s_n && e.gen < s_peers[i].gen) {
        return 0;
    }
    if (ttl == 0) { /* goodbye */
        if (i < s_n) {
            drop(i);
            if ((s_state == 1 || s_state == 4) && e.id == s_peer_id) {
                fail("peer left lobby");
            }
        }
        return 0;
    }
    if (i == s_n) {
        if (s_n == PC_LAN_MAX_PEERS) {
            if (!s_full) {
                s_full = true;
                pc_log_line("lan: lobby full (%d peers), ignoring %s", PC_LAN_MAX_PEERS, e.p.name);
            }
            return 0;
        }
        s_n++;
        pc_log_line("lan: found %s %s:%u%s", e.p.name, e.p.ip, e.p.port,
            e.p.compatible ? "" : " (incompatible build, not eligible)");
        if (!e.p.compatible) {
            pc_log_line("lan:   theirs: proto %s rev %s disc %s, ours: proto %s rev %s disc %s",
                t.v, t.rev, t.disc, s_proto, pc_app_rev(), pc_lan_disc_id());
        }
    } else {
        e.last_gen = s_peers[i].last_gen;
        /* Carried across updates so a lobby->starting transition keeps its
         * observation history (a wholesale overwrite would reset it). */
        e.lobby_ns = s_peers[i].lobby_ns;
        /* Seen on both families: keep the IPv4 address. */
        if (strchr(e.p.ip, ':') != NULL && strchr(s_peers[i].p.ip, ':') == NULL) {
            memcpy(e.p.ip, s_peers[i].p.ip, sizeof e.p.ip);
        }
    }
    e.seen_ns = SDL_GetTicksNS();
    e.p.host = e.state == ST_STARTING;
    /* Trust-on-observation (LOBBY_DWELL_NS): stamp the first sighting in a
     * non-starting state; a record that arrives already claiming starting
     * never sets it and is never joined. */
    if ((e.state == ST_LOBBY || e.state == ST_READY) && e.lobby_ns == 0) {
        e.lobby_ns = e.seen_ns;
    }
    s_peers[i] = e;
    return 0;
}

/* ---- interface selection ---------------------------------------------- */

/* Case-insensitive against the lowercase list: Windows friendly names are
 * capitalized ("Tailscale"). Local, not SDL_strncasecmp: the LAN unit test
 * stubs SDL, and strncasecmp's header differs on MinGW (main.c's ieq). */
static bool prefix_ci(const char* s, const char* lower) {
    while (*lower && tolower((unsigned char)*s) == *lower)
        s++, lower++;
    return *lower == '\0';
}

static bool iface_skipped(const char* name) {
    static const char* const virt[] = {"docker", "veth", "br-", "virbr", "tun", "tap", "wg", "utun",
        "zt", "zerotier", "tailscale", "radmin"};
    for (size_t i = 0; i < sizeof virt / sizeof virt[0]; i++) {
        if (prefix_ci(name, virt[i])) {
            return true;
        }
    }
    return false;
}

static void iface_add(
    Iface* v, int* n, const char* name, unsigned index, const struct sockaddr* sa) {
    int i = 0;
    while (i < *n && strcmp(v[i].name, name) != 0) {
        i++;
    }
    if (i == *n) {
        if (*n == MAX_IFACES) {
            return;
        }
        memset(&v[i], 0, sizeof v[i]);
        snprintf(v[i].name, sizeof v[i].name, "%s", name);
        v[i].index = index;
        (*n)++;
    }
    if (sa->sa_family == AF_INET) {
        if (v[i].a.s_addr == 0) {
            v[i].a = ((const struct sockaddr_in*)sa)->sin_addr;
        }
    } else if (sa->sa_family == AF_INET6) {
        const struct in6_addr* a6 = &((const struct sockaddr_in6*)sa)->sin6_addr;
        bool ll = IN6_IS_ADDR_LINKLOCAL(a6);
        if (IN6_IS_ADDR_UNSPECIFIED(&v[i].aaaa) || (v[i].link_local6 && !ll)) {
            v[i].aaaa = *a6;
            v[i].link_local6 = ll;
        }
    }
}

/* Up, non-loopback interfaces with their unicast addresses. */
static int iface_list(Iface* v) {
    int n = 0;
#if defined(_WIN32)
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG len = 16384;
    IP_ADAPTER_ADDRESSES* list = malloc(len);
    if (list != NULL &&
        GetAdaptersAddresses(AF_UNSPEC, flags, NULL, list, &len) == ERROR_BUFFER_OVERFLOW)
    {
        free(list);
        list = malloc(len);
    }
    if (list != NULL && GetAdaptersAddresses(AF_UNSPEC, flags, NULL, list, &len) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = list; a != NULL; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            char name[32];
            if (WideCharToMultiByte(
                    CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name, NULL, NULL) == 0)
            {
                snprintf(name, sizeof name, "%s", a->AdapterName);
            }
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != NULL; u = u->Next) {
                iface_add(v, &n, name, a->Ipv6IfIndex, u->Address.lpSockaddr);
            }
        }
    }
    free(list);
#else
    struct ifaddrs* list;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs* a = list; a != NULL; a = a->ifa_next) {
            unsigned want = IFF_UP | IFF_RUNNING;
            if (a->ifa_addr != NULL && (a->ifa_flags & (want | IFF_LOOPBACK)) == want) {
                iface_add(v, &n, a->ifa_name, if_nametoindex(a->ifa_name), a->ifa_addr);
            }
        }
        freeifaddrs(list);
    }
#endif
    return n;
}

/* Our IPv4 source on the route to `to` (a connected, never sent on, UDP
 * socket); false when there is no route. */
static bool route_to(const struct sockaddr_in* to, struct in_addr* out) {
    struct sockaddr_in self;
    socklen_t len = sizeof self;
    int probe = (int)socket(AF_INET, SOCK_DGRAM, 0);
    bool ok = probe >= 0 && connect(probe, (const struct sockaddr*)to, sizeof *to) == 0 &&
              getsockname(probe, (struct sockaddr*)&self, &len) == 0;
    if (probe >= 0) {
        sock_close(probe);
    }
    if (ok) {
        *out = self.sin_addr;
    }
    return ok;
}

/* Send on, and also listen on, the picked interface: mdns.h joined the
 * groups on the kernel's default one, which can be a virtual interface. */
static void mcast_iface(const Iface* f) {
    if (s_sock4 >= 0 && f->a.s_addr != 0) {
        struct ip_mreq req;
        memset(&req, 0, sizeof req);
        req.imr_multiaddr.s_addr = htonl(0xE00000FBu);
        req.imr_interface = f->a;
        setsockopt(s_sock4, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&f->a, sizeof f->a);
        setsockopt(s_sock4, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&req,
            sizeof req); /* EADDRINUSE: already the default */
    }
    if (s_sock6 >= 0 && f->index != 0) {
        struct ipv6_mreq req;
        unsigned index = f->index;
        memset(&req, 0, sizeof req);
        req.ipv6mr_multiaddr.s6_addr[0] = 0xFF;
        req.ipv6mr_multiaddr.s6_addr[1] = 0x02;
        req.ipv6mr_multiaddr.s6_addr[15] = 0xFB;
        req.ipv6mr_interface = index;
        setsockopt(s_sock6, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char*)&index, sizeof index);
        setsockopt(s_sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP, (const char*)&req, sizeof req);
    }
}

/* The address our A/AAAA records and host= carry: the interface on the
 * route to the mDNS group unless it is a container/VPN one and a plain one
 * exists, else the first plain one (IPv4 first), else anything. */
static void pick_iface(void) {
    Iface v[MAX_IFACES];
    int n = iface_list(v);
    struct sockaddr_in group;
    struct in_addr route;
    memset(&group, 0, sizeof group);
    group.sin_family = AF_INET;
    group.sin_port = htons(MDNS_PORT);
    group.sin_addr.s_addr = htonl(0xE00000FBu); /* 224.0.0.251 */
    int on_route = -1;
    if (route_to(&group, &route)) {
        for (int i = 0; i < n && on_route < 0; i++) {
            if (v[i].a.s_addr == route.s_addr) {
                on_route = i;
            }
        }
        if (on_route < 0 && n < MAX_IFACES) { /* enumeration missed it: still usable */
            memset(&v[n], 0, sizeof v[n]);
            strcpy(v[n].name, "?");
            v[n].a = route;
            on_route = n++;
        }
    }
    int pick = -1, best = -1;
    for (int i = 0; i < n; i++) {
        int score = !iface_skipped(v[i].name) * 4 + (i == on_route) * 2 + (v[i].a.s_addr != 0);
        if (score > best) {
            best = score;
            pick = i;
        }
    }
    memset(&s_self4, 0, sizeof s_self4);
    memset(&s_self6, 0, sizeof s_self6);
    strcpy(s_self_ip, "0.0.0.0");
    if (pick < 0) {
        pc_log_line("lan: no usable network interface");
        return;
    }
    s_self4 = v[pick].a;
    s_self6 = v[pick].aaaa;
    mcast_iface(&v[pick]);
    char a[16] = "-", aaaa[46] = "-";
    if (s_self4.s_addr != 0) {
        inet_ntop(AF_INET, &s_self4, a, sizeof a);
    }
    if (!IN6_IS_ADDR_UNSPECIFIED(&s_self6)) {
        inet_ntop(AF_INET6, &s_self6, aaaa, sizeof aaaa);
    }
    snprintf(s_self_ip, sizeof s_self_ip, "%s", s_self4.s_addr != 0 ? a : aaaa);
    pc_log_line("lan: interface %s %s %s%s", v[pick].name, a, aaaa,
        on_route >= 0 && pick != on_route ? " (the multicast route is on a virtual interface)" :
                                            "");
}

/* ---- lobby -------------------------------------------------------------- */

void pc_lan_start(void) {
    if (s_started) {
        return;
    }
    s_started = true;
    pc_android_multicast_lock_acquire(); /* Android: mDNS answers are filtered without it */
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (!s_atexit) { /* a plain exit from the lobby still says goodbye */
        s_atexit = true;
        atexit(pc_lan_stop);
    }
    struct sockaddr_in any4;
    struct sockaddr_in6 any6;
    memset(&any4, 0, sizeof any4);
    memset(&any6, 0, sizeof any6);
    any4.sin_family = AF_INET;
    any4.sin_port = htons(MDNS_PORT);
    any6.sin6_family = AF_INET6;
    any6.sin6_port = htons(MDNS_PORT);
    any6.sin6_addr = in6addr_any;
    s_sock4 = mdns_socket_open_ipv4(&any4);
    s_sock6 = mdns_socket_open_ipv6(&any6);
    s_no_mcast = s_sock4 < 0 && s_sock6 < 0;
    if (s_no_mcast) {
        pc_log_line(
            "lan: cannot open an mDNS socket on udp/%d (v4 or v6): no discovery, direct ip only",
            MDNS_PORT);
    } else if (s_sock4 < 0 || s_sock6 < 0) {
        pc_log_line("lan: mDNS on IPv%c only", s_sock4 < 0 ? '6' : '4');
    }
    pc_lan_local_name();
    const char* p = getenv("MELEE_NET_PORT");
    s_port = (uint16_t)(p ? atoi(p) : 41000);
    /* Persistent per install so the election is stable, mixed with the game
     * port so two instances of one install (one machine, test rigs) differ. */
    s_id = pc_install_id() ^ (uint64_t)s_port << 48;
    snprintf(s_proto, sizeof s_proto, "%d", PC_NET_PROTO_VERSION);
    snprintf(
        s_instance, sizeof s_instance, "%s-%016llx." SERVICE, s_name, (unsigned long long)s_id);
    snprintf(s_hostname, sizeof s_hostname, "%s.local.", s_name);
    /* Newer than any gen a previous run of ours announced: across processes
     * by the clock, and across a lobby re-entry within one (the menu stops
     * and restarts us in the same second, after Start may have bumped gen
     * past the clock) by construction. on_record() drops older records. */
    uint32_t now_s = (uint32_t)time(NULL);
    s_gen = now_s > s_gen ? now_s : s_gen + 1;
    s_n = 0;
    s_full = false;
    s_heard = false;
    s_rj_logged = 0;
    s_state = 0;
    s_why = NULL;
    s_hosting = false;
    s_offer_pending = false;
    s_offer_gen = 0;
    s_peer_id = 0;
    s_barrier_ns = 0;
    pick_iface();
    if (s_sock4 >= 0) {
        mdns_query_send(
            s_sock4, MDNS_RECORDTYPE_PTR, MDNS_STRING_CONST(SERVICE), s_tx, sizeof s_tx, 0);
    }
    if (s_sock6 >= 0) {
        mdns_query_send(
            s_sock6, MDNS_RECORDTYPE_PTR, MDNS_STRING_CONST(SERVICE), s_tx, sizeof s_tx, 0);
    }
    announce(s_tx, sizeof s_tx, false);
    s_start_ns = s_announce_ns = SDL_GetTicksNS();
    pc_log_line("lan: %s %s id %016llx proto %s rev %s disc %s game port %u",
        s_no_mcast ? "lobby without mDNS:" : "announcing", s_name, (unsigned long long)s_id,
        s_proto, pc_app_rev(), pc_lan_disc_id(), s_port);
}

void pc_lan_stop(void) {
    if (!s_started) {
        return;
    }
    timer_stop();
    if (s_state == 1) {
        pc_net_disconnect(); /* a session that never became a match */
    }
    s_state = 0; /* goodbye goes out as a plain lobby record */
    s_hosting = false;
    announce(s_tx, sizeof s_tx, true);
    if (s_sock4 >= 0) {
        mdns_socket_close(s_sock4);
    }
    if (s_sock6 >= 0) {
        mdns_socket_close(s_sock6);
    }
    s_sock4 = s_sock6 = -1;
    pc_android_multicast_lock_release();
    s_started = false;
    s_no_mcast = false;
    s_n = 0;
    s_full = false;
    s_why = NULL;
    s_peer_id = 0;
    s_barrier_ns = 0;
    pc_log_line("lan: stopped");
}

static bool open_host(Entry* e) {
    s_offer_pending = false;
    s_t0_ns = SDL_GetTicksNS(); /* proposal retry time is not handshake time */
    pc_log_line("lan: host election: we host as P1, guest %s %s:%u", e->p.name, e->p.ip, e->p.port);
    if (!pc_net_connect(e->p.ip, e->p.port, 0, s_seed)) {
        fail("connect failed");
        return false;
    }
    /* RULES now, before this thread's first tick can park in the lockstep
     * wait for the guest's first input: the guest accepts no host datagram
     * until a RULES has told it the session id (net.c recv_inputs), so a
     * host that died inside that wait left the guest nothing it could hear,
     * and it sat in the 60 s first-packet wait instead of timing a silent
     * peer out. The reliable lane's timer resends it from here on whatever
     * this thread does. tools/net_lan_test.py host_dies (guest's inputs held
     * 3 s off the host, host killed 1 s in): no "lan: failed:" within 20 s
     * before, on the base build too; "connection lost" 9.0 s after the kill
     * with this. poll_connecting() repeats the call (idempotent) until
     * READY is in. */
    pc_net_host_match(s_seed, &s_start_frame);
    timer_start();
    pc_log_line("lan: connect %s:%u as P1 seed=%08x", e->p.ip, e->p.port, s_seed);
    return true;
}

static void connect_as_host(Entry* e) {
    timer_stop();
    s_hosting = true;
    s_peer_id = e->id;
    s_seed = (uint32_t)SDL_GetTicks() ^ (uint32_t)s_id;
    s_state = 1;
    s_barrier_ns = 0;
    s_t0_ns = SDL_GetTicksNS();
    s_gen++;
    s_offer_pending = e->id != 0; /* direct IP already has a shared address tie-break */
    pc_log_line("lan: host proposal: P1, guest %s %s:%u", e->p.name, e->p.ip, e->p.port);
    announce(s_tx, sizeof s_tx, false);
    if (!s_offer_pending) {
        open_host(e);
    }
}

static void connect_as_guest(Entry* e) {
    pc_log_line("lan: host election: %s %s:%u hosts, joining as P2", e->p.name, e->p.ip, e->p.port);
    timer_stop();
    s_hosting = false;
    s_offer_pending = false;
    s_offer_gen = e->gen;
    s_peer_id = e->id;
    s_state = 1;
    s_barrier_ns = 0;
    s_t0_ns = SDL_GetTicksNS();
    /* Keep acknowledging this proposal while the first netplay tick waits
     * for the host. A lost multicast acknowledgement must be recoverable. */
    announce(s_tx, sizeof s_tx, false);
    if (e->id != 0) {
        timer_start();
    }
    if (!pc_net_connect(e->p.ip, e->p.port, 1, 0)) {
        fail("connect failed");
        return;
    }
    pc_log_line("lan: connect %s:%u as P2", e->p.ip, e->p.port);
}

/* The ready decision (state 4, ELECTION_NS after Start): yield to a lower
 * ready id, else host the best peer. */
static void elect(void) {
    Entry* guest = NULL;
    for (int i = 0; i < s_n; i++) {
        Entry* e = &s_peers[i];
        if (!e->p.compatible || e->state >= ST_STARTING) {
            continue;
        }
        if (e->state == ST_READY && e->id < s_id) {
            return; /* they host; their starting record picks us */
        }
        /* Lowest id among the ready ones, else lowest id overall. */
        if (guest == NULL || (e->state == ST_READY) > (guest->state == ST_READY) ||
            (e->state == guest->state && e->id < guest->id))
        {
            guest = e;
        }
    }
    if (guest != NULL) {
        connect_as_host(guest);
    }
}

/* State 1: the RULES/READY handshake, then one READY_BARRIER each way so
 * neither side reports the match before the other is through. */
static void poll_connecting(uint64_t now) {
    if (s_offer_pending) {
        for (int i = 0; i < s_n; i++) {
            Entry* e = &s_peers[i];
            if (e->id != s_peer_id || !e->p.compatible || e->peer_id != s_id) {
                continue;
            }
            /* Both Start announcements may have been lost. Resolve the two
             * proposals before either host can block waiting for P2. */
            if (e->state == ST_STARTING && e->id < s_id && e->gen > e->last_gen &&
                e->lobby_ns != 0 && now - e->lobby_ns >= LOBBY_DWELL_NS)
            {
                e->last_gen = e->gen;
                connect_as_guest(e);
                return;
            }
            if (e->state == ST_JOINING && e->offer == s_gen) {
                pc_log_line("lan: host election: guest acknowledged proposal %u", s_gen);
                open_host(e);
                return;
            }
        }
        if (now - s_t0_ns > TIMEOUT_NS) {
            fail("host proposal not acknowledged");
        }
        return;
    }
    if (!pc_net_active()) {
        int why = pc_net_peer_status();
        fail(why == PC_NET_PEER_INCOMPATIBLE ? "incompatible version" :
             why == PC_NET_PEER_RESUME       ? "could not resume" :
             why == PC_NET_PEER_LEFT         ? "peer left" :
                                               "connection lost");
        return;
    }
    if (s_barrier_ns == 0) {
        bool ok = s_hosting ? pc_net_host_match(s_seed, &s_start_frame) :
                              pc_net_guest_wait_match(&s_seed, &s_start_frame);
        if (ok) {
            s_barrier_ns = now;
            s_scene = scene_kind();
            uint8_t scene = (uint8_t)s_scene;
            if (!pc_net_send_reliable(REL_READY_BARRIER, &scene, 1)) {
                fail("ready barrier not sent");
                return;
            }
            pc_log_line("lan: handshake done, waiting for the peer's ready barrier");
        }
    } else {
        uint8_t type, buf[16];
        int n;
        while ((n = pc_net_recv_reliable(&type, buf, sizeof buf)) >= 0) {
            if (type == REL_READY_BARRIER) {
                if (pc_net_frame() > s_start_frame) {
                    fail("ready barrier arrived after the start frame");
                    return;
                }
                /* -2: an empty barrier, from a build before it carried one */
                int theirs = n == 1 ? (int8_t)buf[0] : -2;
                if (theirs != s_scene || scene_kind() != s_scene) {
                    pc_log_line("lan: refusing the match: the peer is on scene %d, we are on "
                                "scene %d (%d when our barrier went out)",
                        theirs, scene_kind(), s_scene);
                    fail("peer is on another scene");
                    return;
                }
                timer_stop();
                s_state = 2;
                pc_log_line("lan: match start seed=%08x start_frame=%d as P%d", s_seed,
                    s_start_frame, s_hosting ? 1 : 2);
                return;
            }
            pc_log_line("lan: dropped reliable type %02x (%d bytes) before the match", type, n);
        }
    }
    if (pc_net_handshake_state() == 3) {
        fail("handshake failed");
    } else if (s_barrier_ns != 0 && now - s_barrier_ns > TIMEOUT_NS) {
        fail("peer never became ready");
    } else if (s_barrier_ns == 0 && now - s_t0_ns > TIMEOUT_NS) {
        fail("timeout");
    }
}

void pc_lan_poll(void) {
    if (!s_started) {
        return;
    }
    for (int i = 0; i < 32 && s_sock4 >= 0; i++) {
        if (mdns_socket_listen(s_sock4, s_rx, sizeof s_rx, on_record, NULL) == 0) {
            break;
        }
    }
    for (int i = 0; i < 32 && s_sock6 >= 0; i++) {
        if (mdns_socket_listen(s_sock6, s_rx, sizeof s_rx, on_record, NULL) == 0) {
            break;
        }
    }
    uint64_t now = SDL_GetTicksNS();
    for (int i = 0; i < s_n;) {
        if (now - s_peers[i].seen_ns > LOST_NS) {
            drop(i);
        } else {
            i++;
        }
    }
    if (s_n < PC_LAN_MAX_PEERS) {
        s_full = false;
    }
    if (!s_heard && !s_no_mcast && now - s_start_ns > LOST_NS) {
        s_no_mcast = true; /* not even our own announce looped back */
        pc_log_line(
            "lan: nothing heard on mDNS in 5 s: multicast is blocked here, use a direct ip");
    }
    if (now - s_announce_ns >= ANNOUNCE_NS) {
        announce(s_tx, sizeof s_tx, false);
        s_announce_ns = now;
    }
    /* A failure (3) leaves us in the lobby, still announcing state=lobby, so
     * it must still behave like one: a proposal naming us is joined, and
     * pc_lan_start_match() retries. It used to be terminal until the player
     * left and re-entered the lobby, and that exit's goodbye is what failed
     * the other side. In the phone<->PC logs each of the three "Failed: peer
     * left lobby" is the other peer's "lan: stopped" 0.2-0.3 s earlier (the
     * two clocks aligned on a session's shared frames), as it left a lobby
     * that could not answer this side's proposal: twice this state after
     * its own failure, once a fixture session's leftover 2. */
    if (s_state == 0 || s_state == 3 || s_state == 4) {
        for (int i = 0; i < s_n; i++) {
            Entry* e = &s_peers[i];
            if (e->state == ST_STARTING && e->peer_id == s_id && e->p.compatible &&
                e->gen > e->last_gen && e->lobby_ns != 0 && now - e->lobby_ns >= LOBBY_DWELL_NS)
            {
                e->last_gen = e->gen;
                connect_as_guest(e);
                return;
            }
        }
        if (s_state == 4) {
            if (now - s_t0_ns > TIMEOUT_NS) {
                fail("no host");
            } else if (now - s_t0_ns > ELECTION_NS) {
                elect();
            }
        }
    } else if (s_state == 1) {
        poll_connecting(now);
        /* Keep the lobby's scheduled OnFrame on this frame until the peer
         * is ready. Pump the transport here: advancing another game tick
         * would let the two lobbies leave on different frames. The host's
         * schedule is known even while it is still waiting for READY. */
        while (s_state == 1 && !s_offer_pending && pc_net_start_frame() >= 0 &&
               pc_net_frame() >= pc_net_start_frame())
        {
            if (pc_net_frame() > pc_net_start_frame()) {
                fail("match start frame already passed");
                break;
            }
            pc_net_poll();
            poll_connecting(SDL_GetTicksNS());
            if (s_state == 1) {
                SDL_DelayNS(1000000);
            }
        }
    } else if (s_state == 2 && !pc_net_active()) {
        /* Only the MELEE_LAN_TEST fixtures (vi.c) and a lobby still counting
         * down poll in 2 once the session is gone; the menu restarts the
         * lobby when a match ends. Stay a joinable lobby member (3) instead
         * of an unanswering one still announcing state=lobby: the phone's
         * fixture sat in 2 after its session and the PC's next proposal to
         * it waited 9 s for an acknowledgement that never came. */
        fail("session ended");
    } else if (s_state == 2 && pc_net_frame() <= s_start_frame && scene_kind() != s_scene) {
        /* A hand-off agreed while both waited for the start frame: the scene
         * the barriers compared is gone, and the peer may have entered a
         * different one. Both sides leave on the same agreed frame, so both
         * refuse here. */
        pc_log_line("lan: refusing the match: our scene went %d -> %d before start frame %d",
            s_scene, scene_kind(), s_start_frame);
        fail("scene changed before the match start");
    }
}

int pc_lan_peers(PcLanPeer* out, int max) {
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_peers[i].p;
    }
    return n;
}

bool pc_lan_full(void) {
    return s_full;
}

bool pc_lan_discovery_unavailable(void) {
    return s_no_mcast;
}

const char* pc_lan_local_name(void) {
    if (s_name[0] == '\0') {
        char host[64];
        const char* dev = pc_android_device_name(); /* NULL off Android */
        if (dev != NULL) {
            snprintf(host, sizeof host, "%s", dev);
        } else if (gethostname(host, sizeof host) != 0) {
            strcpy(host, "melee");
        }
        host[sizeof host - 1] = '\0';
        host[strcspn(host, ".")] = '\0';
        /* Truncated to the label the announce and parse_txt both bound. */
        snprintf(s_name, sizeof s_name, "%.*s", (int)sizeof s_name - 1, host);
    }
    return s_name;
}

bool pc_lan_start_match(void) {
    if (!s_started || (s_state != 0 && s_state != 3) || s_n == 0) {
        return false;
    }
    s_state = 4;
    s_t0_ns = SDL_GetTicksNS();
    s_gen++;
    announce(s_tx, sizeof s_tx, false);
    pc_log_line("lan: ready, electing a host among %d peers", s_n);
    return true;
}

bool pc_lan_connect_direct(const char* ip, uint16_t port) {
    if (!s_started || s_state == 1 || s_state == 2) {
        return false;
    }
    /* The lower ip:port hosts. Our own address on the route to the peer
     * comes from a connected (never sent on) UDP socket. */
    struct sockaddr_in peer;
    struct in_addr self;
    memset(&peer, 0, sizeof peer);
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &peer.sin_addr) != 1) {
        pc_log_line("lan: direct: bad ip %s", ip);
        return false;
    }
    if (!route_to(&peer, &self)) {
        pc_log_line("lan: direct: no route to %s", ip);
        return false;
    }
    inet_ntop(AF_INET, &self, s_self_ip, sizeof s_self_ip);
    uint64_t mine = (uint64_t)ntohl(self.s_addr) << 16 | s_port;
    uint64_t theirs = (uint64_t)ntohl(peer.sin_addr.s_addr) << 16 | port;
    if (mine == theirs) {
        pc_log_line("lan: direct: %s:%u is ourselves", ip, port);
        return false;
    }
    Entry e;
    memset(&e, 0, sizeof e);
    snprintf(e.p.name, sizeof e.p.name, "%s", ip);
    snprintf(e.p.ip, sizeof e.p.ip, "%s", ip);
    e.p.port = port;
    e.p.compatible = true; /* net.c refuses another protocol version at the first packet */
    pc_log_line("lan: direct %s:%u, we are %s:%u: %s", ip, port, s_self_ip, s_port,
        mine < theirs ? "hosting" : "joining");
    if (mine < theirs) {
        connect_as_host(&e);
    } else {
        connect_as_guest(&e);
    }
    return s_state == 1;
}

int pc_lan_state(const char** why) {
    if (why) {
        *why = s_why;
    }
    return s_state;
}

bool pc_lan_is_host(void) {
    return s_hosting;
}

uint32_t pc_lan_seed(void) {
    return s_seed;
}

int32_t pc_lan_start_frame(void) {
    return s_start_frame;
}
