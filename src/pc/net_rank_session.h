/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_RANK_SESSION_H
#define PC_NET_RANK_SESSION_H
#include "net_rank_store.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PC_RANK_PUBLIC_BYTES 52
#define PC_RANK_RULESET_ID 0x52563201u
#define PC_RANK_BEP44_SALT "meleepc-rank-v2"
enum PcRankSessionState {
    PC_RANK_SESSION_OFF,
    PC_RANK_SESSION_WAIT,
    PC_RANK_SESSION_PLAY,
    PC_RANK_SESSION_SIGN,
    PC_RANK_SESSION_SAVED,
    PC_RANK_SESSION_FAILED
};
/* Call after authenticated pairing, before either peer enters CSS. WAIT is
 * not permission to start: both signed hellos and verified public proofs are
 * required. Call poll on the game thread, never during rollback replay. */
bool pc_rank_session_begin(const char* directory, const PcNetIdentity* identity,
    const uint8_t peer_key[32], unsigned local_port, uint32_t seed);
void pc_rank_session_stop(void);
void pc_rank_session_poll(void);
/* Matcher forwards early rank messages while it owns the handshake queue. */
bool pc_rank_session_receive(uint8_t type, const void* payload, int length);
int pc_rank_session_state(const char** reason);
/* Cache current state before matchmaking for BEP44 publication/lookup. This
 * encoding is also used by the signed HELLO. It contains no display name. */
void pc_rank_session_initial_public(uint8_t out[PC_RANK_PUBLIC_BYTES]);
bool pc_rank_session_latest_record(uint8_t out[PC_RANK_RECORD_BYTES]);
bool pc_rank_session_public_state(uint8_t out[PC_RANK_PUBLIC_BYTES]);
/* Only call with a verified BEP44 response for this player's authenticated
 * key, expected salt, and sequence. A confirmed NOT_FOUND may supply only
 * canonical genesis; timeout/cancelled must never be treated as absence. */
/* Bounded ancestry to the latest signed opponent state held locally. */
bool pc_rank_session_chain_target(uint8_t target[20]);
bool pc_rank_session_chain_record(const void* value, size_t size);
bool pc_rank_session_proof(unsigned player, const void* value, size_t size, int64_t sequence);
/* Derived only from recorded game outcomes, safe for synchronized scene routing. */
bool pc_rank_session_set_complete(void);
bool pc_rank_session_active(void);
/* Stage protocol consumes synchronized pad input only. First game random;
 * otherwise previous winner bans two distinct stages, then loser chooses. */
void pc_rank_session_stage_begin(void);
int pc_rank_session_stage_port(void); /* -1 when stage already fixed */
bool pc_rank_session_choose_stage(unsigned player, unsigned stage);
bool pc_rank_session_stage_available(unsigned stage);
unsigned pc_rank_session_stage(void); /* 0 until legal pick is complete */
const char* pc_rank_session_stage_prompt(void);
unsigned pc_rank_session_stocks(void);
unsigned pc_rank_session_seconds(void);
bool pc_rank_session_game(
    unsigned winner, unsigned stocks0, unsigned stocks1, unsigned stage, uint32_t frames);
void pc_rank_session_abort(const char* reason);
/* Available only after durable append. Caller may publish this record/state;
 * SAVED does not imply that a public DHT acknowledged publication. */
const PcNetRankRecord* pc_rank_session_record(void);
#ifdef __cplusplus
}
#endif
#endif
