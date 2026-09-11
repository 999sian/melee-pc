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
  match starts and plays. Ness vs a CPU on one stage survived 23 rounds of
  scripted attacks, grabs, shields and item pickups under `MELEE_HEAP_CHECK`.
- Attract demos: 6 seeds x 100s each with heap canaries, no crash.
- Audio: software AX mixer (DSP-ADPCM/PCM voices) on an SDL3 stream. The
  request path and the mixer are both measurably working (50+ requests all
  accepted; mix peaks at 0.82 with 62% of samples nonzero).
- Saves: Dolphin-compatible `.gci` in `~/.local/share/melee-pc/USA/Card A/`.
- Not done: 1-P modes, trophies, most menus beyond VS, Windows/macOS.

## Known bugs

- **White untextured quads.** Some surfaces draw as flat white. Confirmed a
  port bug, not a decomp one: Dolphin renders the same scene correctly from
  the same disc. The artifact is a 4-vertex draw that binds no texture, with
  a 13-byte vertex stride, and `AURORA_SKIP_UNTEX_VTX=4` removes it while
  leaving the rest of the scene intact. It also appears in demo/movie
  content, not only live matches. The object responsible is **not**
  identified. Ruled out by measurement: fog, stage lighting, projected
  shadows, the effect system, the SObj sprite path, stage materials,
  `TEX_LIGHTMAP_*` gating, texmap assignment, and stage dropping in
  `HSD_TExpCompile`.
- **Crashes during play.** Reported but not reproduced here. The attract
  demos and the one scripted VS pairing above are clean, so the trigger is
  likely elsewhere (other characters, 1-P modes, specific moves). A
  backtrace is the fastest route: `tools/run.sh <disc>` runs under gdb and
  prints every thread's stack on a crash.
- **Missing sound effects.** Reported. No defect demonstrated: the whole
  lower stack checks out (aux routing, synth lookup, sound-machine
  allocation, the `.sem` interpreter, the mixer). Most likely specific
  sounds rather than the pipeline.

Two measurement caveats worth knowing before trusting these knobs: aurora
processes the GX FIFO on a worker thread, so reading a global at render time
attributes a draw to whatever the game thread did last — attribution has to
travel in the FIFO (`GXInsertDebugMarker`). And a debug marker persists
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
build/melee <disc.iso|.gcm|.ciso|.rvz>
```

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

Diagnostics for the open bugs below. All are off by default and resolve
their variable once, so they cost nothing when unset. They only measure or
suppress — none of them fixes anything.

| Variable | Effect |
|---|---|
| `AURORA_LOG_UNTEX=1` | Report draws that bind no texture, with vertex count and stride. |
| `AURORA_SKIP_UNTEX=1` | Drop every untextured draw. |
| `AURORA_SKIP_UNTEX_VTX=n` | Drop only untextured draws with exactly n vertices (`4` removes the white quad). |
| `AURORA_LOG_TEV=1` | For a draw that samples no texture, report what its TEV stages asked for. |
| `MELEE_MOBJ_MARK=1` | Tag draws with whether the material had a texture (see the FIFO-marker caveat below). |
| `MELEE_TEV_TREE=1` | Count compiled TEV stages and how many carry a texture. |
| `MELEE_TEX_ASSIGN=1` | Count tobjs assigned a texmap vs forced to `GX_TEXMAP_NULL`. |
| `MELEE_SFX_STATS=1` | Sound-effect request/accept/reject counts and `.sem` opcode histogram. |
| `MELEE_AUDIO_STATS=1` | Per-0.5s voice census, and master-volume changes. |
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

`python3 tools/compile_check.py <files|dirs>` syntax-checks with the build's
exact flags (fast; use it before a full build).

## Tools

- `tools/run.sh <disc>` – run under gdb, dump all threads on a crash.
- `tools/demo_run.sh <seed> [secs]` – boot, enter the attract demo, report
  survival or the crash frames; `tools/demo_sweep.sh <seeds...>` batches it.
- `tools/devctl.py key|hold|shot` – drive/capture the game window under X11
  (`SDL_VIDEO_DRIVER=x11`, honours `MELEE_WINDOW_TITLE`).

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
