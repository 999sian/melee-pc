# Special Smash crash investigation — 2026-09-13

Build base: `c213063`, with the existing local PC port and launcher changes plus the fixes below. This is not an unmodified build of that commit.

## Fixes

- **Stamina:** both recent crash dumps (PIDs 630864 and 632764) end in `Stage_8022519C(-1)` through `gm_801B927C`. Manual stage selection leaves `force_stage_id = -1`, while the actual selected stage is in `vs.start.rules.stkind` (4, Kongo Jungle in these crashes). Reuse `gmVsMelee_ExitSss` to preload the selected stage audio and preserve cancel routing.
- **Camera:** the initial snapshot/card screen crashed in `gmCamera_801A2224` while formatting 2043 free blocks. Fake padded structs assumed 32-bit native pointers. Index native SIS slot 3 directly, then decode its big-endian disc pointer table entry.

Both focused regressions failed before their fixes and passed afterward:

```sh
python3 tools/test_stamina_stage.py
python3 tools/test_camera_digits.py
cmake --build build --target melee -j 6
```

## Live test method

`tools/test_special_smash.py` runs each mode under GDB with heap guards (`MELEE_HEAP_CHECK=1`), using a verified USA Rev 2 CISO and a private copy of the existing memory card. Real boot, scene initialization, character/stage exit handlers and gameplay execute. Debugger writes simulate menu choices: Mario/Fox level-9 CPUs, Kongo Jungle, manual stage selection (`force_stage_id=-1`). Camera's introductory card screen also initializes and runs before proceeding.

Each mode must reach 1800 VS frame callbacks (including countdown), or a verified natural timeout/elimination result, and exit normally. Canceled, retried and terminated matches do not qualify. Screenshots are captured asynchronously while the game runs. This tests entry and short gameplay; it does not exhaust all fighters, stages, controller actions or Camera photo saving.

```sh
DISPLAY=:1 XAUTHORITY=/run/user/1000/xauth_qoruHs \
python3 tools/test_special_smash.py \
  '../iso/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).ciso' \
  --output /tmp/melee-special-fixed
```

Results and per-mode GDB logs: `/tmp/melee-special-fixed/results.json` and the corresponding mode directories. Initial failure evidence remains under `/tmp/melee-special-matrix` and `/tmp/melee-stamina-backtrace.txt`.

### Display fallback

After Stamina and Camera passed on Intel Vulkan/Xwayland, subsequent launches stalled before boot in `SDL_ShowWindow` → `XIfEvent`. Those interrupted launches are failures in their original logs, not passing mode tests. A virtual X server with the Intel driver also failed presentation because Xvfb has no DRI3. Remaining tests use a private Xvfb display and Mesa Lavapipe Vulkan software renderer. No system packages were installed: runtime packages were extracted beneath `/tmp/melee-xvfb`.

The software run uses the same executable. `MELEE_TEST_SOFTWARE=1` permits the CPU adapter via a debugger write to Aurora's initialization config; `MELEE_TEST_ROOT_SHOT=1` captures the private display root. This does not bypass game callbacks. Software results are in `/tmp/melee-special-software/results.json`.

```sh
DISPLAY=:97 LD_LIBRARY_PATH=/tmp/melee-xvfb/usr/lib \
VK_DRIVER_FILES=/tmp/melee-xvfb/usr/share/vulkan/icd.d/lvp_icd.json \
MELEE_TEST_ROOT_SHOT=1 MELEE_TEST_SOFTWARE=1 \
python3 tools/test_special_smash.py \
  '../iso/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).ciso' \
  --output /tmp/melee-special-software
```

## Final results

| Mode | Renderer | Result |
|---|---|---|
| stamina | Intel Vulkan / Xwayland | PASS — natural elimination, 1476 callbacks; returned to CSS |
| camera | Intel Vulkan / Xwayland | PASS — 1800 VS callbacks, normal shutdown |
| super-sudden-death | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |
| giant | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |
| tiny | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |
| invisible | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |
| fixed-camera | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |
| single-button | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |
| lightning | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |
| slow-motion | Lavapipe / Xvfb | PASS — 1800 VS callbacks, normal shutdown |

All ten mode tests passed with heap guards. The original desktop startup stalls remain outside this game-mode fix; eight modes have not been revalidated on the Intel desktop renderer. Camera photo capture/saving and exhaustive character/stage combinations were not tested.
