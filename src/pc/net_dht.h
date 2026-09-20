/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_DHT_H
#define PC_NET_DHT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum pc_dht_mode { PC_DHT_DIRECT, PC_DHT_UNRANKED, PC_DHT_RANKED };
struct pc_dht_endpoint {
    uint32_t address;
    uint16_t port;
}; /* network IP, host port */
typedef void (*pc_dht_datagram_fn)(const void*, size_t, const struct pc_dht_endpoint*, void*);
/* Main-thread-only singleton. Port zero requests an ephemeral port. */
bool pc_dht_start(enum pc_dht_mode mode, const char* code, int band, uint16_t port);
void pc_dht_poll(void);
bool pc_dht_next_candidate(struct pc_dht_endpoint* out);
void pc_dht_set_datagram_callback(pc_dht_datagram_fn callback, void* context);
/* Borrow for pairing sendto only: poll owns receive until take_socket.
 * take_socket shuts down DHT and transfers the SAME nonblocking IPv4 socket;
 * caller closes it. Returns -1 if inactive. stop closes only an owned socket. */
intptr_t pc_dht_socket(void);
intptr_t pc_dht_take_socket(void);
void pc_dht_stop(void);
uint16_t pc_dht_port(void);
bool pc_dht_ready(void);
/* Consensus of three distinct responding /24 networks, valid for 10 minutes. */
bool pc_dht_external_endpoint(struct pc_dht_endpoint* out);
void pc_dht_sha1(const void* data, size_t len, unsigned char out[20]);
bool pc_dht_topic(
    enum pc_dht_mode mode, const char* code, int band, int64_t unix_minute, unsigned char out[20]);
#ifdef __cplusplus
}
#endif
#endif
