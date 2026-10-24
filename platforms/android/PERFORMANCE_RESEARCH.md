# Android performance research

Date: 20 October 2026. Inspected baseline: `253d47a14` plus the current
uncommitted Android compatibility and Performance preset changes.

This report combines repository inspection, official Android/Khronos guidance
and the existing tablet experiments. Recommendations below are proposals,
not implemented or measured gains. This research adds documentation only.

## What the measurements establish

The Samsung SM-T505 / Adreno 610 test used Link/Mario debug VS, seed 1,
1,200 retraces, 1x resolution, MSAA off, 1x anisotropy, reverb off, the same
SD-card image and a warm 506-configuration shader cache.

| Configuration | Mean sampled FPS, two runs | Frames over 20 ms | Frames over 33 ms |
| --- | --- | --- | --- |
| Archive prewarm on, Fifo presentation | 57.15 / 58.02 | 101 / 93 | 18 / 10 |
| Archive prewarm on, Mailbox presentation | 59.54 / 59.96 | 23 / 10 | 5 / 1 |

One run with archive prewarm off and Fifo averaged 35.82 sampled FPS,
with 762 frames over 20 ms. Keep archive prewarm enabled on this tablet.
The existing Android Performance preset now requests VSync off, allowing
Mailbox where supported; other devices need their own comparison.

Evidence: `local/android-performance-20261020/results.json` and adjacent logs
(local, untracked artifacts). These are means of complete approximately
one-second FPS samples, excluding the startup sample and partial tail, not
per-frame percentiles. Runs were short; OS caches and clocks were uncontrolled.
All comparison runs reported 1,830 Dawn cache hits and zero misses/stores.
The results support a presentation/prewarm improvement in this scene; they do
not prove that shader compilation caused these warm-cache slow frames.

Cold pipeline compilation previously averaged about 167 ms per pipeline,
with a 570 ms maximum. Warm-cache compilation averaged about 13.2–13.4 ms;
warming 506 configurations still took about 6.7 seconds. Startup and first-use
stutter remain separate from steady-state FPS.

## Priorities for this repository

| Priority | Proposed work and repository location | Expected value / confidence | Effort and acceptance condition |
| --- | --- | --- | --- |
| 1 | Capture CPU, GPU and presentation timing around `src/pc/vi.c`, `extern/aurora/lib/gfx/render_worker.cpp` and `extern/aurora/lib/webgpu/gpu_prof.cpp`. | High confidence that this identifies which work to optimize; no direct FPS gain promised. | Small/medium profiling task. Correlate slow frames with compile events, I/O, scheduling, GPU work or presentation waits. |
| 2 | Test scene-aware warm-up ordering in `extern/aurora/lib/gfx/pipeline_cache.cpp` and launcher loading. Prioritize previously observed configurations for the selected stage/fighters. | Plausible reduction in entry/first-effect stalls; unknown effects still require compilation. | Medium. First collect scene coverage; keep existing device-built-only safety rule. Accept only if first-use late frames fall without missing graphics or excessive loading time. |
| 3 | Pilot Android performance hint sessions through `src/pc/android_compat.cpp`, `MeleeActivity.java` and native frame/thread lifecycle hooks. | Potential CPU scheduling/power benefit; device-dependent and unmeasured. | Medium. API 31+, optional service/session, correct worker TIDs, actual work timing and lifecycle cleanup. Compare frame tails and sustained power. |
| 4 | Add thermal telemetry and loading/gameplay hints using existing activity/launcher transitions. | Helps explain sustained degradation; hints alone may produce no measurable gain. | Small/medium. Thermal headroom API 30+, Game State API 33+; safe fallback on API 26. Run at least 20 minutes before choosing adaptation. |
| 5 | If GPU-bound, reduce proven redundant attachment traffic/pass breaks in `extern/aurora/lib/gfx/frame_packet.hpp` and renderer paths. | Mobile GPU bandwidth savings are plausible, but no specific redundant operation is established. | Medium/large. GPU capture first; preserve GX effects, snapshots and depth reads; verify visuals across stages and GPU families. |

### Profiling and pacing

Use Perfetto system traces for scheduling, CPU frequency, waits and I/O;
use AGI where the device/driver passes its validation. Do not assume that this
tablet's custom Android 16 build supports AGI. Reuse optional Tracy GPU timestamp
zones when the adapter exposes TimestampQuery; inspect profiling build setup
before enabling them and measure instrumentation overhead.
[Android optimization overview](https://developer.android.com/games/optimize/overview),
[AGI device requirements](https://developer.android.com/agi/supported-devices).

The renderer already selects Fifo/FifoRelaxed with VSync, or
Mailbox/Immediate/Fifo without it. Simulation has its own 60 Hz pacing in
`vi.c`, and `AuroraSurface.java` requests a 60 Hz display mode on API 30+.
More frame-pacing code should follow traces of the remaining waits. Swappy's
Vulkan integration wraps queue presentation and needs swapchain/queue lifecycle
access. Dawn owns those here, so integration belongs at that backend boundary;
adding it to the activity could double-pace frames or miss presentation entirely.
[Android Vulkan frame pacing integration](https://developer.android.com/games/sdk/frame-pacing/vulkan/add-functions).

### Shader warm-up

The repo already persists configuration and Dawn blob caches, warms known
configurations in the launcher and waits for the queue on Play. Android disables
background compilation because of driver crashes and warms only configurations
successfully built on that device. Preserve those constraints. Scene-aware
ordering is an inference from the current design, not an upstream recipe or a
guaranteed fix. Do not ship arbitrary desktop pipeline seeds or restore shader
workers without reproducing and resolving the driver failures.
[Khronos pipeline cache guidance](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html).

### CPU and thermal hints

For an initial hint-session experiment, use Java `PerformanceHintManager`
behind API 31 checks, retaining minimum API 26. Associate actual native worker
TIDs with a measured work cycle; avoid counting deliberate frame-pacing sleep
as useful work. Handle unsupported/null sessions, serialize access, and close
sessions when their work ends. On API 31–33, recreate a session when its TID set
changes: `setThreads` requires API 34. Newer WorkDuration overloads require
API 35 and cannot be used with the current compile SDK 34.
[Manager reference](https://developer.android.com/reference/android/os/PerformanceHintManager),
[Session reference](https://developer.android.com/reference/android/os/PerformanceHintManager.Session).

The activity already requests sustained-performance mode where supported.
Thermal telemetry should tolerate unavailable/NaN headroom and misleading vendor
status, and query headroom no more frequently than once every 10 seconds.
Game State can report loading/menu/gameplay on API 33+; guard with API 33 even
though the guide's sample currently uses an older guard. These are OEM hints,
not guaranteed boosts. Adapt rendering quality if measurements justify it;
preserve the game's 60 Hz simulation and determinism.
[Thermal guidance](https://developer.android.com/games/optimize/adpf/thermal),
[Game State API](https://developer.android.com/games/optimize/adpf/gamemode/gamestate-api).

### Conditional later work

Mobile tile-based GPUs benefit from avoiding unnecessary attachment loads,
stores and render-pass breaks. Inspect captures before changing them. Depth
peeks are already asynchronous and limited to 30 Hz; disabling them can break
effects. Do not lower shader precision globally without graphical validation.
[Android graphics design notes](https://developer.android.com/ndk/guides/graphics/design-notes).

Profile-guided optimization is a later CPU experiment if traces find hot code.
This project mixes NDK Clang renderer code with GCC game code for
`scalar_storage_order`; LLVM profiles cannot simply optimize both compilers.
Use representative workloads, compatible compiler/profile versions and separate
pipelines if pursued. Do not replace the GCC bridge or enable global fast-math.
[NDK PGO guidance](https://developer.android.com/ndk/guides/pgo).

Do not add blanket CPU affinity: Android/ARM pinning is already disabled to let
the scheduler place work. The 1 kHz input thread used little CPU in the short
tablet sample, so reducing input precision is not justified. SQLite cache
storage already uses WAL/NORMAL. Large-RAM archive budgets, render queue depth,
Dawn upgrades and Vulkan pipeline-library extensions need targeted evidence;
none is established as the next bottleneck, and native Vulkan features may not
be exposed through this pinned Dawn build.

## Experiment and release gates

1. Save APK/native hashes, commit/diff, device/OS/GPU driver, display mode,
   settings, cache state, SD source, battery/charging state and thermal readings.
2. Repeat a fixed-seed scene at least three times per condition, alternating
   baseline/candidate order. Test cold and warm shader caches separately, backing
   up caches first. Include scene entry, first effects and sustained gameplay.
3. Collect per-frame durations for median/p95/p99, counts over 20/33 ms,
   startup/warm-up time, CPU/GPU work, presentation waits and audio underruns.
   Existing one-second FPS samples cannot yield accurate per-frame percentiles.
4. Run a 20-minute session for heat/power behavior. Battery temperature and
   thermal status alone do not prove the absence of throttling.
5. Compare an additional Adreno generation and Mali before changing defaults.
   Include an API 26 fallback, lifecycle transitions and visual checks of VS
   stages/effects. Test Odin2 when hardware or fresh reporter evidence exists.
6. Land one experiment at a time only after a repeatable improvement without
   graphical, input, audio or simulation regressions. Keep profiling optional.

No new performance implementation or device test was performed for this report.
Odin2, Mali, long-session behavior and 16 KB runtime remain unverified.

Subsequent work is recorded in [profiling and trial results](PERFORMANCE_RESULTS.md),
including implemented instrumentation, scene history and an opt-in hint pilot.
