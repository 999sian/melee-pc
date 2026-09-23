/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_H
#define PC_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Netplay prototype: rollback lockstep between two instances over UDP
 * (src/pc/net.c, docs/netcode-plan.md §4). Enabled at boot by
 * MELEE_NET=<peer host:port> or at runtime by pc_net_connect()
 * (src/pc/net_lan.h); see net.c for the other knobs. */

/* Wire protocol version; a peer with another one is refused (both sides
 * report PEER_INCOMPATIBLE). Bump on any change to the packet layouts,
 * Rules or the handshake. */
/* Version 6 requires sequenced scene exits and acknowledged LAN election.
 * Version 7 adds the sender's frame advantage to every input packet; the
 * phase controller acts on the difference of the two, so a peer that does
 * not send one cannot be synchronised against.
 * Version 8 appends a truncated keyed-BLAKE2b tag to every datagram, so a
 * peer that does not authenticate what it sends cannot be talked to at all
 * once the session key exists (src/pc/net_wire.c). */
#define PC_NET_PROTO_VERSION 8
void pc_net_init(void);
void pc_net_set_input_delay(int frames);
bool pc_net_active(void);
/* Presentation-only quick chat is available in connected noncombat scenes. */
bool pc_net_chat_available(void);
/* True when the simulation must be reproducible elsewhere: netplay,
 * record, replay or sync test. Guards machine-seeded retail behaviour. */
bool pc_net_deterministic(void);

/* Netplay scene hand-off: true while the scene that asked to end must keep
 * ticking, so both peers leave it on the same frame however long their loads
 * took (src/melee/gm/gmscene.c, docs/netcode-plan.md section 5.2). */
bool pc_net_scene_hold(void);
/* Controller port the local player drives (0 = P1/host, 1 = P2/guest). */
int pc_net_local_player(void);

/* Frame of the tick being simulated (-1 before the first); identical on both
 * peers, so a scene change scheduled for a given frame lands in sync. */
int32_t pc_net_frame(void);
/* Scheduled lobby exit, including a still-pending host handshake. */
int32_t pc_net_start_frame(void);
/* Service transport without advancing simulation (lobby start fence). */
void pc_net_poll(void);
/* Internet rendezvous transfers its already-bound IPv4 socket. Ownership
 * transfers on success only; no new NAT mapping is created. */
bool pc_net_connect_socket(
    intptr_t socket, const char* ip, uint16_t port, int player, uint32_t seed);
/* Offered every IPv4 datagram on the session socket before the session sees
 * it; true consumes it. Runs on net.c's receive thread, and
 * pc_net_set_datagram_handler() returns only once no call into the previous
 * handler is still running, so what one reads must stay fixed while it is
 * installed. */
typedef bool (*PcNetDatagramHandler)(const void*, size_t, uint32_t, uint16_t);
void pc_net_set_datagram_handler(PcNetDatagramHandler handler);
bool pc_net_send_datagram(const void* data, size_t size, uint32_t address, uint16_t port);

/* RNG seed agreed for the session (pc_net_connect / match handshake). */
uint32_t pc_net_seed(void);

/* Match handshake progress: 0 idle, 1 pending, 2 done, 3 failed (15 s). */
int pc_net_handshake_state(void);

/* Match rules in force from the RULES handshake until disconnect: unlock-all
 * is on for both peers, frozen stadium is the host's setting. False when no
 * rules are in force (use the local prefs).
 *
 * While a session is in force the unlock masks in the save data are actually
 * written all-unlocked (net_handshake.c), so this flag now agrees with them
 * instead of overriding them; the direct mask readers agree too. */
bool pc_net_rules(bool* unlock_all, bool* frozen_stadium);

/* Called once per simulation tick before the pad queue head is consumed.
 * Replaces the head sample's four ports with the synced inputs for this
 * frame, predicting the remote one when it has not arrived (stalling only
 * when the remote is more than the rollback window behind). */
void pc_net_sync(void);

/* Called after each tick. Returns true when the tick must be run again
 * (rollback re-simulation or the MELEE_NET_SYNCTEST self-check). */
/* Finish rollback/bookkeeping, but defer fresh advances during scene exit. */
bool pc_net_after_tick(bool scene_ending);

/* Called by the frame boundary (src/pc/vi.c) after the pad alarm ran; the
 * returned ns are added to the next pacing wait. Time-sync corrections are
 * paid here rather than by sleeping inside a tick, and only ever lengthen a
 * frame: the peer that is behind is caught by the one ahead slowing down. */
uint64_t pc_net_pace_adjust_ns(void);

/* True while re-simulating: sound/music/rumble starts must be suppressed. */
bool pc_net_resim(void);
/* Reconcile physical motors after rollback; hardware state is not snapshotted. */
void pc_net_rumble_command(unsigned port, unsigned command);

/* The simulation asks the audio engine two questions whose answers live
 * outside every snapshot and move in real time: "did this sound start, and
 * with what voice id" (AXDriver_8038CFF4) and "is that voice still playing"
 * (AXDriver_8038D9D8 -> HSD_SynthSFXCheck, whose nodes sit in the excluded
 * HSD_Synth heap and whose voice state the audio engine clears when the
 * sample ends). A rollback re-runs a frame one round trip later, so on any
 * link with real latency the second answer differs from the first, the
 * simulation branches differently (src/melee/sfx/crowdsfx.c,
 * src/melee/gr/ground.c) and the frame's checksum diverges from the peer
 * that never re-ran it. So the answers are journalled per frame: the first
 * simulation of a frame asks the engine and records, every later
 * simulation of that frame replays the same answers and touches nothing.
 * MELEE_NET_AUDIO_JOURNAL=off restores the old behaviour.
 *
 * replay: true when this frame already has an answer at this point in its
 * call order, and *out is it. record: stores and returns v. Both are no-ops
 * outside a netplay tick on the game thread. */
bool pc_net_audio_replay(int32_t* out);
int32_t pc_net_audio_record(int32_t v);

/* The second question -- "is that voice still playing" (AXDriver_8038D9D8 ->
 * HSD_SynthSFXCheck) -- cannot be journalled into agreement, because each
 * peer's audio engine runs on its own wall clock and a peer that stalled for
 * a round trip has aged the voice further than the peer that did not. During
 * a session the simulation is therefore told "no, it finished" (true return,
 * *answer = false); outside one the real answer stands. That is a behaviour
 * change: crowd cheers no longer wait for the previous cheer to end and the
 * ground.c:3125 gate reads as "the stage sound is over".
 * MELEE_NET_AUDIO_DEAF=off restores the real answer.
 * pc_net_audio_deaf_note() records what the engine would have said, so the
 * periodic report can show how often that differs. */
bool pc_net_audio_deaf(bool* answer);
void pc_net_audio_deaf_note(bool live);

/* Called whenever the game issues a disc request: a tick that did I/O can
 * never be re-simulated (completions land on worker threads). */
void pc_net_note_io(void);

/* Live netplay numbers for the HUD. False when netplay is not active. */
bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks);

/* Link quality for the HUD/lobby: 0 stable, 1 warning (loss, jitter or
 * deep rollbacks in the last second), 2 stalling (a stall over 500 ms in the
 * last two seconds, or the peer announced it is leaving), 3 reconnecting
 * (the peer has been silent past the stall timeout and the session is being
 * resumed, net.c's MELEE_NET_RECONNECT_MS). A reader that only knows 0-2
 * must treat anything above 2 as at least as bad as 2. */
int pc_net_quality(void);

/* True once this session's checksums have disagreed with the peer's. The
 * simulations have parted and no rollback will bring them back; the match is
 * no longer a match. Reported so the player can see it -- it used to be a
 * log line and nothing else, which left two players finishing a game that
 * only one of them was playing. */
bool pc_net_desync(void);

/* Why the last session ended (kept until the next connect): 0 still up /
 * never broke, 1 the peer left (BYE), 2 timeout, 3 desync, 4 incompatible
 * protocol version, 5 an interruption could not be resumed (the input gap
 * outran the rings, or the peer answered for another session or seed). */
enum {
    PC_NET_PEER_OK,
    PC_NET_PEER_LEFT,
    PC_NET_PEER_TIMEOUT,
    PC_NET_PEER_DESYNC,
    PC_NET_PEER_INCOMPATIBLE,
    PC_NET_PEER_RESUME
};
int pc_net_peer_status(void);
void pc_net_peer_status_clear(void);

#ifdef __cplusplus
}
#endif

#endif
