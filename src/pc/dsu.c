/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * DSU (Cemuhook UDP) client: reads controllers from a DSU server such as NSO
 * GC Driver (macOS, NSO GameCube controllers), DS4Windows or BetterJoy, the
 * same source Dolphin's "Alternate Input Sources" uses.
 *
 * One non-blocking UDP socket, drained from the 1000 Hz input thread next to
 * the GC adapter. Every second it (re)subscribes to all four server slots;
 * servers drop a client that goes quiet for a few seconds. Without an
 * explicit address it subscribes on 127.0.0.1 ports 26760-26764 (the range
 * NSO GC Driver binds) and settles on the first one that answers. DSU slot N
 * is PAD port N, behind a GC-adapter controller on the same port.
 *
 * Sticks arrive as bytes centred on 128 at aurora's +-127 gamepad scale, so
 * they reach the game exactly as the same controller would through SDL. Y is
 * up above 128, the Cemuhook convention and what NSO GC Driver sends (checked
 * with an NSO GameCube controller); MELEE_DSU_INVERT_Y=1 flips it for a
 * server that sends it the other way up.
 *
 * On by default; the launcher's Controls page (launcher.cfg "dsu") or
 * MELEE_DSU=0/1 turns it off. MELEE_DSU_SERVER=host:port pins the server.
 * With no server running it costs one 28-byte loopback datagram per port a
 * second, and SDL's gamepads are left exactly as they are: they are only
 * hidden (hide_driver_gamepads) once a server has answered.
 */
#include "pc/pc.h"

#include <dolphin/pad.h>
/* rumble.h references DiscU16; disc.h defines it for this C TU. */
#include "pc/disc.h"
#include <sysdolphin/baselib/rumble.h>

#include <SDL3/SDL.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET dsu_socket;
#define DSU_BAD_SOCKET INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int dsu_socket;
#define DSU_BAD_SOCKET (-1)
#define sock_close close
#endif

#define DSU_SLOTS 4
#define DSU_PROTOCOL 1001
#define DSU_MSG_VERSION 0x100000u
#define DSU_MSG_PAD_DATA 0x100002u
#define DSU_MSG_RUMBLE 0x110002u
#define DSU_FIRST_PORT 26760
#define DSU_LAST_PORT 26764
/* A slot that sends nothing for this long is unplugged. NSO GC Driver sends
 * on every controller report (~125+ Hz), so this is generous. */
#define DSU_SLOT_TIMEOUT_NS 1000000000ull
#define DSU_SUBSCRIBE_NS 1000000000ull
/* Port probing gives up on a locked port after this much silence. */
#define DSU_SERVER_TIMEOUT_NS 3000000000ull
/* How long pc_dsu_init waits for a server before SDL_Init. A loopback answer
 * takes well under a millisecond; this is only spent when nothing answers. */
#define DSU_PROBE_NS 50000000ull

extern HSD_RumbleData HSD_Rumble_804C22E0[DSU_SLOTS];

static atomic_bool s_enabled;
static bool s_invert_y;
static uint32_t s_client_id;
/* Set by the poll thread once a server answers; the main thread then hides
 * NSO GC Driver's gamepads (s_hidden is main-thread only). */
static atomic_bool s_server_found;
static bool s_hidden;

/* Poll-thread state. */
static dsu_socket s_sock = DSU_BAD_SOCKET;
static struct sockaddr_in s_server; /* sin_port 0 = probe the default range */
static bool s_pinned;               /* MELEE_DSU_SERVER chose the address */
static uint16_t s_locked_port;      /* host order, 0 while probing */
static uint64_t s_last_rx_ns, s_last_sub_ns;
static uint64_t s_slot_seen_ns[DSU_SLOTS];
static PADStatus s_status[DSU_SLOTS];
static bool s_present[DSU_SLOTS];
static uint8_t s_rumble_sent[DSU_SLOTS];
static bool s_warned_socket;

/* Same short copy-only snapshot as gcadapter.c. */
static atomic_flag s_snapshot_lock = ATOMIC_FLAG_INIT;
static PADStatus s_published_status[DSU_SLOTS];
static bool s_published_present[DSU_SLOTS];
static _Atomic unsigned char s_motor_request[DSU_SLOTS];

static void lock_snapshot(void) {
    while (atomic_flag_test_and_set_explicit(&s_snapshot_lock, memory_order_acquire)) {
    }
}

static void unlock_snapshot(void) {
    atomic_flag_clear_explicit(&s_snapshot_lock, memory_order_release);
}

static void publish_snapshot(void) {
    lock_snapshot();
    memcpy(s_published_status, s_status, sizeof(s_status));
    memcpy(s_published_present, s_present, sizeof(s_present));
    unlock_snapshot();
}

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Client header: "DSUC", protocol, payload length, CRC, client id, type. */
static void finish_request(uint8_t* p, size_t n, uint32_t type) {
    memcpy(p, "DSUC", 4);
    put16(p + 4, DSU_PROTOCOL);
    put16(p + 6, (uint16_t)(n - 16));
    put32(p + 8, 0);
    put32(p + 12, s_client_id);
    put32(p + 16, type);
    put32(p + 8, crc32(p, n));
}

static void send_to_port(const uint8_t* p, size_t n, uint16_t port) {
    struct sockaddr_in to = s_server;
    to.sin_port = htons(port);
    (void)sendto(s_sock, (const char*)p, (int)n, 0, (const struct sockaddr*)&to, sizeof(to));
}

static void send_request(const uint8_t* p, size_t n) {
    if (s_locked_port != 0) {
        send_to_port(p, n, s_locked_port);
        return;
    }
    for (uint16_t port = DSU_FIRST_PORT; port <= DSU_LAST_PORT; port++) {
        send_to_port(p, n, port);
    }
}

static void subscribe(void) {
    /* Pad data request, flags 0: every slot the server has. */
    uint8_t p[28] = {0};
    finish_request(p, sizeof(p), DSU_MSG_PAD_DATA);
    send_request(p, sizeof(p));
}

static void send_rumble(int slot, uint8_t intensity) {
    /* flags 1 (slot-based), slot, mac[6], motor, intensity. */
    uint8_t p[30] = {0};
    p[20] = 1;
    p[21] = (uint8_t)slot;
    p[28] = 0;
    p[29] = intensity;
    finish_request(p, sizeof(p), DSU_MSG_RUMBLE);
    send_request(p, sizeof(p));
}

static bool set_nonblocking(dsu_socket s) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static void parse_server(void) {
    memset(&s_server, 0, sizeof(s_server));
    s_server.sin_family = AF_INET;
    s_server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s_pinned = false;
    s_locked_port = 0;
    const char* env = getenv("MELEE_DSU_SERVER");
    if (env == NULL || env[0] == '\0') {
        return;
    }
    char host[64];
    int port = DSU_FIRST_PORT;
    const char* colon = strrchr(env, ':');
    size_t len = colon != NULL ? (size_t)(colon - env) : strlen(env);
    if (len >= sizeof(host)) {
        len = sizeof(host) - 1;
    }
    memcpy(host, env, len);
    host[len] = '\0';
    if (colon != NULL) {
        port = atoi(colon + 1);
    }
    struct in_addr addr;
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, host, &addr) != 1) {
        pc_log_line("DSU: MELEE_DSU_SERVER \"%s\" is not an IPv4 host:port; probing "
                    "127.0.0.1:%d-%d",
            env, DSU_FIRST_PORT, DSU_LAST_PORT);
        return;
    }
    s_server.sin_addr = addr;
    s_pinned = true;
    s_locked_port = (uint16_t)port;
}

static bool open_socket(void) {
#if defined(_WIN32)
    static bool wsa;
    if (!wsa) {
        WSADATA data;
        wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
#endif
    parse_server();
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    /* Bound to loopback for a loopback server, so the socket is never
     * reachable from the network (and Windows has no firewall prompt). */
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = s_server.sin_addr.s_addr == htonl(INADDR_LOOPBACK) ?
                                htonl(INADDR_LOOPBACK) :
                                htonl(INADDR_ANY);
    if (s_sock == DSU_BAD_SOCKET || !set_nonblocking(s_sock) ||
        bind(s_sock, (const struct sockaddr*)&local, sizeof(local)) != 0)
    {
        if (s_sock != DSU_BAD_SOCKET) {
            sock_close(s_sock);
            s_sock = DSU_BAD_SOCKET;
        }
        if (!s_warned_socket) {
            s_warned_socket = true;
            pc_log_line("DSU: could not open a UDP socket");
        }
        return false;
    }
    s_last_rx_ns = 0;
    s_last_sub_ns = 0;
    memset(s_rumble_sent, 0, sizeof(s_rumble_sent));
    char addr[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &s_server.sin_addr, addr, sizeof(addr));
    if (s_pinned) {
        pc_log_line("DSU: client on, server %s:%u", addr, s_locked_port);
    } else {
        pc_log_line("DSU: client on, probing %s:%d-%d", addr, DSU_FIRST_PORT, DSU_LAST_PORT);
    }
    return true;
}

static void clear_slot(int i) {
    if (s_present[i]) {
        pc_log_line("DSU: slot %d disconnected", i + 1);
    }
    s_present[i] = false;
    s_slot_seen_ns[i] = 0;
    memset(&s_status[i], 0, sizeof(s_status[i]));
}

static void close_socket(void) {
    sock_close(s_sock);
    s_sock = DSU_BAD_SOCKET;
    for (int i = 0; i < DSU_SLOTS; i++) {
        clear_slot(i);
    }
    publish_snapshot();
    atomic_store_explicit(&s_server_found, false, memory_order_relaxed);
    pc_log_line("DSU: client off");
}

/* Byte centred on 128 to aurora's +-127 stick; flip negates it. */
static s8 stick(uint8_t v, bool flip) {
    int d = flip ? 128 - (int)v : (int)v - 128;
    return (s8)(d < -127 ? -127 : d > 127 ? 127 : d);
}

/* A whole server message of this type, at least min_len bytes, CRC intact. */
static bool valid_reply(const uint8_t* p, int n, uint32_t type, int min_len) {
    if (n < min_len || n < 20 || memcmp(p, "DSUS", 4) != 0 || le32(p + 16) != type) {
        return false;
    }
    const int len = le16(p + 6) + 16;
    uint8_t buf[256];
    if (len > n || len < min_len || len > (int)sizeof(buf)) {
        return false;
    }
    memcpy(buf, p, (size_t)len);
    const uint32_t crc = le32(buf + 8);
    put32(buf + 8, 0);
    return crc32(buf, (size_t)len) == crc;
}

/* Server pad data: header(16) type(4) slot state model conn mac[6] battery
 * connected counter(4) buttons1 buttons2 home touch lx ly rx ry
 * analog dpad/face/shoulders(12) ... */
static bool parse_pad(const uint8_t* p, int n, uint64_t now) {
    if (!valid_reply(p, n, DSU_MSG_PAD_DATA, 56)) {
        return false;
    }
    const int slot = p[20];
    if (slot >= DSU_SLOTS) {
        return true;
    }
    if (p[21] != 2 || p[31] == 0) {
        clear_slot(slot);
        return true;
    }
    if (!s_present[slot]) {
        pc_log_line("DSU: slot %d connected (port %d)", slot + 1, slot + 1);
    }
    s_present[slot] = true;
    s_slot_seen_ns[slot] = now;

    /* PlayStation-positioned bits; NSO GC Driver's GameCube labels in
     * DSUServer.swift padData(). */
    const uint8_t b1 = p[36], b2 = p[37];
    u16 b = 0;
    if (b1 & 0x80)
        b |= PAD_BUTTON_LEFT;
    if (b1 & 0x40)
        b |= PAD_BUTTON_DOWN;
    if (b1 & 0x20)
        b |= PAD_BUTTON_RIGHT;
    if (b1 & 0x10)
        b |= PAD_BUTTON_UP;
    if (b1 & 0x08)
        b |= PAD_BUTTON_START;
    if (b1 & 0x04)
        b |= PAD_TRIGGER_Z;
    if (b2 & 0x80)
        b |= PAD_BUTTON_Y;
    if (b2 & 0x40)
        b |= PAD_BUTTON_B;
    if (b2 & 0x20)
        b |= PAD_BUTTON_A;
    if (b2 & 0x10)
        b |= PAD_BUTTON_X;
    if (b2 & 0x08)
        b |= PAD_TRIGGER_R;
    if (b2 & 0x04)
        b |= PAD_TRIGGER_L;

    PADStatus* st = &s_status[slot];
    st->button = b;
    st->stickX = stick(p[40], false);
    st->stickY = stick(p[41], s_invert_y);
    st->substickX = stick(p[42], false);
    st->substickY = stick(p[43], s_invert_y);
    st->triggerRight = p[54];
    st->triggerLeft = p[55];
    st->err = PAD_ERR_NONE;
    return true;
}

static void update_rumble(void) {
    /* Game last_status: 0 hard stop, 1 stop, 2 rumble. */
    for (int i = 0; i < DSU_SLOTS; i++) {
        const uint8_t want =
            s_present[i] && atomic_load_explicit(&s_motor_request[i], memory_order_relaxed) == 2 ?
                255 :
                0;
        if (want != s_rumble_sent[i]) {
            s_rumble_sent[i] = want;
            send_rumble(i, want);
        }
    }
}

/* NSO GC Driver also presents each controller as a virtual HID gamepad
 * (SInput 2e8a:10c6, or 05ac:061a in its compatibility mode), and SDL may
 * open the controller itself (057e:2073): once a DSU server is feeding the
 * game, each would be the same controller a second time on the same port,
 * so SDL is told to leave them alone. Only then: with no server, a player
 * using that controller through SDL keeps it. SDL reads the list as each
 * device appears, which is why pc_dsu_init asks for a server before
 * SDL_Init; a server found later only hides pads that connect after it.
 * Main thread only. MELEE_DSU_KEEP_GAMEPADS=1 leaves them to SDL. */
static void hide_driver_gamepads(bool hide) {
    static const char k_ignore[] = "0x2e8a/0x10c6,0x05ac/0x061a,0x057e/0x2073";
    static int s_keep = -1;
    static char s_prev[512];
    if (s_keep < 0) {
        const char* keep = getenv("MELEE_DSU_KEEP_GAMEPADS");
        s_keep = keep != NULL && keep[0] != '\0' && keep[0] != '0';
    }
    if (s_keep || hide == s_hidden) {
        return;
    }
    s_hidden = hide;
    if (!hide) {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, s_prev[0] != '\0' ? s_prev : NULL);
        pc_log_line("DSU: NSO GC Driver's gamepads handed back to SDL");
        return;
    }
    const char* prev = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES);
    snprintf(s_prev, sizeof(s_prev), "%s", prev != NULL ? prev : "");
    char list[sizeof(s_prev) + sizeof(k_ignore) + 1];
    snprintf(list, sizeof(list), "%s%s%s", s_prev, s_prev[0] != '\0' ? "," : "", k_ignore);
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, list);
    pc_log_line("DSU: hiding NSO GC Driver's gamepads from SDL so the controller is read once");
}

void pc_dsu_set_enabled(bool on) {
    if (pc_dsu_env_override()) {
        on = getenv("MELEE_DSU")[0] != '0';
    }
    atomic_store_explicit(&s_enabled, on, memory_order_relaxed);
    if (!on) {
        atomic_store_explicit(&s_server_found, false, memory_order_relaxed);
        hide_driver_gamepads(false);
    }
}

bool pc_dsu_env_override(void) {
    const char* env = getenv("MELEE_DSU");
    return env != NULL && env[0] != '\0';
}

/* Before SDL_Init and before the poll thread exists, so the poll thread's
 * socket and state are used directly and it carries on from here. A version
 * request is the one message every server answers, controller or not. */
void pc_dsu_init(void) {
    const char* inv = getenv("MELEE_DSU_INVERT_Y");
    s_invert_y = inv != NULL && inv[0] != '\0' && inv[0] != '0';
    s_client_id = (uint32_t)SDL_GetPerformanceCounter() | 1u;
    if (!atomic_load_explicit(&s_enabled, memory_order_relaxed) || !open_socket()) {
        return;
    }
    uint8_t req[20] = {0};
    finish_request(req, sizeof(req), DSU_MSG_VERSION);
    send_request(req, sizeof(req));
    const uint64_t start = SDL_GetTicksNS();
    uint8_t pkt[512];
    while (SDL_GetTicksNS() - start < DSU_PROBE_NS) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n =
            (int)recvfrom(s_sock, (char*)pkt, sizeof(pkt), 0, (struct sockaddr*)&from, &from_len);
        if (n > 0 && from.sin_addr.s_addr == s_server.sin_addr.s_addr &&
            valid_reply(pkt, n, DSU_MSG_VERSION, 22))
        {
            s_locked_port = ntohs(from.sin_port);
            pc_log_line("DSU: server answered on port %u at startup", s_locked_port);
            hide_driver_gamepads(true);
            return;
        }
        if (n < 0) {
            SDL_Delay(1);
        }
    }
    pc_log_line("DSU: no server answered at startup; SDL gamepads left as they are");
}

void pc_dsu_poll(void) {
    const bool on = atomic_load_explicit(&s_enabled, memory_order_relaxed);
    if (!on) {
        if (s_sock != DSU_BAD_SOCKET) {
            close_socket();
        }
        return;
    }
    if (s_sock == DSU_BAD_SOCKET) {
        static uint64_t s_retry_ns;
        const uint64_t t = SDL_GetTicksNS();
        if (t - s_retry_ns < 1000000000ull && s_retry_ns != 0) {
            return;
        }
        s_retry_ns = t;
        if (!open_socket()) {
            return;
        }
    }

    const uint64_t now = SDL_GetTicksNS();
    bool changed = false;
    uint8_t pkt[512];
    /* Bounded: Windows reports each ICMP port-unreachable from the probe as
     * a WSAECONNRESET on the next receive. */
    for (int k = 0; k < 64; k++) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n =
            (int)recvfrom(s_sock, (char*)pkt, sizeof(pkt), 0, (struct sockaddr*)&from, &from_len);
        if (n < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAECONNRESET) {
                continue;
            }
#else
            if (errno == ECONNREFUSED || errno == EINTR) {
                continue;
            }
#endif
            break;
        }
        if (from.sin_addr.s_addr != s_server.sin_addr.s_addr) {
            continue;
        }
        if (parse_pad(pkt, n, now)) {
            s_last_rx_ns = now;
            changed = true;
            if (s_locked_port == 0) {
                s_locked_port = ntohs(from.sin_port);
                pc_log_line("DSU: server found on port %u", s_locked_port);
            }
            atomic_store_explicit(&s_server_found, true, memory_order_relaxed);
        }
    }

    for (int i = 0; i < DSU_SLOTS; i++) {
        if (s_present[i] && now - s_slot_seen_ns[i] > DSU_SLOT_TIMEOUT_NS) {
            clear_slot(i);
            changed = true;
        }
    }
    if (!s_pinned && s_locked_port != 0 && now - s_last_rx_ns > DSU_SERVER_TIMEOUT_NS) {
        pc_log_line("DSU: server on port %u went quiet, probing again", s_locked_port);
        s_locked_port = 0;
    }
    if (s_last_sub_ns == 0 || now - s_last_sub_ns >= DSU_SUBSCRIBE_NS) {
        s_last_sub_ns = now;
        subscribe();
    }
    if (changed) {
        publish_snapshot();
    }
    update_rumble();
}

/* Main-thread input boundary, beside pc_gcadapter_apply. */
void pc_dsu_apply(void) {
    if (!s_hidden && atomic_load_explicit(&s_server_found, memory_order_relaxed) &&
        atomic_load_explicit(&s_enabled, memory_order_relaxed))
    {
        hide_driver_gamepads(true);
    }
    for (int i = 0; i < DSU_SLOTS; i++) {
        atomic_store_explicit(
            &s_motor_request[i], HSD_Rumble_804C22E0[i].last_status, memory_order_relaxed);
    }
}

bool pc_dsu_status(int port, PADStatus* out) {
    if (port < 0 || port >= DSU_SLOTS || out == NULL) {
        return false;
    }
    lock_snapshot();
    const bool present = s_published_present[port];
    if (present) {
        *out = s_published_status[port];
    }
    unlock_snapshot();
    return present;
}
