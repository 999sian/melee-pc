/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_RANK_STORE_H
#define PC_NET_RANK_STORE_H
#include "net_rank.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PcNetRankStore PcNetRankStore;
typedef enum PcNetRankStoreResult {
    PC_RANK_STORE_OK,
    PC_RANK_STORE_INVALID,
    PC_RANK_STORE_IO,
    /* The replacement happened but its durability could not be confirmed.
     * The handle is poisoned; close/reopen before doing anything else. */
    PC_RANK_STORE_UNCERTAIN
} PcNetRankStoreResult;
/* Directory must exist. A damaged or wrong-key history fails closed.
 * Only one writer may open a directory at a time. */
PcNetRankStore* pc_rank_store_open(
    const char* directory, const uint8_t key[32], PcNetRankStoreResult* result);
bool pc_rank_store_current(
    const PcNetRankStore* store, PcNetRating* rating, uint8_t head[32], uint32_t* count);
bool pc_rank_store_latest_record(const PcNetRankStore* store, uint8_t out[PC_RANK_RECORD_BYTES]);
/* Latest opponent state attested by a dual-signed record held locally. */
bool pc_rank_store_peer_state(
    const PcNetRankStore* store, const uint8_t peer_key[32], PcNetRating* rating, uint8_t head[32]);
PcNetRankStoreResult pc_rank_store_append(PcNetRankStore* store, const PcNetRankRecord* record);
void pc_rank_store_close(PcNetRankStore* store);
#ifdef __cplusplus
}
#endif
#endif
