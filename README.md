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
- Audio: software AX mixer (DSP-ADPCM/PCM voices) on an SDL3 stream.
- Saves: Dolphin-compatible `.gci` in `~/.local/share/melee-pc/USA/Card A/`.
- Not done: 1-P modes, trophies, most menus beyond VS, Windows/macOS.

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
