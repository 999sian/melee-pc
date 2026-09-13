# Stadium completion investigation — 2026-09-13

Base `c213063` with the existing local port/launcher changes and prior Special Smash fixes.

## Home-Run Contest

Idle timeout returned to character select, but a real forward-smash at a bag prepared with 150% damage reproduced the reported failure. Before fixes, the bag jumped far beyond the field on landing and continued moving indefinitely; the 3600-frame test failed to finish. Evidence: `/tmp/melee-stadium-smash/home-run/gdb.log`.

Two decoding bugs were corrected:

- `ftcommon.c`: Sandbag knockback slowdown values in disc-backed attributes were read as native floats. Both accessors now decode `DiscF32` values. The regression uses literal big-endian bytes and checks decay to zero in both directions.
- `grhomerun.c`: a native `int[2]` was cast to a disc-layout `GrJoint` table. This changed intended collision joint 0 / map object 10 into joint 2560 / object 0 on this host. Without the intended collision updates, `mpGetSpeed` interpreted the stretched field as platform movement. A watchpoint captured a jump from x=3069 to x=7228 in `Fighter_procUpdate`. Use a properly typed `GrJoint` initializer.

The slowdown fix alone did not resolve the landing problem. With both fixes, the same real forward-smash produced a settled distance of 1504 internal units and returned to CSS after 855 match callbacks (outcome 9, Home-Run's normal custom termination). Evidence: `/tmp/melee-stadium-fixed-all/home-run/gdb.log`.

```sh
python3 tools/test_sandbag_slowdown.py
python3 tools/test_homerun_collision.py
cmake --build build --target melee -j 6
DISPLAY=:1 XAUTHORITY=/run/user/1000/xauth_qoruHs MELEE_TEST_HIT=1 \
python3 tools/test_stadium.py \
  '../iso/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).ciso' \
  --mode home-run --output /tmp/melee-stadium-home-run
```

## Other Stadium coverage

The GDB harness copies the user's memory card to a private profile, enables heap guards, and runs real boot/CSS/gameplay/exit callbacks. It selects Mario. Completion tests inject boundary conditions after 300 callbacks: all targets cleared; final Multi-Man wave defeated; final second of timed modes; or the fighter below the blast zone for a loss. They require the game's own completion handling and normal process exit. They do not constitute full target-clearing or 3-/15-minute playthroughs.

`MELEE_TEST_SCENARIO=loss` tests loss exits. Endless and Cruel have no victory endpoint and are tested through a loss. No production termination function is called by the harness. An earlier Target test tried a nested GDB inferior call, which stalled the debugger; that harness attempt was discarded and replaced with data-only boundary setup.

The primary completion and initial loss sweeps used Intel Vulkan/Xwayland. During final loss verification, a later desktop launch stalled before boot in `SDL_ShowWindow` → `XIfEvent`. The final sweep therefore uses the same executable on private Xvfb/Lavapipe, with CPU adapter permission set only in the debugger. Loss checks require both the correct loss outcome and a recorded player fall, since outcome 9 is shared by Multi-Man victory and Endless/Cruel loss.

```sh
DISPLAY=:1 XAUTHORITY=/run/user/1000/xauth_qoruHs \
python3 tools/test_stadium.py \
  '../iso/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).ciso' \
  --mode target-test --mode 10-man --mode 100-man \
  --mode 3-minute --mode 15-minute --mode endless --mode cruel \
  --output /tmp/melee-stadium-completion2

# Same list with MELEE_TEST_SCENARIO=loss exercises defeat instead of victory.
# For private display testing, use the Xvfb/Lavapipe setup in
# docs/special-smash-validation.md and set MELEE_TEST_ROOT_SHOT=1,
# MELEE_TEST_SOFTWARE=1, DISPLAY=:97 plus the Vulkan driver/library paths.
```

The first Lavapipe loss rerun reached the correct Target loss, but shutdown waited on LLVM shader optimization; its interrupted run is not counted as a pass. The final confirmed loss sweep uses `GALLIVM_PERF=nopt` to disable that software-driver optimization. Logs: `/tmp/melee-stadium-loss-confirmed`. This environment setting is not applied to the user build.

## Final results

| Event | Completion check | Loss check |
|---|---|---|
| Home-Run | PASS: actual smash, settled positive distance, CSS return | Idle timeout also returned normally during diagnosis |
| target-test | PASS: injected ending boundary, CSS return | PASS: recorded fall, correct outcome, CSS return |
| 10-man | PASS: injected ending boundary, CSS return | PASS: recorded fall, correct outcome, CSS return |
| 100-man | PASS: injected ending boundary, CSS return | PASS: recorded fall, correct outcome, CSS return |
| 3-minute | PASS: injected ending boundary, CSS return | PASS: recorded fall, correct outcome, CSS return |
| 15-minute | PASS: injected ending boundary, CSS return | PASS: recorded fall, correct outcome, CSS return |
| endless | No victory endpoint | PASS: recorded fall, correct outcome, CSS return |
| cruel | No victory endpoint | PASS: recorded fall, correct outcome, CSS return |

All counted runs exited normally with heap guards enabled. Both focused regressions failed before their respective corrections and pass afterward. The executable rebuilt successfully. All completion cases and initial loss cases passed on Intel Vulkan; stricter loss assertions passed on Lavapipe. This is focused finish-path coverage with Mario, not exhaustive character, controller, distance, or full-duration coverage.
