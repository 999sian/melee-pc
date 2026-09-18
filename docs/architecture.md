# Architecture

## Layers

```
retail disc (.dat/.usd/opening.bnr, big-endian, verbatim in MEM1)
        │
        ▼
src/melee, src/sysdolphin     game code from doldecomp/melee (C11, GCC,
                              upstream commit in src/UPSTREAM_COMMIT),
                              adapted to the PC data model
        │  GX / OS / PAD / DVD / CARD / AX / THP / VI calls
        ▼
src/pc                        platform layer: main, OS/VI/GX glue, keyboard,
                              1000 Hz input poll, software AX mixer, THP,
                              file cache, texture replacements, widescreen,
                              touch, launcher + F1 overlay (RmlUi), updater
        │
        ▼
extern/aurora (vendored)      GX command processor -> WebGPU (Dawn: Vulkan,
                              D3D12, Metal), Dolphin SDK services (OS, PAD,
                              DVD via nod, CARD, ARAM, MTX, THP), SDL3 window
                              and input, RmlUi, pipeline cache (SQLite)
```

The boundary rule and the per-layer coding conventions are in
[CODING_STYLE.md](../CODING_STYLE.md#1-architectural-overview--the-two-layers).

- `src/melee`, `src/sysdolphin` - game code from the decomp, adapted to the PC
  data model ([porting-notes.md](porting-notes.md)). Divergences from upstream
  are tagged `/* PORT: ... */`.
- `src/pc` - platform layer: `main.c` (entry, logging, crash handler,
  backend selection), `vi.c` (frame boundary and frame pacing), `os.c`
  (interrupt masking, alarms, reset), `gx.c` / `vtxarray.c` (GX glue and
  vertex-array sizing), `keyboard.c`, `input_poll.c`, `touch.c`, `audio.c`
  (software AX mixer), `music_stream.cpp` (custom soundtrack), `thp.c`,
  `discfont.c` (font atlases read from the disc), `region.c` (PAL support),
  `file_cache.cpp` (in-memory archive cache and loose-file overlay),
  `textures.cpp` (Dolphin-format texture packs), `widescreen.c`,
  `launcher.cpp` / `launcher_data.cpp` (RmlUi launcher, F1 overlay,
  `launcher.cfg`), `updater.cpp`, `version.cpp`.
- `extern/aurora` - vendored aurora with local changes; editable in-tree.
  Dawn, SDL3 and nod arrive as prebuilts or are built from source per platform
  ([building.md](building.md)).

## Threads

Melee itself is single-threaded: the game loop runs on the main thread and
`VIWaitForRetrace` (`src/pc/vi.c`) is the frame boundary. Around it:

- aurora's GX render worker drains the FIFO and issues WebGPU work off the main
  thread; the pipeline cache has a compile worker and a SQLite writer thread
  (`extern/aurora/lib/gfx/pipeline_cache.cpp`).
- aurora's DVD layer reads on its own thread with 2-4 preloader threads for
  block decompression of `.ciso`/`.rvz` images.
- `src/pc/input_poll.c` samples keyboard and touch state at 1000 Hz on a
  high-priority SDL thread, independent of the 60 Hz game loop.
- `src/pc/audio.c` mixes in a 5 ms callback; voice parameters are protected by a
  recursive `s_audio_mutex`. Interrupt disables are limited to the synth tick.
- `src/pc/file_cache.cpp` pre-warms core fighter, stage and menu archives on a
  background thread after boot; `updater.cpp` checks and downloads on a worker.

aurora delivers GX draw-done and DVD completion callbacks on worker threads. The
game brackets its shared-state updates with `OSDisableInterrupts` /
`OSRestoreInterrupts`, which `src/pc/os.c` maps onto a recursive mutex to keep
that atomicity.

## Memory map

- MEM1 (24 MB) is mapped at `0x80000000` so a 32-bit disc pointer slot is the
  GameCube address. On Linux and Windows the executable is linked non-PIE with
  text at `0x10000000` (`-no-pie -Wl,-Ttext-segment=0x10000000`, or
  `--image-base 0x10000000 --disable-dynamicbase` for MinGW), so the whole 4 GB
  range is game-addressable and `HSD_ArchiveParse` relocates file offsets to
  absolute host addresses. Windows pre-allocates MEM1 in `OSInit()` before SDL
  and the GPU driver fragment the low 4 GB, and `VirtualAlloc2` keeps it 64 KB
  aligned and under 4 GB.
- arm64 macOS and iOS require PIE and a 4 GB `__PAGEZERO`, so MEM1 is instead
  mapped at an address whose low 32 bits are `0x80000000`
  (`extern/aurora/lib/dolphin/os/OSMemory.cpp`). A MEM1 pointer truncated to
  32 bits is still its GameCube address; `DP()` restores the high half.
- ARAM (16 MB) is a separate host buffer; ARAM "addresses" are offsets into it,
  and anything below `0x01000000` is an ARAM offset (`PC_IS_ARAM_ADDR` in
  `src/pc/disc.h`).
- Disc pointer slots are `DISC_PTR(T)` (a `u32` holding a host address), read
  through `DP(T, slot)` and written through `DP_SET(slot, p)`; every struct that
  maps disc bytes is `DISC_STRUCT` so GCC byte-swaps its scalars on access.

## Aurora

`extern/aurora` is the [encounter/aurora](https://github.com/encounter/aurora)
compatibility layer, vendored with local changes. It provides the Dolphin SDK
surface the decomp calls (GX, OS, PAD, DVD, CARD, AR, MTX, THP, VI, SI) and
implements GX by recording the FIFO into a command processor that compiles TEV
and vertex state into WebGPU pipelines (`extern/aurora/lib/gx`), rendered by
Dawn on Vulkan, D3D12 or Metal. `MELEE_BACKEND` pins the backend
(`src/pc/main.c`). Pipelines are cached in SQLite and seeded from
`tools/initial_pipeline_cache.db.gz` so first runs do not compile every shader
on first use. Disc images are read through nod (raw, `.ciso`, `.rvz`; raw
images are memory-mapped). Windows uses MSVC-built prebuilt Dawn
(`AURORA_DAWN_PROVIDER=package`, `extern/aurora/cmake/AuroraDawnProvider.cmake`).
