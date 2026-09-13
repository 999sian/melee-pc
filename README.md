# melee-pc

A PC port of Super Smash Bros. Melee (NTSC-U 1.02), built from the
[doldecomp/melee](https://github.com/doldecomp/melee) decompilation on top of
[aurora](https://github.com/encounter/aurora) (GX/OS/PAD/DVD/CARD/THP
compatibility layer, WebGPU via Dawn, SDL3), in the same spirit as
[dusklight](https://github.com/TwilitRealm/dusklight). Linux only for now.

You need your own disc image. Nothing from the game ships in this repository.

## Status

- Boots: opening movie, memory-card create/load, title, attract-mode demos.
- Main menu, VS Mode, character select and stage select work; a human vs CPU
  match starts and plays.
- Attract demos play as real matches: the CPUs attack, grab, throw, use items
  and take damage, matches reach KOs and the sequence returns to the title.
  Seeds 3, 5, 7 and 11 each ran 10+ minutes under `MELEE_HEAP_CHECK` and
  `MELEE_CAM_BONE` with no crash, assert or heap-canary trip.
- 1-P Classic runs to completion: character select, the stage sequence, the
  matches, Results, and the score written back (a 33-minute session finished a
  run with a total score and returned to character select).
- Training mode works, including its stage picker; Icicle Mountain scrolls
  correctly for 7 minutes.
- Adventure mode plays: the stage-1 flythrough, Mushroom Kingdom, losing the
  run and restarting at stage 1. 38 minutes clean under `MELEE_HEAP_CHECK`.
- Stadium: Target Test (a run scored 3 and the high score persisted),
  Home-Run Contest (run, result banner, back to select) and 10-Man Melee
  (wireframes attack, KO counter, HUD). ~10 minutes each, clean.
- Trophies: the gallery loads and rotates trophy models from their own
  archives (`TyVirus.dat`, `TyStarman.dat`, …) with description text and
  series index. Event Match list renders and navigates.
- Audio: software AX mixer (DSP-ADPCM/PCM voices) on an SDL3 stream, with the
  two aux busses and a Schroeder reverb behind `AXFXReverbHi`/`AXFXReverbStd`.
  Music and sound effects both play; a 3-minute capture peaks at 1.0 (clamped)
  with an RMS around 0.12, and the voice pool sits at 2-6 voices instead of
  saturating.
- Saves: Dolphin-compatible `.gci` in `~/.local/share/melee-pc/USA/Card A/`.
- Against a Dolphin capture of the same attract sequence the frame statistics
  line up (near-white pixel fraction 0-1.4% here vs 0-2.1% there) with no
  untextured-quad signature.
- Not done: All-Star (locked until the roster is unlocked, so unreachable on
  a fresh save), Windows/macOS.

## Recently fixed

The bugs this README used to list as open, and what they actually were:

- **White untextured quads** — eight files called `GXBegin` with no matching
  `GXEnd`. The retail SDK's `GXEnd` is empty, so the decompilation omits it;
  aurora's `GXEnd` is what submits the draw (`fifo::finish_draw`), so those
  batches were swept into whatever draw closed next and rasterised with its
  state. The files were `psdisp.c` (particles), `lbbgflash.c`,
  `ftafterimage.c`, `lbcollision.c`, `gm_1832.c`, `lb_00F9.c`, `toy.c` and
  `hsd_3915.c`. `psdisp.c` also read the particle form/texture stream
  (`u32` primitive counts and `f32` s/t coordinates) host-endian out of
  big-endian disc data.
- **CPU players never attacked** — `ftCo_AttackEntry`, the row type of every
  `Fighter_804D64FC` attack table, is read in place out of `PlCo.dat` but was
  declared as a native struct. Every field came back byte-swapped, so the
  level gate, the period and the four reach floats rejected every candidate.
  Tagging it `DISC_STRUCT` fixed all four consumers.
- **Missing sound effects** — `AXSetVoiceSrc` reassembled the 16.16 sample-rate
  ratio with a `memcpy` over `ratioHi`/`ratioLo`, which on a little-endian host
  yields `(Lo << 16) | Hi`; the synth's nominal 1.0 therefore became a ratio of
  1, i.e. one source sample per 65536 output samples. Every effect was
  inaudible *and* never reached its end address, so voices were never released
  and the 64-voice pool filled with stuck voices — after which no new effect
  could start at all. The AI stream volume was also being applied as a master
  gain, so fading the music faded the effects with it.
- **Crashes during play** — several, all of the same family. `Mtx` (48 bytes)
  passed to `MTXOrtho`/`MTXPerspective` (`Mtx44`, 64 bytes) in `gm_1832.c`,
  `lbvector.c` and `hsd_3A76.c`; 26 "layout struct" overlays that reached
  past their object into a neighbouring static (statics are not adjacent on
  `-no-pie` x86-64); motion-variable union views whose members no longer
  aliased once a pointer inside them grew to 8 bytes; and the stock-icon HUD
  dereferencing a 32-bit disc slot as a host pointer.
- **1-P Classic crashed at every stage it reached** — a 15-agent sweep over
  the whole tree against a catalogue of LP64/endian bug classes. The ones that
  were actually crashing: `gmregclear.c`'s `char pad_88[0x38]` was really
  `HSD_Text* x88[7]` plus `s32 xA4[7]`, so two strides disagreed;
  `TrophyData` was declared native while `Toy_803060BC` walks it in place off
  the disc, so the `id != -1` scan ran off the table; `grpushon.c` read
  `((HSD_GObj*) HSD_GObjGXLinkHead)->next_gx`, which is array element 4 with
  4-byte pointers and element 3 with 8; `grrcruise.c` allocated a 408-byte
  GameCube blob through a union view whose record size had changed;
  `ftparts.c` left `fp->parts[]` entries above `parts_num` holding the
  previous fighter's freed `HSD_JObj*`, which the camera-bone lookup then
  dereferenced; and `ftBossLib_8015BD24` divided by `fp->cpu.level`, which
  PowerPC tolerates and x86-64 turns into `SIGFPE`.
- **`bool` parameters that are really integers** — `_Bool` clamps to 0/1.
  `grAnime_801C8138`'s third argument is an animation index used to subscript
  four disc arrays (Flat Zone passes 2 and 3, Fourside passes 0/4/8), and
  `StageData::on_demo_init` takes a scene id that `grLast` compares against
  26. Both are `s32` now; a `-Wbool-compare -Wint-in-bool-context` sweep over
  all 1167 TUs is what found them and is now clean.
- **Missing struct words** — compiling each TU with `-m32 -DLINT` turns every
  `ASSERT_SIZE` in the reachable headers into a real check against the
  GameCube ABI. Three failed: `gmm_x0_vsmodes` was missing the four alignment
  bytes between `nametags[4]` and `vs_melee`, and `gmm_x0`'s trailer was sized
  from two stale offset comments. (`struct Fighter` still fails by the
  deliberate +4 of `throw_thrower`.)
- **Icicle Mountain launched every fighter to y = 7.26e6** — `grIceMt`'s
  scroll state machine read its target table with `((f32*) ((u8*) param + 4))
  [idx]`, a plain host-endian cast over a `DISC_STRUCT` param block. Decoded
  as big-endian the table is a clean speed ramp (−0.15, −0.1, 0, 0.03 … 0.8);
  read host-endian, element 1 is −4.29e8, and integrating that as a per-frame
  scroll speed put the whole roster far outside `lbVector_WorldToScreen`'s
  range assert. This is what `MELEE_CAM_BONE=1` was added to catch, and its
  report — sane x and z, identical absurd y on every fighter, the fighter's
  own `cur_pos` already wrong — is what pointed at the stage rather than the
  bone matrix.
- **Trophy gallery crashed on entry, then stomped the heap on exit** — two
  bugs in the same GameCube-word-indexing family. `Toy_803087F4` read the
  selected entry through a private `ToyEntryData` overlay whose head was
  `u8 x0[0x8]` — the two retail 4-byte list pointers — so with 8-byte ones
  every field after it read 8 bytes early and the archive handle came back
  truncated (`SIGSEGV` in `HSD_ArchiveGetPublicAddress`). The overlay is
  deleted; all three call sites already pass the real `ToyListEntry`.
  Teardown then did `((void**) Toy_sbss_804D6ED8)[0x14] = NULL`: word 0x14 is
  byte 0x50 (`ToyED8Data::archive`) with 4-byte pointers but byte 160 with
  8-byte ones, exactly one past the 160-byte allocation. Both slots are named
  now.
- **The window froze while the game kept running** — aurora only presented on
  `SurfaceGetCurrentTextureStatus::SuccessOptimal`. `SuccessSuboptimal` still
  returns a usable texture, but it took the failure branch: log "skipping
  present", request a reconfigure that `refresh_surface(false)` no-ops when
  the size has not changed, repeat forever. On a compositor that reports
  suboptimal persistently (Xwayland here) nothing is ever presented again.
  Suboptimal textures are presented now, with a reconfigure requested once on
  the transition rather than every frame (`refresh_surface` synchronises the
  GPU). This one is upstream's, not a vendoring artefact: `encounter/aurora`
  `main` still acquires only on `SuccessOptimal`, so the fix is worth sending
  back.

Two measurement caveats worth knowing before trusting the render knobs below:
aurora processes the GX FIFO on a worker thread, so reading a global at render
time attributes a draw to whatever the game thread did last — attribution has
to travel in the FIFO (`GXInsertDebugMarker`). And a debug marker persists
until the next one, while several draw paths never emit one, so
`MELEE_MOBJ_MARK` tags cannot be read as "this draw came from a textured
material".

## Building

Requirements: Linux x86-64, GCC (the game code relies on
`scalar_storage_order("big-endian")`, which only GCC implements), CMake ≥ 3.25,
Ninja, SDL3, a Vulkan driver. Aurora fetches Dawn/nod prebuilts itself.

```sh
python3 tools/extract_fonts.py <disc sys dir> src/sysdolphin/baselib   # once
cmake -B build -G Ninja
ninja -C build
```

`extract_fonts.py` needs the disc's `sys/main.dol` (extract the ISO with
Dolphin or `nodtool`); it writes the two font atlases the decomp keeps out of
tree.

## Running

```sh
build/melee                              # open the launcher
build/melee <disc.iso|.gcm|.ciso|.rvz>
```

The RmlUi launcher provides disc selection (native file picker or drag and drop),
Play, disc verification, fullscreen/windowed display, VSync, and UI scale.
Navigate with arrows/Tab or a gamepad D-pad/left stick; Enter/A selects, Esc/B
returns from settings or cancels verification. The native file picker uses the
desktop's own controls.

Only **Melee USA revision 2 (NTSC-U 1.02, GALE01)** is supported. Valid command-line
disc paths boot directly; invalid or missing saved images return to the launcher.
Settings and the selected path are stored in `launcher.cfg` in SDL's `melee-pc`
preference directory (normally `$XDG_DATA_HOME/melee-pc`, or
`~/.local/share/melee-pc`). `MELEE_VSYNC` overrides the saved VSync preference.

Verification reads the decoded disc through nod, including compressed images,
and compares SHA-1 against the
[Redump DAT](https://github.com/libretro/libretro-database/blob/master/metadat/redump/Nintendo%20-%20GameCube.dat):
`d4e70c064cc714ba8400a849cf299dbd1aa326fc`, 1,459,978,240 bytes. It supports
progress and cancellation. Verification is not cached across launches; compatible
images can be played unverified, and mismatches are reported explicitly.

Builds now require OpenSSL development headers/libcrypto and enable Aurora's
pinned RmlUi dependency. CMake copies `resources/` beside the executable; keep
that directory with the binary when distributing it. The included Liberation
Sans fonts are covered by `resources/FONT-LICENSE.txt`.

Launcher checks: build and run `launcher_data_test`. Run
`python3 tools/test_launcher.py` on an X11/Xwayland desktop with `DISPLAY` and
`XAUTHORITY` set; optional `--disc /path/to/image` checks the UI-to-game handoff.

Gamepads work through SDL. Keyboard: arrows = stick, IJKL = C-stick,
X = A, Z = B, C = X, V = Y, Q/E = L/R, Tab = Z, Enter = Start, TFGH = D-pad.

Environment knobs:

| Variable | Effect |
|---|---|
| `MELEE_SEED=<n>` | Deterministic RNG for the attract demo (reseeded at demo setup). |
| `MELEE_HEAP_CHECK=1` | 32-byte canaries on every heap allocation, verified each frame/alloc/free; aborts at the first stomp. |
| `MELEE_FPS=1` | Print frame rate once a second. |
| `MELEE_AUDIO_DUMP=<file>` | Also write the mix as raw f32 stereo 32 kHz. |
| `MELEE_WINDOW_TITLE=<t>` | Window title (the save/cache dir stays `melee-pc`). |

Diagnostics. All are off by default and resolve their variable once, so they
cost nothing when unset. They only measure or suppress — none of them fixes
anything.

| Variable | Effect |
|---|---|
| `AURORA_LOG_UNTEX=1` | Report draws that bind no texture, with vertex count and stride. |
| `AURORA_SKIP_UNTEX=1` | Drop every untextured draw. |
| `AURORA_SKIP_UNTEX_VTX=n` | Drop only untextured draws with exactly n vertices. |
| `AURORA_LOG_TEV=1` | For a draw that samples no texture, report what its TEV stages asked for. |
| `MELEE_MOBJ_MARK=1` | Tag draws with whether the material had a texture (see the FIFO-marker caveat above). |
| `MELEE_TEV_TREE=1` | Count compiled TEV stages and how many carry a texture. |
| `MELEE_TEX_ASSIGN=1` | Count tobjs assigned a texmap vs forced to `GX_TEXMAP_NULL`. |
| `MELEE_PS_TEXMISS=1` | Report particles that ask for a texture but resolve none (bank, texGroup, poseNum). |
| `MELEE_SFX_STATS=1` | Sound-effect request/accept/reject counts and `.sem` opcode histogram. |
| `MELEE_AUDIO_STATS=1` | Per-0.5s voice census (used/running/zero-mix/zero-ratio/looping/stuck) plus a one-line dump of any voice still running after ten seconds. |
| `MELEE_AUDIO_ADDR=1` | Report every voice sample address against the ARAM bounds. |
| `MELEE_CPU_TRACE=1` | Per-CPU-player AI census every ~2s: interpreter ticks, commands executed and their opcodes, attack candidates accepted, and one counter per rejection reason. |
| `MELEE_EF_LOG=1` / `MELEE_EF_SKIP=a-b` | Report or suppress effect ids. |

`--no-card` boots without a memory card (the game then never prompts to
save); `--dvd <image>` is an explicit form of the positional disc argument.

## Porting notes

The port keeps disc data big-endian in memory and describes it with
`DISC_STRUCT` / `DISC_PTR` (see `src/pc/disc.h`): every struct that maps
archive contents is byte-swapped on access by GCC, pointers in disc data are
32-bit slots relocated to host addresses, and MEM1 is mapped at `0x80000000`
so those slots always fit. Everything below 4 GB is game-addressable
(`-no-pie`, text at `0x10000000`).

Recurring classes of bugs when bringing up new scenes, all seen so far:

- Runtime structs have 8-byte pointers: any decomp idiom that hard-codes a
  GameCube offset (`(u8*)gp + 0xD8`, `memzero(p, 0x74)`, padded overlay
  structs, `HSD_MemAlloc(0x44)`) must become a real field / `sizeof`.
- Statics are not adjacent: casts of `&some_static` to a bigger struct to reach
  the next static must reference the neighbour by name.
- Bitfields are LSB-first on x86: unions that overlay bitfields with an integer
  view need `DISC_STRUCT` on the union *and* every nested struct.
- Unions of per-stage `Ground` views must agree on PC layout, not just on GC.
- Motion-variable unions (`Fighter::mv`) are the same trap one level down: the
  game writes one view and reads another, so a pointer inside a view shifts
  every member below it out from under the other views. Where no position
  inside the union survives (the throw victim had to outlive the damage view),
  move the field out of the union into `Fighter` and say so.
- `UNK_T` is `void*`, so every unidentified word the decompiler left unnamed is
  8 bytes here. Inside a union view that is a layout change; retype the numeric
  ones `u32`.
- Retail `GXEnd` is empty and the decomp omits it; aurora's `GXEnd` submits the
  draw. Every `GXBegin` needs one.
- `Mtx` is 48 bytes, `Mtx44` is 64: `MTXOrtho`/`MTXPerspective` take `Mtx44`.
- Japanese string literals are Shift-JIS at runtime (the build passes
  `-fexec-charset=CP932`; upstream gets this from `sjiswrap` around mwcc).
- `bool` in a decomp signature usually means "int the decompiler could not
  name". Here it is `_Bool`, so every value above 1 clamps: an animation
  index, a scene id or a count silently becomes 1.
- Walking an object as `void**`/`HSD_Archive**` and indexing by the GameCube
  *word* number (`p[0x14]` for byte 0x50) scales by 8 here, so the write
  lands at byte 160 instead. Index by name. `MELEE_HEAP_CHECK=1` catches the
  ones that fall off the end and now names the allocating call site.

A validated whole-tree warning sweep is the cheapest way to find the next
batch of these. Compile every TU for real (`-Wreturn-type` is not emitted
under `-fsyntax-only`), strip the build's `-Wno-all -Wno-extra`, pass
`-fdiagnostics-color=never`, and add `-Warray-bounds=2 -Wstringop-overflow=2
-Wformat-overflow=2 -Wbool-operation`. Always validate the harness by
re-introducing one known-true positive and checking the count moves by one.

Two more harnesses, both validated the same way:

- `python3 tools/lint_sweep.py` compiles every game TU `-m32 -DLINT`, which
  turns every `ASSERT_SIZE` / `ASSERT_OFFSET` in the reachable headers into a
  real check against the GameCube ABI (they are no-ops on `TARGET_PC`). A
  failure means the *reconstruction* is wrong, not the port. Deliberate
  divergences are listed in the script's `EXPECTED` set; it exits non-zero
  only for new ones.
- For "does this union view still alias on LP64", build one probe TU of the
  real headers twice with the project's own flags — once native, once `-m32`
  — and diff member offsets *and sizes* out of DWARF (`gdb -batch -ex
  'ptype /o T'`). Byte-range intersections, not equal start offsets. This
  replaces hand arithmetic, which gets it wrong often enough to matter.

`python3 tools/compile_check.py <files|dirs>` syntax-checks with the build's
exact flags (fast; use it before a full build).

## Tools

- `tools/run.sh <disc>` – run under gdb, dump all threads on a crash.
- `tools/demo_run.sh <seed> [secs]` – boot, enter the attract demo, report
  survival or the crash frames; `tools/demo_sweep.sh <seeds...>` batches it.
- `tools/devctl.py key|hold|shot` – drive/capture the game window under X11
  (`SDL_VIDEO_DRIVER=x11`, honours `MELEE_WINDOW_TITLE`).
- `tools/run_dbg.sh <disc>` – as above plus a FIFO/PAD state dump, so
  `hub send --keys CTRL_C` (or any interrupt) prints why a frame stalled.

A caveat that cost real time: `import -window` can keep returning the last
composited frame under Xwayland while the game presents normally, which reads
as a frozen game and is not one. `devctl.py shot` now detects two identical
captures and nudges the window to force a fresh one — but when a screenshot
and a backtrace disagree, believe the backtrace.

## Layout

- `src/melee`, `src/sysdolphin` – game code from the decomp (upstream commit in
  `src/UPSTREAM_COMMIT`), modified for the PC data model.
- `src/pc` – platform layer: main, OS/VI/GX glue, keyboard, audio mixer, THP,
  vertex-array sizing.
- `extern/aurora` – vendored aurora with local changes (card, DVD, OSAlloc
  canaries, AR).

## License

Game code is derived from the decompilation and remains the property of its
copyright holders; aurora is MIT. See the upstream projects for details.

## F1 PC enhancements menu

Press **F1** during the game to open the PC settings overlay. Use Tab and
Enter or the mouse; Escape or F1 closes it. Gameplay continues, with game
input blocked while the overlay is open.

- Fullscreen/windowed and VSync apply immediately (the `MELEE_VSYNC`
  environment override takes priority).
- Internal resolution cycles through Auto (window size), 1x, 2x, 3x and 4x.
  This is separate from the launcher's UI scale setting.
- Anti-aliasing (off/4x MSAA) and anisotropic filtering (1x–16x) apply on
  the next launch.
- Master volume, mute and an FPS counter apply immediately.
- Settings share `launcher.cfg` in the port's user-data directory.

This version uses Aurora's renderer and input-blocking APIs. Controller
remapping UI and true widescreen camera/HUD support are not implemented.

Validation: `build/launcher_data_test`, `python3 tools/test_audio_stream.py`,
and `python3 tools/test_launcher.py --disc <disc> --case f1-menu --case f1-restart`.
