/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_LAN_H
#define PC_NET_LAN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LAN lobby (src/pc/net_lan.c). mDNS/DNS-SD discovery of other melee-pc
 * instances on the local network, Double Dash style: everyone who opens the
 * LAN menu is announced, the lobby shows how many were found, the first to
 * press Start becomes the host and the match starts on every peer. */

#define PC_LAN_MAX_PEERS 8
#define PC_LAN_NAME_LEN 16

typedef struct PcLanPeer {
    char name[PC_LAN_NAME_LEN]; /* display name (hostname by default) */
    char ip[46];                /* dotted IPv4 or IPv6 text */
    uint16_t port;              /* game UDP port */
    bool host;                  /* elected host */
} PcLanPeer;

/* Start/stop announcing + browsing. Idempotent. */
void pc_lan_start(void);
void pc_lan_stop(void);

/* Poll from the game thread once per frame; services sockets. */
void pc_lan_poll(void);

/* Snapshot of known peers, excluding ourselves. Returns count. */
int pc_lan_peers(PcLanPeer* out, int max);

/* Our own display name, the same one the announce carries (hostname). */
const char* pc_lan_local_name(void);

/* Local player pressed Start: claim host and start a match with the first
 * peer. Returns false if there is no peer yet. */
bool pc_lan_start_match(void);

/* Lobby state for the menu: 0 idle/searching, 1 connecting, 2 in match,
 * 3 failed (message in *why). pc_lan_is_host(): we pressed Start first
 * (valid while the state is 1 or 2). */
int pc_lan_state(const char** why);
bool pc_lan_is_host(void);

/* Valid once pc_lan_state() returns 2: the match seed (host picked it, the
 * guest got it from RULES) and the synced frame at which to enter GS_VS. */
uint32_t pc_lan_seed(void);
int32_t pc_lan_start_frame(void);

/* Session control (src/pc/net.c). Connect at runtime instead of via
 * MELEE_NET; player 0 = P1 (host), 1 = P2. Returns false on socket error.
 * seed != 0 is applied to *HSD_RandSeedPtr at once (host); the guest passes
 * 0 and learns it from RULES. The next sim tick after connecting blocks in
 * the lockstep wait until the peer's first packet (60 s), so announce
 * before connecting. Game thread only. */
bool pc_net_connect(const char* ip, uint16_t port, int player, uint32_t seed);
void pc_net_disconnect(void);

/* Reliable lobby messages over the game socket (stop-and-wait, one in
 * flight). type is caller-defined (>= 0x10); payload <= 256 bytes. */
bool pc_net_send_reliable(uint8_t type, const void* payload, int len);
/* Returns payload length (>= 0) and fills *type when a message is pending,
 * -1 otherwise. */
int pc_net_recv_reliable(uint8_t* type, void* payload, int max);

/* Match handshake used by every lobby: host calls pc_net_host_match() once
 * both sides are connected; it sends RULES (seed, ruleset id) and waits for
 * READY; the guest's pc_net_guest_wait_match() returns true once RULES
 * arrived and READY was sent. Both return the frame at which GS_VS must be
 * entered so the transition happens on the same synced frame.
 * Non-blocking: call once per frame from the game thread; false means not
 * yet, or failed when pc_net_handshake_state() (net.h) is 3 (15 s timeout).
 * start_frame is 120 frames after the host's call; net.c re-applies the
 * seed on both peers entering that frame and compares frame checksums only
 * from it on. The first call with the same seed may be repeated. */
bool pc_net_host_match(uint32_t seed, int32_t* start_frame);
bool pc_net_guest_wait_match(uint32_t* seed, int32_t* start_frame);

#ifdef __cplusplus
}
#endif

#endif
