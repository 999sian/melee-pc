# Crash, rendering and audio investigation — 12 September 2026

Investigated the active `melee-pc` port and preserved the extensive changes
already in the working tree. Four additional defects were reproduced and addressed.

## Fixes

### Normal window close aborted

Closing the window with `WM_DELETE_WINDOW` produced SIGABRT. GDB showed
`std::thread::~thread` calling `std::terminate`, with `arq_worker` still waiting
on its condition variable. `ARQInit` started a worker, but nothing joined it.

Implemented the already-declared `ARQReset`: stop, discard queued requests,
wake the worker and join it. The main shutdown path now stops SDL audio,
closes the DVD worker, resets ARQ, then destroys Aurora's resources. This
ordering keeps audio callbacks and DMA work out of platform teardown.

`tools/test_window_close.py` launches its own no-card session and sends a
normal close event. It failed before the fix and exits with status 0 after it.
The later live match session also closed with status 0.

### Incorrect NBT3 vertex-array sizing

`src/pc/vtxarray.c` treated an indexed NBT3 normal attribute as one index.
GX uses three separate indices. The scanner therefore read subsequent texture
indices at the wrong byte positions and could underestimate the normal array
uploaded to the GPU. Aurora's display-list parser already handles three.

The scanner now includes all three indices in both vertex length and maximum
array index. Synthetic two-vertex display lists failed before the fix and
pass afterwards for INDEX8/INDEX16 and the NRM/NBT attribute names. Checks also
cover truncated batches and ordinary single-index normals. This establishes
the parser defect; no particular retail model's visual artifact was attributed
to it during this session.

### Undefined arithmetic while decoding negative audio samples

PCM8 shifted a negative signed sample left; ADPCM sign extension could overflow
a signed shift and its accumulator also shifted negative values. These are
undefined in C even for valid audio data. UBSan reproduced the PCM8 error with
sample byte `0x80`. Replaced the shifts with subtraction/multiplication that
preserve the intended sample values without undefined arithmetic.

The existing audio regression now includes negative PCM8 and ADPCM samples
and accepts `--sanitize`. Inclusive endpoints, HPS ring transitions and loop
history tests still pass.

### Black floors on stages with projected fighter shadows

Reproduced solid black floor polygons on Temple and Corneria. GPU readback
showed all four generated R4 shadow textures had white backgrounds and gray
silhouettes, including subsequent frames. Their copy-texture bindings resolved
correctly. Bypassing shadow multiplication restored the floor; fixed sampling
coordinates, bounded coordinates, and explicit level-zero sampling each removed
the black areas in separate diagnostic builds.

The final change uses `textureSampleLevel(..., 0.0)` for textures with exactly
one mip level. Textures with multiple levels retain `textureSampleBias` and
normal LOD selection. This preserves the original projected coordinates and
sampler wrapping, rather than disabling fighter shadows or clamping every
projected texture. The failing implicit-LOD path was observed on Intel Arc
(Meteor Lake) with Vulkan; the underlying driver/compiler mechanism has not
been independently isolated.

The final build was checked live on Temple: the large black floor polygons
were gone and localized fighter shadows remained. Diagnostic level-zero builds
were also checked on Fountain of Dreams. Before/after evidence:
`/tmp/melee-floor-before.png` and `/tmp/shadow-final-current.png`.
All temporary GPU readbacks, logging, fixed coordinates, and debugger-only
material/stage overrides were removed from the delivered source.

A separate collision assertion (`ftcoll.c:1292`, attack power 4294967296) was
captured in an earlier diagnostic run. That run included an unverified debugger
register override; it is not counted as a reproduced production defect or a
fixed crash. Its backtrace is retained in `/tmp/melee-shadow-gdb.log`.

## Verification

- `cmake --build build -j 4`: pass.
- `python3 tools/test_audio_stream.py --sanitize`: pass.
- `python3 tools/test_vtxarray.py`: pass with UBSan.
- `python3 tools/test_window_close.py ../melee.ciso` with an X11 display: pass.
- Aurora `gx_fifo_tests`: 203 passed; `render_worker_tests`: 6 passed;
  `os_alloc_tests`: 5 passed. Their build emitted an existing warning about
  `__OSCurrHeap` being initialized and declared `extern`.
- Live seed-17 no-card run with heap canaries and camera diagnostics: opening
  movie and attract matches rendered; resizing to 960×720, minimizing, restoring
  and returning to 1280×960 produced fresh frames. The run exited normally with
  no logged panic, fatal error, assertion or heap-canary report.
- Audio capture: 186.74 seconds, all samples finite, peak 1.0, RMS 0.10565;
  at most 27 of 64 voices occupied. Silent intervals corresponded to zero
  active voices in the census; this does not certify scene-transition timing
  or physical speaker output.

Temporary evidence is in `/tmp/melee-final-play.{log,f32}`,
`/tmp/melee-final-{resized,restored}.png`, `/tmp/melee-investigation.log`
(original shutdown backtrace), and `/tmp/melee-close-before.log`.

This is a targeted investigation and short runtime check, not exhaustive
coverage of every mode, fighter, stage, audio effect or long-running race.
Unconfirmed synth concurrency findings in the earlier audio report remain
outside these verified fixes.

## Internet references consulted

- [Dolphin AX voice implementation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DSPHLE/UCodes/AXVoice.h)
  — sample-rate conversion and envelope behavior.
- [Dolphin normal vertex loader](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VertexLoader_Normal.cpp)
  — `Normal_ReadIndex_Indices3` consumes three indices for NBT3.
- [C++ thread destructor reference](https://en.cppreference.com/w/cpp/thread/thread/~thread.html)
  — destroying a joinable thread calls `std::terminate`.
- [Dawn API definitions](https://github.com/google/dawn/blob/main/src/dawn/dawn.json)
  — surface acquisition statuses, checked while reviewing the existing
  suboptimal-surface fix.

- [Aurora EFB conversion shader](https://github.com/encounter/aurora/blob/main/lib/gfx/tex_copy_conv.cpp)
  — compared the R4 conversion path.
- [Melee shadow implementation](https://github.com/doldecomp/melee/blob/master/src/sysdolphin/baselib/shadow.c)
  — checked the shadow background, copy format, and projection setup.
- [Dolphin texture coordinate generation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VertexShaderGen.cpp)
  — compared projected texture matrices and zero-Q handling.
