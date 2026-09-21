# Netplay implementation verification

Branch: `netplay-finish`, based on `c9d769f9e`. Verified on Linux x86-64,
2026-09-20. This is implementation and test evidence, not a declaration that
all hardware/network acceptance in `netcode-plan.md` has passed.

## Delivered

- Corrected RNG restoration, rumble replay/output reconciliation and
  render-dependent offscreen damage during rollback.
- Shared stage selection and complete two-player scene flow through rematch.
- Persistent signing identity, friend codes, internet Direct/Unranked/Ranked
  pairing, DHT bootstrap/rendezvous and mutable/immutable BEP44 storage.
- Ranked best-of-three rules, stage bans/picks, tiebreaks, deterministic rating
  math, dual-signed records, durable-save confirmation and retriable publication.
- Rejection of known-opponent history rollback/forks, with bounded authenticated
  ancestry lookup before gameplay. Format 2 explicitly rejects old format 1.
- Launcher/F1 Online settings, profile/rating display, opponent HUD, 16-phrase
  noncombat quick chat, late local input polling, XFB timing diagnostics and
  optional just-in-time presentation pacing.
- Snapshot sections for every supported platform: ELF on Linux/Android, PE on
  Windows x86-64/ARM64 and Mach-O on macOS/iOS. All final images verify game
  inclusion and engine/audio exclusion; missing support is a build error.

## Checks completed

| Check | Result |
| --- | --- |
| `cmake --build build -j 8 --target melee unit_tests` | Passed |
| `ctest --test-dir build -L melee --output-on-failure` | 38/38 passed |
| `python3 tools/check_style.py` | 117 files passed |
| Changed game-source syntax checks and `git diff --check` | Passed |
| Delayed local match, 50 ms outgoing delay and 2% loss | Passed; over 12,200 in-match frames per peer, 902/1,155 rollbacks, no desync or lost rollback |
| Full lobby → CSS → SSS → VS → Results → CSS → SSS → VS | Passed; matching transition frames and 16,200-frame budget completed |
| Public DHT bootstrap | Ready in approximately 30 seconds |
| Public mutable BEP44 PUT/GET | Five acknowledgements; authenticated exact 36-byte readback at sequence 1 |
| Public immutable BEP44 PUT/GET | Six acknowledgements; exact 69-byte readback by target |
| `python3 tools/test_pe_snapshot.py --wine` | Both data/BSS layouts restored; pointer relocation and engine/audio exclusion checks passed |
| Real game-object compiler checks | Fighter, controller, rumble, online mode, stage select and audio compile/section correctly through iOS and Windows ARM64 bridges with release flags |
| Platform snapshot suite | 12 passed, native macOS runtime skipped on Linux; includes Apple/Windows ARM64 compiler and CMake checks, universal-object preservation, and Android ARM64/x86-64 restore |
| Linux versus MinGW/Wine format-2 oracle | 4,096 rating updates, 32 signed records and 32 immutable heads byte-identical |

The rating oracle's normalized output is 488,000 bytes; SHA-256:
`0c8818916c2198f9688e153d72f28df18f1aefe641f593e491fb01be71ada489`.

The first two live gameplay rows cover the magnifier/scene correction checkpoint.
The final combined build also passed the full 16,200-frame scene/rematch run
(`/tmp/netplay-finish-scenes-fixed`), including late polling and native chat/HUD.
It recorded 580/101 rollbacks at the final periodic sample, maximum depth 8,
and no desync or lost rollback. The final binary also passed a 10,800-frame
run with 50 ms outgoing delay and 2% injected loss
(`/tmp/netplay-finish-delay-final`): 741/756 rollbacks, maximum depth 8,
no desync or lost rollback. An earlier combined run exposed a stale chat
text pointer at Results; live-list ownership checks and a text-pool lifecycle
regression now cover that failure.

Unit coverage includes malformed/conflicting DHT replies, official signature
vectors, signed pairing and cancellation, proof mismatch/timeout refusal,
durable history corruption, known-history ancestry, withheld save confirmations,
failed publication/restart recovery, quick-chat delivery and rate limits, and
rollback input/scene timing, and chat text-pool resets/address reuse.
Public probes used fresh temporary identities and harmless values with the user's explicit permission.

## Phase sync, per-scene delay and replayable recordings, 2026-09-22

Protocol 7. The acceptance matrix was re-run against this build; 15 of 19
rows are on record and every one passed, 12,000 in-match frames per peer,
with no desync and no lost rollback anywhere.

| Link condition | Rollbacks a/b | Max depth | Note |
| --- | --- | --- | --- |
| clean | 0 / 0 | 0 | |
| loss 5 % | 60 / 0 | 2 | |
| loss 20 % | 77 / 159 | 3 | 33 % of round trips lost |
| delay 100 ms | 1,372 / 1,349 | 8 | full window, counts within 2 % |
| delay 200 ms | 2,417 / 2,377 | 8 | |
| burst | 9 / 7 | 3 | |
| reorder | 374 / 0 | 2 | |
| jitter | 0 / 1,067 | 3 | |
| dup | 0 / 294 | 2 | |
| rx delay 100 ms | 1,336 / 1,356 | 8 | asymmetric path |
| snapshot OOM | 529 / 2,001 | 4 / 6 | falls back to lockstep, finishes |
| disconnect, resume 11 s, resume expiry 30 s | — | — | passed |

The symmetry of the delayed rows is the point: the phase controller now keys
on the difference of the two peers' GGPO frame advantages, which needs no
clock, no round trip and no assumption that the two directions are equally
fast. The estimator it replaces asked what time it was when the peer sent a
packet, and the only handle on that is round trips measured through a socket
the game thread drains once per frame -- a bias in the same direction on both
peers, which survives the trimmed mean. Before: 6 rollbacks on one side
against 724 on the other, on a gentler link than any row above.

In-match frame pacing on a 50 ms link with 20 ms jitter and 1 % loss, per
second: frames over 20 ms fell from 0.31 to 0.03, worst frame from 72.7 ms to
32.6 ms, mean 59.99 fps.

Live PC to Android tablet over Wi-Fi (x86-64 against aarch64): mDNS
discovery, host election, handshake, lobby to CSS to SSS to VS on identical
frames, rollback depth 8, no desync; and the PC survived the tablet being
force-stopped mid-match and re-hosted a new session.

Netplay recordings replay without diverging. A capture with 450 rollbacks
ends each scene where the recording did (509, 993, 1477) with no divergence
in 5,000 frames.

Not covered: the two 60-minute soaks, and `scene flow to SSS`, which cannot
leave the results screen. That row is a harness limitation, not a netplay
one -- the session is healthy for the 12,000 frames it sits there (ping
11 ms, no rollback, no desync, no reliable resends) and the scene never asks
to end, so the hand-off code is never reached. `fn_801791E4`
(gmresultplayer.c) exits results on START only when the match was cancelled,
otherwise on an internal counter; why the drive's START presses do not take
is unresolved.

## Remaining acceptance and limitations

- Two different home NATs and a complete live ranked gameplay set remain
  unverified. The ranked fixtures use real UDP and crypto with an isolated
  DHT responder; public storage tests separately use real internet nodes.
- Full Windows gameplay rollback, further Android device coverage and Apple
  gameplay have not been verified in this batch. All supported builds enable
  rollback; Apple/Windows ARM64 cross-link checks are not runtime match evidence.
  Native macOS restore is now required by the macOS build workflow, but that
  remote workflow has not been run from this session. No Android device was
  attached when checked with ADB.
- No physical button-to-photon comparison or hour-long soak was performed.
  Automatic delay is jitter-aware 1–4 frames, sized for lockstep in menus and
  two frames lower in a fight, with manual 0–4 available.
- Ratings are community ratings. Previously unknown histories remain
  self-attested; collusion/new identities remain possible. Four-character
  friend-code suffixes are compact rendezvous identifiers, not independent
  cryptographic contact verification. Public DHT retention is not guaranteed.
- Native chat/menu visuals need broader resolution/device checks. Text labels
  are used instead of separate texture assets.
