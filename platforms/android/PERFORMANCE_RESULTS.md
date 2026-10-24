# Android profiling and performance trials

20 October 2026; baseline `253d47a14` plus the uncommitted compatibility,
Performance preset and profiling changes in this workspace.

## Implemented

- Native Perfetto markers separate game work, FIFO/frame completion,
  simulation pacing, frame-slot waits, disc reads, shader compilation,
  render work, surface acquisition, GPU queue submission and presentation.
- Optional `MELEE_GPU_PROFILE=1` exposes the existing asynchronous GPU query
  profiler through diagnostic logs without a Tracy connection. It requests
  TimestampQuery only when available and explicitly enabled. Ordinary Android
  builds leave this instrumentation off.
- Android learns GX pipeline associations for a stage/fighter/costume/slot
  combination. Selection drains the previous FIFO context, prioritizes that
  history within the background queue and uses a 250 ms match-entry budget.
  Active draw requests retain priority; the budget can overrun by one compile.
  Unknown effects still need discovery. Successful-device-build filtering and
  serialized inline compilation remain in place.
- `MELEE_ANDROID_PERF_HINTS=1` trials separate native hint sessions for the
  game thread and render worker. It is off by default. Dynamic symbol lookup
  avoids mandatory API 33 imports; missing symbols/services/sessions and
  reporting failures fall back to normal scheduling. Sessions release on
  pause/inactivity, shutdown or error. Deliberate pacing and queue idle time
  are excluded; waits inside work remain included.

API 31–32's Java bridge was not added: the smaller native pilot runs where
API 33 native symbols are available while keeping minimum API 26.
See [Android's native hint API](https://developer.android.com/ndk/reference/group/a-performance-hint)
and [native tracing API](https://developer.android.com/ndk/reference/group/tracing).

## Tablet observations

Samsung SM-T505, Android 16, Adreno 610 / Qualcomm 512.502.0 driver,
4 KB pages. Same SD-card disc, Link/Mario debug VS, seed 1, 1,200 retraces,
1x rendering, MSAA off, anisotropy 1x, reverb off, archive prewarm on,
Mailbox presentation. Perfetto captured scheduling/frequency/storage and app
markers. The hint comparison used GPU query logging off in both runs and
831 known pipeline configs; both reported 2,783 Dawn hits and zero misses/stores.

The following uses consecutive **simulation work-start timestamps**, excluding
the first 120 game frames. It does not measure displayed-frame intervals or
input-to-display latency. Percentiles use nearest rank, 1,079 intervals per run.

| Hint trial | Mean interval | p95 | p99 | Maximum | Intervals >20 ms |
| --- | --- | --- | --- | --- | --- |
| Off | 16.667 ms | 16.764 ms | 16.887 ms | 18.164 ms | 0 |
| On | 16.667 ms | 16.771 ms | 16.922 ms | 21.774 ms | 1 |

Actual hint sessions were created for both native thread IDs, with no logged
reporting failure. This one short pair establishes integration, not a speedup
or a regression: clocks, temperature, order and background activity were not
controlled. Keep hints opt-in pending repeated scenes and sustained tests.

The settled **hint-off** trace separates the work as follows. CPU-side values
are elapsed wall time, including waits; overlapping threads must not be added
to estimate a frame's duration.

| Marker | Mean | p99 | Maximum |
| --- | --- | --- | --- |
| Game work | 3.541 ms | 4.924 ms | 6.952 ms |
| End frame / FIFO drain | 3.982 ms | 6.314 ms | 6.856 ms |
| Frame begin / slot wait | 0.219 ms | 0.340 ms | 0.495 ms |
| Disc read (22 calls) | 0.041 ms | 0.172 ms | 0.172 ms |
| Surface acquire | 0.224 ms | 0.340 ms | 0.661 ms |
| GPU queue submit (CPU call) | 4.116 ms | 5.096 ms | 9.721 ms |
| Surface present (CPU call) | 0.583 ms | 0.826 ms | 3.512 ms |

No shader compilation appeared in this settled window. Disc reads were short;
this scene does not justify changing storage/preloader behavior. Queue submit
and FIFO completion are larger CPU-side costs to investigate in a heavier
scene if it actually misses deadlines. A submit call's time is not GPU execution.

A separate GPU-instrumented run reported 1,198 pass spans after the initial
FPS report: mean 10.254 ms, p95 13.165 ms, p99 15.680 ms, maximum 21.860 ms.
These are GPU timestamp spans and may include inter-pass gaps. This profiling
feature changed Dawn cache keys: that run logged 2,188 misses/stores and about
85 seconds of cumulative pipeline creation before/around entry. It is not a
valid cold-start or FPS comparison with the hint runs. Do not leave GPU
profiling enabled in normal play.

The first match saved **78 scene associations**. A stopped-process snapshot of
the SQLite database and WAL confirmed all 78 matched `pipeline_device` rows.
Two later launches loaded the same scene key and reported zero selected
pipelines pending because the launcher had already warmed them. This confirms
learning/persistence; it does not establish an extra FPS benefit for a fully
warmed cache. The queue test verifies selection and budgeting when rows remain
pending, without compiling unapproved device configurations.

## Validation and installed build

- Android Release native and Linux builds passed, including rollback-section checks.
- All 62 Melee CTest cases passed; the new tests cover scene priority/budget,
  disabled selection, hint opt-in, unavailable symbols/session, target updates,
  failure fallback, reset and cleanup. Updated tests passed again afterward.
- Render-worker tests: 6 passed. Graphics recording tests: 15 passed.
- CLI smoke checks: 6 passed. `git diff --check` passed.
- Final compatibility APK installed as **Melee Compatibility Test** /
  `dev.melee.game.compat`, preserving the separate original app. Normal manifest:
  no benchmark, GPU-profiling or performance-hint environment overrides.
- APK: `dist/Melee-Android-compat-test.apk`, SHA-256
  `7d5237fbb29a5026284056e342638856deede204c0d9b458456da17104ea6cc3`.
- Final APK passes signature verification and build-tools 36.1's 16 KB
  zip-alignment check; all native LOAD segments have `0x4000` alignment.
  No mandatory `APerformanceHint` dynamic imports appear in the final library.

Raw traces, logs, SQLite snapshot, analysis scripts and JSON summaries are in
`local/android-profile-20261020/` (local ignored artifacts). The trace processor
reported no error stats for the analyzed instrumented captures.

Odin2/Mali, API 26 runtime, 16 KB runtime, audible audio quality, input latency,
power consumption and 20-minute thermal behavior remain untested in this trial.
The [latest Dusklight comparison](DUSKLIGHT_COMPARISON.md) records which fixes
were already present; no wholesale renderer or Android-shell migration was made.

### Android menu/fullscreen follow-up (2026-10-20)

The installed compatibility APK now includes touch access via Menu → Game Settings,
and a persisted Fullscreen / System bars shown option. Verified on the SM-T505:
opening the port menu during gameplay, both system-bar modes, restart persistence,
fullscreen toggle followed immediately by Home and return, and Resume restoring
touch controls. The transition exposed a native-window-in-use Vulkan abort; surface
release now explicitly unconfigures the old swapchain before reconnecting it.
Android/Linux builds passed; all 62 Melee tests passed before the surface fix,
then the Android input and four renderer tests passed after it. The input test also
checks surface teardown order and modern/legacy fullscreen behavior with doubles.
APK signature and 16 KB ZIP alignment checks passed. Current installed APK SHA-256:
`d08c5999fba3808944984d2bd79cd4443bf2e6eaaff893957fdf294a8a680a6c`.
Physical controller hardware, API 26 runtime and other device models remain untested.
