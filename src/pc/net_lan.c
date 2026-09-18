/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LAN lobby: mDNS/DNS-SD discovery of other melee-pc instances and the
 * host-picks-guest match start (docs/netcode-plan.md §8).
 *
 * Every instance announces `<name>-<id>._meleepc._udp.local.` once a second
 * as an unsolicited multicast answer (PTR + SRV + TXT) and answers PTR
 * queries for the service, so a fresh instance sees everyone at once. TXT:
 *   v=1 id=<random 32-bit hex> name=<hostname> port=<game udp port>
 *   state=lobby|starting host=<our ip:port> peer=<guest id>   (last two when starting)
 * Peers are keyed by id (our own looped-back record is skipped, but its
 * source address tells us our own ip); a peer silent for 5 s, or one that
 * sent a goodbye (ttl 0), is dropped.
 *
 * Match start: the first instance to press Start picks its first peer,
 * flips its record to state=starting peer=<that id> and opens the netplay
 * session as P1. The first netplay tick blocks the game thread until the
 * guest's inputs arrive, so while that record is live it is repeated from
 * an SDL timer rather than from pc_lan_poll(). The chosen guest sees the
 * flip and connects as P2 to the announcer's source ip + TXT port. Both
 * then poll the RULES/READY handshake in net.c once per frame.
 *
 * MELEE_LAN_TEST=1|host runs this without the menu (os.c/vi.c); two
 * instances need distinct MELEE_NET_PORT and MELEE_CACHE_DIR.
 *
 * ponytail: IPv4 only, one interface (INADDR_ANY), no A record and only
 * service PTR questions answered (peers use the datagram source address,
 * so third-party browsers list us but cannot resolve us); add IPv6/A when
 * a v6-only LAN shows up. */
#include "pc/net_lan.h"
#include "pc/pc.h"

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include "mdns/mdns.h"

#define SERVICE "_meleepc._udp.local."
#define ANNOUNCE_NS 1000000000ull
#define LOST_NS 5000000000ull
#define TIMEOUT_NS 15000000000ull
#define MSTR(s) { (s), strlen(s) }

typedef struct Entry {
    uint32_t id;
    uint32_t peer_id; /* guest chosen by a starting host */
    bool starting;
    uint64_t seen_ns;
    PcLanPeer p;
} Entry;

static int s_sock = -1;
static uint32_t s_id;
static char s_name[PC_LAN_NAME_LEN];
static char s_instance[64]; /* <name>-<id>._meleepc._udp.local. */
static char s_hostname[32]; /* <name>.local. */
static char s_self_ip[46];  /* learned from our own looped-back announce */
static uint16_t s_port;
static Entry s_peers[PC_LAN_MAX_PEERS];
static int s_n;
static uint64_t s_announce_ns;
static uint32_t s_rx[512]; /* mdns.h wants 32-bit aligned buffers */
static uint32_t s_tx[512];
static SDL_TimerID s_timer;

/* Lobby state machine (pc_lan_state()). */
static int s_state;
static const char* s_why;
static bool s_host;
static uint32_t s_peer_id;
static uint64_t s_t0_ns;
static uint32_t s_seed;
static int32_t s_start_frame;

static void announce(void* buf, size_t cap, bool goodbye) {
    char port[8], id[12], peer[12], host[64];
    snprintf(port, sizeof port, "%u", s_port);
    snprintf(id, sizeof id, "%08x", s_id);
    snprintf(peer, sizeof peer, "%08x", s_peer_id);
    snprintf(host, sizeof host, "%s:%u", s_self_ip, s_port);
    bool starting = s_host && s_state >= 1;
    mdns_record_t ptr = { .name = MSTR(SERVICE),
                          .type = MDNS_RECORDTYPE_PTR,
                          .data.ptr.name = MSTR(s_instance) };
#define TXT(k, v) { .name = MSTR(s_instance), .type = MDNS_RECORDTYPE_TXT, .data.txt = { MSTR(k), MSTR(v) } }
    mdns_record_t extra[] = {
        { .name = MSTR(s_instance),
          .type = MDNS_RECORDTYPE_SRV,
          .data.srv = { 0, 0, s_port, MSTR(s_hostname) } },
        TXT("v", "1"),
        TXT("id", id),
        TXT("name", s_name),
        TXT("port", port),
        TXT("state", starting ? "starting" : "lobby"),
        TXT("host", host),
        TXT("peer", peer),
    };
#undef TXT
    size_t n = sizeof extra / sizeof extra[0] - (starting ? 0 : 2);
    (goodbye ? mdns_goodbye_multicast : mdns_announce_multicast)(s_sock, buf, cap, ptr, NULL, 0, extra, n);
}

/* Repeats the state=starting record while the host's game thread is stuck in
 * the first lockstep wait (SDL timer thread; own buffer). */
static Uint32 SDLCALL announce_timer(void* ud, SDL_TimerID id, Uint32 interval) {
    static uint32_t buf[512];
    (void) ud;
    (void) id;
    if (s_state != 1) {
        return 0;
    }
    announce(buf, sizeof buf, false);
    return interval;
}

static void fail(const char* why) {
    s_state = 3;
    s_why = why;
    pc_log_line("lan: failed: %s", why);
}

static void drop(int i) {
    pc_log_line("lan: lost %s %s:%u", s_peers[i].p.name, s_peers[i].p.ip, s_peers[i].p.port);
    s_peers[i] = s_peers[--s_n];
}

static void txt_copy(char* dst, size_t cap, mdns_string_t v) {
    size_t n = v.length < cap - 1 ? v.length : cap - 1;
    memcpy(dst, v.str, n);
    dst[n] = '\0';
}

static int on_record(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry,
                     uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl,
                     const void* data, size_t size, size_t name_offset, size_t name_length,
                     size_t record_offset, size_t record_length, void* ud) {
    (void) sock;
    (void) addrlen;
    (void) query_id;
    (void) rclass;
    (void) name_length;
    (void) ud;
    char namebuf[256];
    mdns_string_t name = mdns_string_extract(data, size, &name_offset, namebuf, sizeof namebuf);
    const size_t slen = sizeof SERVICE - 1;
    if (entry == MDNS_ENTRYTYPE_QUESTION) {
        if ((rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY) && name.length == slen &&
            memcmp(name.str, SERVICE, slen) == 0) {
            announce(s_tx, sizeof s_tx, false);
        }
        return 0;
    }
    if (rtype != MDNS_RECORDTYPE_TXT || from->sa_family != AF_INET || name.length <= slen ||
        memcmp(name.str + name.length - slen, SERVICE, slen) != 0) {
        return 0;
    }
    mdns_record_txt_t txt[8];
    size_t n = mdns_record_parse_txt(data, size, record_offset, record_length, txt, 8);
    Entry e;
    memset(&e, 0, sizeof e);
    for (size_t i = 0; i < n; i++) {
        char val[64];
        txt_copy(val, sizeof val, txt[i].value);
        const char* k = txt[i].key.str;
        size_t kl = txt[i].key.length;
        if (kl == 2 && memcmp(k, "id", 2) == 0) {
            e.id = (uint32_t) strtoul(val, NULL, 16);
        } else if (kl == 4 && memcmp(k, "name", 4) == 0) {
            txt_copy(e.p.name, sizeof e.p.name, txt[i].value);
        } else if (kl == 4 && memcmp(k, "port", 4) == 0) {
            e.p.port = (uint16_t) atoi(val);
        } else if (kl == 5 && memcmp(k, "state", 5) == 0) {
            e.starting = strcmp(val, "starting") == 0;
        } else if (kl == 4 && memcmp(k, "peer", 4) == 0) {
            e.peer_id = (uint32_t) strtoul(val, NULL, 16);
        }
    }
    inet_ntop(AF_INET, &((const struct sockaddr_in*) from)->sin_addr, e.p.ip, sizeof e.p.ip);
    if (e.id == s_id) {
        memcpy(s_self_ip, e.p.ip, sizeof s_self_ip);
        return 0;
    }
    if (e.id == 0 || e.p.port == 0) {
        return 0;
    }
    int i = 0;
    while (i < s_n && s_peers[i].id != e.id) {
        i++;
    }
    if (ttl == 0) { /* goodbye */
        if (i < s_n) {
            drop(i);
        }
        return 0;
    }
    if (i == s_n) {
        if (s_n == PC_LAN_MAX_PEERS) {
            return 0;
        }
        s_n++;
        pc_log_line("lan: found %s %s:%u", e.p.name, e.p.ip, e.p.port);
    }
    e.seen_ns = SDL_GetTicksNS();
    e.p.host = e.starting;
    s_peers[i] = e;
    return 0;
}

void pc_lan_start(void) {
    if (s_sock >= 0) {
        return;
    }
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    struct sockaddr_in any;
    memset(&any, 0, sizeof any);
    any.sin_family = AF_INET;
    any.sin_port = htons(MDNS_PORT);
    s_sock = mdns_socket_open_ipv4(&any);
    if (s_sock < 0) {
        pc_log_line("lan: cannot open mDNS socket on udp/%d", MDNS_PORT);
        return;
    }
    pc_lan_local_name();
    s_id = (uint32_t) SDL_GetPerformanceCounter() ^ ((uint32_t) getpid() << 16);
    snprintf(s_instance, sizeof s_instance, "%s-%08x." SERVICE, s_name, s_id);
    snprintf(s_hostname, sizeof s_hostname, "%s.local.", s_name);
    const char* p = getenv("MELEE_NET_PORT");
    s_port = (uint16_t) (p ? atoi(p) : 41000);
    strcpy(s_self_ip, "0.0.0.0");
    s_n = 0;
    s_state = 0;
    s_why = NULL;
    s_host = false;
    s_peer_id = 0;
    mdns_query_send(s_sock, MDNS_RECORDTYPE_PTR, MDNS_STRING_CONST(SERVICE), s_tx, sizeof s_tx, 0);
    announce(s_tx, sizeof s_tx, false);
    s_announce_ns = SDL_GetTicksNS();
    pc_log_line("lan: announcing %s id %08x game port %u, browsing " SERVICE, s_name, s_id, s_port);
}

void pc_lan_stop(void) {
    if (s_sock < 0) {
        return;
    }
    if (s_timer) {
        SDL_RemoveTimer(s_timer);
        s_timer = 0;
    }
    if (s_state == 1) {
        pc_net_disconnect(); /* a session that never became a match */
    }
    announce(s_tx, sizeof s_tx, true);
    mdns_socket_close(s_sock);
    s_sock = -1;
    s_n = 0;
    s_state = 0;
    s_why = NULL;
    s_host = false;
    pc_log_line("lan: stopped");
}

void pc_lan_poll(void) {
    if (s_sock < 0) {
        return;
    }
    for (int i = 0; i < 32; i++) {
        if (mdns_socket_listen(s_sock, s_rx, sizeof s_rx, on_record, NULL) == 0) {
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
    if (now - s_announce_ns >= ANNOUNCE_NS) {
        announce(s_tx, sizeof s_tx, false);
        s_announce_ns = now;
    }
    if (s_state == 0) {
        for (int i = 0; i < s_n; i++) {
            Entry* e = &s_peers[i];
            if (!e->starting || e->peer_id != s_id) {
                continue;
            }
            pc_log_line("lan: host election: %s %s:%u hosts, joining as P2", e->p.name, e->p.ip,
                        e->p.port);
            s_host = false;
            s_state = 1;
            s_t0_ns = now;
            if (!pc_net_connect(e->p.ip, e->p.port, 1, 0)) {
                fail("connect failed");
                return;
            }
            pc_log_line("lan: connect %s:%u as P2", e->p.ip, e->p.port);
            return;
        }
    } else if (s_state == 1) {
        bool ok = s_host ? pc_net_host_match(s_seed, &s_start_frame)
                         : pc_net_guest_wait_match(&s_seed, &s_start_frame);
        if (ok) {
            s_state = 2;
            pc_log_line("lan: match start seed=%08x start_frame=%d as P%d", s_seed, s_start_frame,
                        s_host ? 1 : 2);
        } else if (now - s_t0_ns > TIMEOUT_NS) {
            pc_net_disconnect();
            fail("timeout");
        }
    }
}

int pc_lan_peers(PcLanPeer* out, int max) {
    for (int i = 0; i < s_n && i < max; i++) {
        out[i] = s_peers[i].p;
    }
    return s_n;
}

const char* pc_lan_local_name(void) {
    if (s_name[0] == '\0') {
        char host[64];
        if (gethostname(host, sizeof host) != 0) {
            strcpy(host, "melee");
        }
        host[sizeof host - 1] = '\0';
        host[strcspn(host, ".")] = '\0';
        snprintf(s_name, sizeof s_name, "%s", host);
    }
    return s_name;
}

bool pc_lan_start_match(void) {
    if (s_sock < 0 || s_state != 0 || s_n == 0) {
        return false;
    }
    Entry* e = &s_peers[0];
    s_host = true;
    s_peer_id = e->id;
    s_seed = (uint32_t) SDL_GetTicks() ^ s_id;
    s_state = 1;
    s_t0_ns = SDL_GetTicksNS();
    pc_log_line("lan: host election: we host as P1, guest %s %s:%u", e->p.name, e->p.ip, e->p.port);
    /* The guest must learn we are starting before the session opens (see
     * the header comment): flipped record out now, then from the timer. */
    announce(s_tx, sizeof s_tx, false);
    s_timer = SDL_AddTimer(500, announce_timer, NULL);
    if (!pc_net_connect(e->p.ip, e->p.port, 0, s_seed)) {
        fail("connect failed");
        return false;
    }
    pc_log_line("lan: connect %s:%u as P1 seed=%08x", e->p.ip, e->p.port, s_seed);
    return true;
}

int pc_lan_state(const char** why) {
    if (why) {
        *why = s_why;
    }
    return s_state;
}

bool pc_lan_is_host(void) {
    return s_host;
}

uint32_t pc_lan_seed(void) {
    return s_seed;
}

int32_t pc_lan_start_frame(void) {
    return s_start_frame;
}
