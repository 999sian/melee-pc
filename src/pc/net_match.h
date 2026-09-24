/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_MATCH_H
#define PC_NET_MATCH_H
#include "net_identity.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum PcNetMatchMode { PC_MATCH_DIRECT, PC_MATCH_UNRANKED, PC_MATCH_RANKED };
enum PcNetMatchState { PC_MATCH_SEARCH, PC_MATCH_CONNECT, PC_MATCH_READY, PC_MATCH_FAIL };
#define PC_NET_CONTACTS_MAX 16
typedef struct PcNetContact {
    char code[18];       /* NAME#SUFFIX as last seen */
    int64_t last_played; /* unix seconds */
} PcNetContact;
typedef struct PcNetMatchProgress {
    uint64_t elapsed_ms; /* since the search began; 0 once past it */
    bool network_ready;  /* the DHT has enough nodes to search */
    unsigned candidates; /* addresses tried so far */
    bool found;          /* a verified Hello arrived */
    bool calling;        /* direct, with a code: we are the caller */
} PcNetMatchProgress;
/* Opponents this profile has played, newest first; codes are only saved once
 * a match actually connected. */
int pc_net_match_contacts(PcNetContact* out, int max);
bool pc_net_match_clipboard_code(char out[18]);
bool pc_net_match_copy_code(void);
void pc_net_match_progress(PcNetMatchProgress* progress);
bool pc_net_match_publish_rank(void);
void pc_net_match_poll_publication(void);
/* 0 idle, 1 pending, 2 acknowledged, -1 failed (durable save preserved). */
int pc_net_match_publication(const char** reason);
bool pc_net_match_start(enum PcNetMatchMode mode, const char* target_code);
void pc_net_match_stop(void);
void pc_net_match_poll(void);
int pc_net_match_state(const char** why);
bool pc_net_match_is_host(void);
int32_t pc_net_match_start_frame(void);
uint32_t pc_net_match_seed(void);
const char* pc_net_match_local_code(void);
const char* pc_net_match_profile_error(void);
const char* pc_net_match_profile_directory(void);
enum PcNetMatchMode pc_net_match_mode(void);
const char* pc_net_match_opponent_code(void);
const PcNetIdentity* pc_net_match_identity(void);
const uint8_t* pc_net_match_peer_key(void);
#ifdef __cplusplus
}
#endif
#endif
