# Browser platform (Emscripten + WebGPU)

Runs the game in a web page from the player's own disc image. The page reads the
image with the File API; nothing is uploaded and no game data is part of the
build. Game logic, Aurora GX, the AX mixer and memory-card support are the same
code as every other platform.

Status: desktop Chrome (tested on Apple Silicon; Chromium on Linux with Intel
Arc graphics as well) holds 60 fps in every scene the tests reach,
including four CPUs on Final Destination. It needs WebGPU; there is no WebGL
fallback. Netplay is compiled in but refused at connect (browsers have no UDP),
and HD texture packs, custom music, the desktop launcher and the updater are
absent.

## Build and run

Prerequisites: Python 3, CMake, Ninja, Git, LLVM 22 with LibTooling
(`LLVM_ROOT`, default `/opt/homebrew/opt/llvm@22`), and a real GCC 12+ on `PATH`
as `gcc-NN` for the lowering oracle.

```sh
python3 tools/browser/setup_sdk.py        # pinned Emscripten into build/browser/emsdk
python3 tools/browser/build.py --jobs 8   # oracle tests, game, Aurora, link, node tests
python3 tools/browser/serve.py            # http://127.0.0.1:5190/
```

`serve.py` exists because the engine uses threads, which browsers only allow on
a cross-origin-isolated page (COOP/COEP headers).

Any `MELEE_*` query parameter becomes an environment variable, so the knobs in
`docs/testing.md` work as they do natively:
`http://127.0.0.1:5190/?MELEE_BOOT_SCENE=vs&MELEE_DEBUG_VS=cpu4`.

## Why the game is compiled differently here

The decomp reads disc structures through
`__attribute__((scalar_storage_order("big-endian")))`, which only GCC
implements. Android, Apple and Windows-ARM64 solve that by routing game C
through a GCC cross-compiler. There is no GCC for WebAssembly, so this platform
lowers the attribute instead:

1. Clang preprocesses each unit with `DISC_STRUCT` defined as an annotation.
2. `tools/browser/disc_lower.cpp` (LibTooling) rewrites every read and write of
   an annotated struct's scalar members, bit-fields included, into explicit
   big-endian loads and stores, and emits plain C.
3. `emcc` compiles the result with the same floating-point flags as
   `melee_game`.

`tools/browser/test_disc_lower.py` is the safety net: each `tests/browser/disc_*.c`
is built with real GCC, and the lowered program must print identical values and
bytes both natively and as wasm. `build.py` runs it before compiling the game.
String literals are converted to CP932 first (`execution_charset.py`), which is
what `-fexec-charset=CP932` does for GCC.

A wasm32 host is also the first 32-bit target, which is what the `UINTPTR_MAX`
branches in `src/pc/disc.h` and the 64-bit casts in `archive.c` are for, and the
first where a call through a mismatched prototype traps instead of working by
accident (`ftLib_800876B4`, `gm_801677E8`, `mnCharSel_802640A0`).

## What differs from native

- `main.c`, `dvd.c` replace `src/pc/main.c` and nod: the part of the DVD API
  this target links (an unimplemented call is a link error) is served from
  the page's `File` through `disc-cache.mjs` (512 KiB blocks, 32 MiB LRU). A
  cache miss suspends the wasm (Asyncify) until the read resolves. Only plain
  GALE01 revision 2 `.iso`/`.gcm` images are accepted for now.
- `pc_stubs.c` replaces the desktop launcher's settings, the libusb GameCube
  adapter and the archive file cache. Everything else in `src/pc` is compiled
  unchanged; when `PC_SOURCES` grows, add the file to `BROWSER_PC_SOURCES`.
- One thread runs the game and submits GPU work (`ProcessingMode::Inline`);
  the browser only exposes a WebGPU device to the realm that created it. DVD,
  ARQ and card completions that native delivers from worker threads are
  delivered cooperatively from `pc_os_run_alarms`.
- The frame boundary paces with `emscripten_sleep` instead of
  `SDL_DelayPrecise`, and always returns to the event loop once per frame. The
  game's own wait loops (`pc_os_wait_alarm`, `pc_os_yield`) yield to the event
  loop too, instead of SDL's delays, which spin or clamp to 4 ms here.
- Staging uploads use one CPU arena and `Queue.WriteBuffer` for the ranges a
  frame used. Mapping native's 87 MiB staging buffers every frame costs an
  allocate, clear and copy of all of it in emdawnwebgpu.
- Pipelines compile asynchronously; a draw whose pipeline is still compiling is
  skipped, as upstream does natively. Cached pipelines are compiled behind the
  loading status before `melee_main` rather than five per frame during play.
  The cache uses `journal_mode=MEMORY`: WAL on Emscripten's in-memory filesystem
  stalled uploads for over a second at a time and never persisted.
- Saves (`/saves`) and the pipeline cache (`/cache`) persist in IndexedDB.

## Host page interface

`index.html` and `shell.mjs` are a complete host in about a hundred lines. A page
embedding the engine sets these on `Module` before loading `melee_browser.js`:
`canvas` (its `width` and `height` are the render size), `print`/`printErr`,
`readDisc(offset, size)` returning a `Uint8Array` or a promise of one, and
optionally `onFrame(frame)`, `onGraphicsPreparation(done, total)` and `onAbort`.
Environment variables go into `Module.ENV` from `preRun`, which is after
Emscripten creates `ENV` and before the static constructor that snapshots it.
Then the page mounts `/saves` and `/cache` and calls `callMain([])`.

## Testing

```sh
python3 tools/browser/serve.py &
MELEE_ISO=/path/to/GALE01.iso PLAYWRIGHT_MODULE=/path/to/node_modules/playwright \
  node tests/browser/shell-e2e.mjs
```

Headed Chrome boots `title` (answering the memory-card prompt and menus by
keyboard), `vs`, `vs-cpu4`, `classic` and `training`, and fails any case below
58.5 fps or above a 33.4 ms p99 frame time. Boot-scene cases must also log the
scene they asked for, so holding 60 fps on the wrong screen cannot pass.

Known limitations: SDL's Emscripten audio backend uses the deprecated
`ScriptProcessorNode`;
Safari and mobile browsers are untested here; the first launch on a machine
compiles shaders for a few seconds behind the loading status.
