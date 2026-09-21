# Testing

## Unit tests

Launcher settings, version parsing, endian helpers, THP decoding and the updater
have unit tests. On Linux the suite also covers netplay recovery, handshakes,
LAN start fencing, reliable messages, controller remapping and adapter snapshot
publication. The same suite runs in CI:

```sh
ninja -C build unit_tests && ctest --test-dir build -L melee --output-on-failure
```

CI also runs `python3 tools/check_style.py` and `python3 tools/compile_check.py`
on every push (see [CODING_STYLE.md](../CODING_STYLE.md)).

## Netplay implementation checks

The Linux unit target includes rollback/RNG/rumble/magnifier regressions,
ranked rules and durable history, DHT protocol/crypto vectors, quick chat,
controller sampling, pairing and reliable transport. Standalone fixtures use
SDL paths from `build/CMakeCache.txt`; set `MELEE_TEST_BUILD` for another build.
Local UDP tests need loopback socket access.

```sh
MATCH_RANKED=1 python3 tools/test_net_match.py
MATCH_RANKED=1 MATCH_PROOF_TIMEOUT=1 python3 tools/test_net_match.py
MATCH_RANKED=1 MATCH_PROOF_MISMATCH=1 python3 tools/test_net_match.py
MATCH_RANKED=1 MATCH_COMPLETE=1 python3 tools/test_net_match.py
MATCH_RANKED=1 MATCH_COMPLETE=1 MATCH_RECOVER=1 python3 tools/test_net_match.py
python3 tools/test_pe_snapshot.py --wine
```

Ranked fixtures use an isolated responder with real BEP44 parsing, signatures
and loopback UDP. They do not establish two-home-NAT connectivity. PE fixtures
verify Windows section ranges and restores, not a complete Windows match.

For live gameplay (requires your own disc and display):

```sh
SDL_VIDEO_DRIVER=wayland python3 tools/net_test.py --delay 50 --loss 2 --minutes 1
SDL_VIDEO_DRIVER=wayland python3 tools/net_test.py --scenes --minutes 2
```

Use the display backend appropriate to your session. Run live tests sequentially;
competing game instances and builds affect pacing. The scene test requires both
peers to enter CSS, SSS, VS, Results, CSS, SSS and VS at matching frame numbers.
`MELEE_NET_DEBUG=1` adds XFB wait timing and rollback HUD detail.
Physical button-to-photon comparison still requires hardware measurement.

## Smoke tests

`tools/smoke_test.py` is the automated end-to-end pass: a table of cases, each
one a subprocess run with a fixed argv and environment, judged on the exit
status plus required and forbidden regexes over the run's output. It needs only
the standard library.

```sh
python3 tools/smoke_test.py --no-disc     # CLI only: no disc image, no GPU
python3 tools/smoke_test.py               # everything
python3 tools/smoke_test.py --case vs-match
python3 tools/smoke_test.py --list
```

The disc comes from `MELEE_DISC`, defaulting to `../melee.ciso` beside the
checkout. Exit status is 0 only when every case passed. A failing case keeps its
temporary directory and prints the path; a passing one deletes it (each GPU run
leaves a ~75 MB pipeline cache behind).

**Nothing here depends on synthetic input or on pixels.** `devctl.py key` drops
keypresses on this machine and `import -window` can return a stale composited
frame, so neither can decide pass/fail. Scenes are reached with
`MELEE_BOOT_SCENE` and every run is bounded by `MELEE_EXIT_AFTER_FRAMES`, which
is what makes the frame count a fixed number two runs can be compared on.

Each run gets a throwaway `XDG_DATA_HOME`, so `SDL_GetPrefPath` lands in a temp
directory and the real memory card under `$HOME` is never touched. `HOME` itself
is deliberately left alone: overriding it loses `XAUTHORITY` and the x11 video
driver then refuses to start. `MELEE_FILES_DIR` is pointed at a nonexistent
path so the file cache cannot silently prefer a checked-out `iso/extracted_usa`
tree over the disc image.

### The two env knobs the harness needs

- `MELEE_EXIT_AFTER_FRAMES=<n>` — after `n` frames, leave the frame loop through
  the same path a window close takes (`pc_exit_requested` in `src/pc/vi.c`, so
  `atexit(pc_shutdown_once)` still runs), exit 0, and log
  `MELEE_EXIT_AFTER_FRAMES: reached frame <n>, exiting`. This is what makes a
  case bounded: a hang fails on the per-case timeout instead of hanging CI.
- `MELEE_BOOT_SCENE=<title|vs|classic|training>` — boot straight into one scene
  with a hard-coded setup, no menu navigation. It starts the state machine in
  that mode instead of routing through `GM_BOOT`, whose memory-card scene burns
  a mode-dependent and wildly variable number of frames (under 300 ahead of
  `GM_TITLE`, over 1800 ahead of `GM_CLASSIC`). `vs` is `GM_DEBUG_VS`, which is
  already a fixed direct start (`onEnterDebugVs`: Link vs Mario on Final
  Destination). `classic` and `training` open on a character select, so their
  `on_load` hooks seed the pick the way the select screen's exit would and jump
  past it — Mario in Classic, Mario vs a Link CPU on Battlefield in Training.
  While the knob is set, the state machine also logs `boot scene: game mode <n>`
  and `boot scene: state <n> scene <n>` per entry. Both knobs cost one `getenv`
  when unset.

### What each case proves

| case | proves |
| --- | --- |
| `cli-version` | `--version` prints a version and exits 0 |
| `cli-help` | `--help` prints usage and exits 2 |
| `cli-unknown-option` | an unknown flag is named, not ignored |
| `cli-two-discs` | a second disc argument is rejected (both paths exist, so this cannot pass by failing to open the first) |
| `cli-dvd-no-path` | `--dvd` with nothing after it is a usage error |
| `cli-missing-disc` | a nonexistent disc path exits 2 with the path named |
| `launcher-startup` | with no disc the launcher comes up (backend, RmlUi, fonts) and shuts down clean |
| `launcher-close-repeat` | three open/close cycles in one pref dir; a shutdown race shows up on a later pass, not the first |
| `backend-unknown-falls-back` | an unknown `MELEE_BACKEND` lists the valid names and continues on auto instead of failing |
| `title-boot` | the title screen runs 900 frames and exits clean |
| `vs-match` | `GM_DEBUG_VS` reaches `GS_VS` and runs 1500 frames |
| `classic-mode` | Classic gets past its intro into the first fight (`state 1 scene 2`) and runs 1800 frames |
| `training-mode` | Training reaches `GS_TRAINING` and runs 1500 frames |
| `disc-inspection` | the image parsed, the FST walked, and the region-specific archives on it are the NTSC-U ones |
| `memcard-create-and-load` | pass 1 creates the card image in the temp pref dir, pass 2 loads it from there |
| `memcard-no-card` | `--no-card` leaves the game with no save file to open |
| `audio-init` | the AX mixer callback is running against the output clock with voices actually mixing (`MELEE_AUDIO_STATS`) |
| `window-close` | a real `WM_DELETE_WINDOW` exits 0 (runs `tools/test_window_close.py`, which needs X11 and python-xlib) |

Every case additionally fails on `PANIC`, `[FATAL]`, `received signal`, a failed
assertion, or an unexplained `Device lost`. `Device lost ... Device was
destroyed` is aurora's own teardown line and appears on every clean exit, so
only an unexplained loss counts.

Expected frame counts live in the case table (the `frames` column of the output,
and `SCENES` in the script). They were measured, not guessed: each is a count
verified to reach that scene's state. Two runs of the full harness print
identical frame counts.

There is no CLI entry point that verifies a disc, and adding a flag only the
test uses would be backwards: the SHA-1 check is covered by the `launcher_data`
unit test (`tools/test_launcher_data.cpp`). `disc-inspection` asserts on what a
real boot observably does with the image instead.

## Running the game under test

- `tools/run.sh <disc>` runs under gdb and dumps all threads on a crash.
- `tools/demo_run.sh <seed> [secs]` enters the attract demo and reports survival
  or crash frames; `tools/demo_sweep.sh <seeds...>` batches it. `MELEE_SEED=<n>`
  makes the attract demo's RNG deterministic.
- `tools/devctl.py key|hold|shot` drives and captures the game window under X11.
- `tools/run_dbg.sh <disc>` adds a FIFO/PAD state dump on interrupt.
- `tools/gen_pipeline_cache.py` merges this machine's `pipeline_cache.db` into
  the shipped seed `tools/initial_pipeline_cache.db.gz`; `--run <disc>` records
  attract demos first. Play the menus, Classic and the cast by hand before
  merging, since the seed only covers what has been drawn.
- `tools/test_*.py` are targeted regression probes (hitlag, Home-Run collision,
  widescreen maths, vertex arrays, audio streaming, Stadium completion, ...).
  Most compile a sibling `tools/test_*.c` with the build's real flags from
  `build/compile_commands.json` and run it; the gdb-driven ones say what they
  need in their docstring.

`import -window` can keep returning the last composited frame under Xwayland while
the game presents normally, which looks like a freeze and is not one. `devctl.py
shot` detects two identical captures and nudges the window; when a screenshot and
a backtrace disagree, believe the backtrace.

## Harnesses for port bugs

Three harnesses find the next batch of the bug classes listed in
[porting-notes.md](porting-notes.md). Validate any sweep by re-introducing one
known-true positive and checking the count moves by exactly one.

- Whole-tree warning sweep: compile every TU for real (`-Wreturn-type` is not
  emitted under `-fsyntax-only`), strip the build's `-Wno-all -Wno-extra`, pass
  `-fdiagnostics-color=never`, and add `-Warray-bounds=2 -Wstringop-overflow=2
  -Wformat-overflow=2 -Wbool-operation`.
- `python3 tools/lint_sweep.py` compiles every game TU with `-m32 -DLINT`, which
  turns `ASSERT_SIZE` and `ASSERT_OFFSET` into real checks against the GameCube
  ABI. A failure means the struct reconstruction is wrong, not the port.
- `python3 tools/compile_check.py <files|dirs>` syntax-checks with the build's
  exact flags. Fast, so use it before a full build.

For "does this union view still alias on LP64", build one probe TU of the real
headers twice with the project's flags, native and `-m32`, then diff member
offsets and sizes out of DWARF (`gdb -batch -ex 'ptype /o T'`). Compare byte-range
intersections, not start offsets.


## Platform rollback snapshot checks

Run `python3 tools/test_snapshot_platforms.py -v` for cross-link and restore
fixtures. Missing cross compilers are reported as skipped, not passes. On
Linux, the available LLVM tools check macOS Intel, iOS ARM64 and Windows ARM64
objects and actual CMake adapters, including GCC compiler bridges. Android NDK
fixtures link with the production ELF script and execute save/mutate/restore;
ARM64 uses `qemu-aarch64`, x86-64 executes directly. Set `ANDROID_NDK_HOME`,
`GCC_AARCH64_BIN`, `GCC_X86_64_BIN`, and `LLVM_MINGW` for nondefault toolchains.

On macOS, run `python3 tools/test_snapshot_platforms.py
SnapshotPlatforms.test_native_mac_restore` on one line. It compiles game
fixtures through Homebrew GCC, links with native Apple tools, and executes
snapshot restoration; both macOS CI architectures run it before packaging.
Windows x86-64 additionally uses `python3 tools/test_pe_snapshot.py --wine`.
These isolated fixtures do not replace full rollback gameplay on each device.

The platform snapshot compiler wrappers share the audio/worker exclusion list
in `src/pc/melee_state.ld`. Changes to that ownership list affect every format.
Mach-O and ARM64 COFF relabeling must preserve all relocation/symbol ordinals
and zero-fill characteristics. Unknown writable sections and common symbols
fail the build rather than escaping snapshots. Verification runs before
packaging strips symbols.
