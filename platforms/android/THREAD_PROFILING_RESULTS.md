# Thread profiling — laptop and Android, 20 October 2026

Baseline: `2ff0fa8de`. The trial found startup-inventory gaps behind the largest
observed Android gameplay stalls. No gameplay-thread parallelization or shader
compiler concurrency change was made. The bundled Android inventory now contains
6,480 successfully device-built GX configurations, adding 197 and preserving all
6,283 prior configurations. This moves preparation for these known configurations
to the launcher; it cannot eliminate compilation for undiscovered effects.

## Workload and preservation

Both devices ran fixed-seed four-CPU debug VS on Pokémon Stadium: Captain Falcon,
Donkey Kong, Fox and Mr. Game & Watch (`MELEE_SEED=1`, fighter base 0). Settings,
GPU drivers, presentation and resolutions differ, so these are bottleneck checks
within each device, not a cross-device FPS benchmark.

The laptop used isolated preferences/saves/caches copied from the earlier test
profile. Android used `dev.melee.game.compat`, retained its data and original
separate application, and temporarily installed a fixture-only manifest. The
normal compatibility APK was restored afterward with no automatic match,
profiling, FPS-log or performance-hint overrides. It imported all 6,480 bundled
configurations with zero skipped rows and passed signature/16 KB ZIP alignment
checks. Exporter/error-scope/prune/coverage tests and shader-family tests passed.

## Laptop: CPU headroom in this scene

Intel Core Ultra 7 155H / Intel Arc, Mesa 26.2.4, Linux, Vulkan/X11,
2x render scale, VSync on, MSAA off. Warm caches.
User-space CPU sampling used `perf`, 99 Hz, DWARF stacks, after a 12-second delay.
The record completed with 3,538 samples and zero lost samples. Linux kernel
profiling was excluded; no kernel profiling permissions/settings were changed.

An independent 19.955-second `/proc` sample measured CPU usage below. These are
percentages of **one logical core**, including each thread's user/system ticks,
not percentages of total machine capacity or percentages of the perf samples.

| Thread | One-core CPU usage |
| --- | ---: |
| Render worker | 15.79% |
| FIFO graphics processor | 12.88% |
| Main game thread | 9.67% |
| Controller polling | 0.75% |
| SDL audio | 0.60% |

The game's i915 render-engine accounting increased by approximately 15.91% of
that interval for its active DRM client. This is per-client engine busy time,
not whole-system GPU utilization or per-frame GPU timestamps. At one separate
observation the main thread was in a pacing sleep and graphics workers were
waiting for work; that single observation does not describe every frame.

The 109 complete one-second FPS samples after the delay averaged 59.986, with
14 reported simulation intervals above 20 ms, none above 33 ms and a maximum
reported interval of 23.8 ms. These are simulation/retrace measurements, not
verified displayed-frame times or input latency. p95/p99 cannot be reconstructed
from these one-second summaries.

The largest individual user-space sample symbols included graphics-buffer
hashing (XXH3 variants: 5.74% and 2.26% of sampled CPU) and index-buffer preparation
(4.97%). Render-worker/driver activity and FIFO processing each accounted for
about a third of sampled user CPU, with the main thread about a quarter. These
are possible later CPU optimization targets, but this workload does not show a
saturated main thread or establish a need for more workers. Ordinary bounded
shutdown logged device-destruction/readback-abort warnings; no mid-run device
loss or process failure was observed.

## Tablet: first-use compilation dominates the largest stalls

Samsung SM-T505, Android 16, Adreno 610, Mailbox, 1x rendering, MSAA off (1 sample),
anisotropy 1, reverb off. Hint sessions and GPU-query profiling were off.
Battery was charging at 38%; reported battery temperature was approximately
30.0–30.5 °C. This does not measure SoC temperature or rule out throttling.

Two 28-second Perfetto captures included CPU scheduling/frequency, storage,
Android graphics events and existing application scopes. The first 120 game
markers were discarded from each capture. The measurements below are intervals
between simulation-work starts, not screen presentations.

| Settled trace | Initial recorded scene | Repeat with learned caches |
| --- | ---: | ---: |
| Intervals | 1,516 | 1,551 |
| Mean | 16.999 ms | 16.667 ms |
| p95 | 18.851 ms | 16.800 ms |
| p99 | 20.979 ms | 17.222 ms |
| Maximum | 152.767 ms | 20.112 ms |
| Above 20 ms | 29 | 2 |
| Above 33 ms | 7 | 0 |
| Shader compilations | 7 | 0 |

The worst interval overlapped a 130.661 ms shader compilation and 136.039 ms
of FIFO completion. Another 130.338 ms interval overlapped a 114.677 ms compile.
Other long intervals likewise overlapped compilation. Settled disc reads were
at most 0.084 ms. Surface acquire/present and CPU-side queue-submit markers did
not account for those largest stalls. These scopes overlap and cannot be added
together to calculate a frame duration; queue-submit duration is not GPU work
time. Direct GPU timestamps were not collected in this trial.

In the first settled trace, main game-work wall time averaged 9.115 ms, p99
13.929 ms. Scheduler accounting placed the render worker and FIFO worker ahead
of the main thread in CPU time. The repeat is consistent with eliminating the
observed first-use compiles through caching. It is one exploratory pair:
archive/OS caches, clocks, temperature, background activity and exact capture
start were not controlled, and no general FPS gain is claimed.

The first trace reported one `systrace_parse_failure`; the repeat reported no
trace error stats. Application frame scopes and compile-overlap evidence were
present. Treat the first trace with that limitation, rather than describing it
as an error-free capture.

## Follow-up

1. Continue recording missing effects/stage transformations and verify the full
   stage/fighter sweep before claiming exhaustive inventory coverage.
2. Measure fresh-install startup against the expanded inventory. Its import and
   warm-up are checked, but a new cold-driver-cache timing was not collected.
3. Profile graphics hashing/index generation on Android before changing it.
   Desktop samples alone do not justify a mobile optimization or driver workers.
4. Repeat longer workloads and collect GPU timestamps/thermal data if steady
   slow frames remain. Preserve Android's serialized pipeline creation.

Raw traces, CPU samples, scripts, logs and summaries remain local under
`local/thread-profile-20261020/`; no game assets or GPU binaries enter the seed.
