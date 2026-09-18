# Debugging

## Log files

Everything goes through `pc_log_line()` (`src/pc/pc.h`), timestamped in
milliseconds from startup. The log goes to `stderr` and, when `MELEE_LOG_FILE`
names a path, to that file too; output is flushed per line and both streams are
flushed before a fatal `abort()`, so the file survives a crash. An empty value
disables the file sink. On Windows it defaults to `melee-pc.log` beside
`melee.exe`, because a double-clicked console app's window vanishes on exit.
`MELEE_DEBUG=1` raises aurora's log level to debug. Any frame over 50 ms leaves
a marker in the log (`src/pc/vi.c`). Backend selection logs one line per
skipped backend with Dawn's reason (`No usable D3D12 adapter (...); trying the
next backend`) and then a summary such as
`graphics backend: d3d11 (auto), adapter: Intel(R) HD Graphics 4400 [8086:0a16], driver: ...`;
that line is the first thing to read in a GPU report. Android logs to logcat
under the tag `Aurora`; iOS uses `os_log`.

The Windows zip ships `RUN-AND-LOG.bat`: it sets `MELEE_DEBUG`, `MELEE_LOG_FILE`
and `MELEE_FPS`, records the OS version and GPU driver into `melee-pc-env.log`,
boots a `melee.ciso` sitting beside the exe if there is one, and keeps the
window open. Ask for both `.log` files when triaging a Windows report.

## Crash handler (Windows)

A hard access violation never reaches the logger, so on Windows
`SetUnhandledExceptionFilter` (`src/pc/main.c`) writes the exception code, the
faulting address and a backtrace as `module+RVA` per frame to `stderr` and the
log file. The module name separates a fault inside `webgpu_dawn.dll` from one in
`melee.exe`; the RVA feeds `addr2line` against the matching build.

## gdb

`tools/run.sh <disc>` runs the game under gdb and prints every thread's
backtrace on a crash or SIGINT; `tools/run_dbg.sh` adds a FIFO/PAD state dump.
A backtrace is ground truth; a screenshot under Xwayland may be stale (see
[testing.md](testing.md)). For fields of a `DISC_STRUCT`, print a local the
program computed rather than trusting gdb's view of `DW_AT_endianity`.

## Heap check

`MELEE_HEAP_CHECK=1` appends a canary to every `OSAlloc` allocation
(`extern/aurora/lib/dolphin/os/OSAlloc.cpp`), checks them each frame, and aborts
at the first stomp with the allocating call site (resolve with
`addr2line -f -C -e build/melee <addr>`). This is the tool for "walked an object
by GameCube word number" and other off-by-pointer-width writes.

## Diagnostic environment variables

These are off by default and cost nothing when unset. They measure or suppress
only; none of them fixes anything. User-facing knobs (`MELEE_BACKEND`,
`MELEE_VSYNC`, ...) are in the [README](../README.md#environment-variables).

| Variable | Effect |
|---|---|
| `MELEE_DEBUG=1` | Debug-level aurora logging. |
| `MELEE_LOG_FILE=<path>` | Log file sink (default `melee-pc.log` on Windows; empty disables). |
| `MELEE_FPS=1` | Print frame rate once a second. |
| `MELEE_HEAP_CHECK=1` | Canaries on every heap allocation, checked each frame; aborts at the first stomp. |
| `MELEE_SEED=<n>` | Deterministic RNG for the attract demo. |
| `MELEE_AUDIO_DUMP=<file>` | Also write the mix as raw f32 stereo 32 kHz. |
| `MELEE_SFX_STATS=1` | Sound-effect request/accept/reject counts. |
| `MELEE_AUDIO_STATS=1` | Per-0.5s voice census. |
| `MELEE_AUDIO_ADDR=1` | Report voice sample addresses against the ARAM bounds. |
| `MELEE_GFX_STATS=1` | Peak per-pool high-water marks (vertex/uniform/index/storage/texture) for sizing frame budgets. |
| `MELEE_CPU_TRACE=1` | Per-CPU-player AI census every ~2s. |
| `MELEE_PIPELINE_SYNC=1` | Block the frame on a never-before-seen GX pipeline (old behaviour, the #46 stutter) instead of skipping its draw until it compiles. |
| `MELEE_SCENE_LOG=1` | Report scene model descriptor slots (the joint pointer `gmRegClearAddModel` panics on). |
| `MELEE_ARCHIVE_LOG=1` | Report every archive load with its header sizes. |
| `MELEE_INTRO_LOG=1`, `MELEE_ICON_LOG=1`, `MELEE_TEXANIM_LOG=1` | Team-splash capture buffers; CSS icon frame selection; out-of-range texture-animation frames. |
| `MELEE_MOBJ_MARK=1` | Tag draws with whether the material had a texture. |
| `MELEE_TEV_TREE=1` | Count compiled TEV stages and how many carry a texture. |
| `MELEE_TEX_ASSIGN=1` | Count tobjs assigned a texmap vs forced to null. |
| `MELEE_PS_TEXMISS=1` | Report particles that ask for a texture but resolve none. |
| `MELEE_EF_LOG=1`, `MELEE_EF_SKIP=a-b`, `MELEE_EF_MAT=<id>` | Report or suppress effect ids; dump the material of one effect. |
| `MELEE_ZTEX_BIAS=<n>` | Z-texture bias for the sobj tile pass (0 = retail; separates an empty Z capture from a missing draw). |
| `MELEE_CAM_BONE=1` | Report camera-bone inputs where the value is produced, so the first bad frame is logged. |
| `MELEE_INSTANT_WIN=1` | End a VS match after 60 frames (test fixture). |
| `MELEE_CLASSIC_STAGE_OVERRIDE=<1-11>`, `MELEE_CLASSIC_TEAM=kirby\|jiggly` | Force a Classic stage / team fight (test fixture). |
| `AURORA_LOG_UNTEX=1` | Report draws that bind no texture. |
| `AURORA_SKIP_UNTEX=1` | Drop every untextured draw. |
| `AURORA_SKIP_UNTEX_VTX=n`, `AURORA_ONLY_UNTEX_VTX=n` | Drop, or keep only, untextured draws with exactly n vertices. |
| `AURORA_LOG_TEV=1`, `AURORA_LOG_UNTEX_REGS=1`, `AURORA_LOG_QUADPOS=1` | Report what an untextured draw's TEV stages / registers / quad positions asked for. |
| `AURORA_LEGACY_TEX_LOD=1` | Restore the unconditional LOD-bias sample (positive control for the black-floor sweep). |
| `AURORA_NO_PIN=1`, `AURORA_PIN_THREADS=0` | Disable pinning aurora's worker threads to cores. |

`grep -rho 'getenv("\(MELEE\|AURORA\)_[A-Z0-9_]*")' src extern/aurora/lib` is the
authoritative list; the table lags it.
