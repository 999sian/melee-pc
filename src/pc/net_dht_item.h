/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_DHT_ITEM_H
#define PC_NET_DHT_ITEM_H
#include "net_identity.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PC_DHT_ITEM_MAX_VALUE 996 /* 996: plus bytes fits BEP44 encoded limit 1000 */
#define PC_DHT_ITEM_MAX_SALT 64
enum pc_dht_item_status {
    PC_DHT_ITEM_OK,
    PC_DHT_ITEM_NOT_FOUND,
    PC_DHT_ITEM_CONFLICT,
    PC_DHT_ITEM_TIMEOUT,
    PC_DHT_ITEM_CANCELLED
};
typedef struct PcDhtItemResult {
    enum pc_dht_item_status status;
    int64_t sequence;
    uint8_t value[PC_DHT_ITEM_MAX_VALUE];
    size_t value_length;
    unsigned acknowledgements; /* put only; explicit positive DHT responses */
} PcDhtItemResult;
typedef void (*pc_dht_item_callback)(const PcDhtItemResult*, void*);
/* One operation at a time, main thread only. Requires active discovery.
 * Copies all input; callback result lives for the duration of callback.
 * Get ignores valid records below minimum_sequence. Put requires a strictly
 * higher sequence than a found record, or the identical value at the same seq.
 * Salt contributes to the target and signed payload; length zero means absent.
 * These APIs support byte-string values (not arbitrary bencoded containers).
 * Public DHT nodes host values; this client does not serve mutable storage. */
bool pc_dht_item_get(const uint8_t key[32], const void* salt, size_t salt_length,
    int64_t minimum_sequence, pc_dht_item_callback, void*);
/* Same as get, but completes on the first verified record at or above
 * minimum_sequence instead of walking to the closest nodes for the highest
 * one. For rendezvous records where any fresh copy is good enough. */
bool pc_dht_item_get_first(const uint8_t key[32], const void* salt, size_t salt_length,
    int64_t minimum_sequence, pc_dht_item_callback, void*);
bool pc_dht_item_put(const PcNetIdentity*, const void* salt, size_t salt_length, int64_t sequence,
    const void* value, size_t value_length, pc_dht_item_callback, void*);
/* Immutable values use SHA1(the bencoded byte string) as their target.
 * Returned sequence is zero. The hash is verified before exposing a GET;
 * callers remain responsible for application signatures inside the value. */
bool pc_dht_item_immutable_target(const void* value, size_t value_length, uint8_t out[20]);
bool pc_dht_item_get_immutable(const uint8_t target[20], pc_dht_item_callback, void*);
bool pc_dht_item_put_immutable(const void* value, size_t value_length, pc_dht_item_callback, void*);
bool pc_dht_item_busy(void);
void pc_dht_item_cancel(void);
/* Canonical BEP44 signable bytes, for independent tests and interoperability.
 * Destination requires 1200 bytes. Return 0 on invalid inputs. */
size_t pc_dht_item_signable(void* out, const void* salt, size_t salt_length, int64_t sequence,
    const void* value, size_t value_length);
/* Transport hooks owned by net_dht.c; do not call from gameplay. */
void pc_dht_item_tick(void);
bool pc_dht_item_receive(const void* data, size_t length, uint32_t ip, uint16_t port);
#ifdef __cplusplus
}
#endif
#endif
