# Pixel 8 Pro device checks

20 October 2026, commit `38d31422a`. Normal APK SHA-256:
`d08c5999fba3808944984d2bd79cd4443bf2e6eaaff893957fdf294a8a680a6c`.

## Device and method

- Pixel 8 Pro, Android 17, 4 KB pages, Mali-G715 Vulkan driver 54.3.0
  (`v1.r54p3-00eac0.d6609729a0c2466c24fd0530143c8edf`).
- Battery initially 87%, charging, 27.0 C; afterward 88%, 31.1 C.
  Reported thermal status remained 0 at the sampled checks.
- Installed the separate `dev.melee.game.compat` app; retained the original app.
  Android rejected direct filesystem access to the Downloads image and to a
  shell-created external copy. A copy in the test app's private `files/`
  directory worked; the Downloads original remains untouched.
- Temporary benchmark manifest: VS CPU match, seed 1, automatic exit at 1,200
  frames, archive prewarm enabled, GPU timestamp profiling and ADPF disabled.
  Settings: 1x, MSAA off, anisotropy 1x, VSync off, reverb off, fullscreen.
  Overrode an inherited `MELEE_NET_DEBUG` option to 0 for the benchmark.
- First VS run followed ordinary boot/intro checks, so this was not an entirely
  empty cache. The repeat retained its learned caches. No cache was deleted.

## Measured simulation intervals

Perfetto ring-buffer captures retained different portions of the two runs.
These figures describe the retained windows, not all 1,200 frames or a rigorous
paired full-run comparison. The raw trace processor reported no error stats.

| Retained window | First VS | Repeat with warmed cache |
| --- | ---: | ---: |
| Simulation intervals | 871 | 711 |
| Mean | 17.361 ms | 16.667 ms |
| p95 | 17.904 ms | 17.094 ms |
| p99 | 45.189 ms | 17.890 ms |
| Maximum | 117.235 ms | 27.812 ms |
| Intervals over 33 ms | 12 | 0 |

The two longest first-run intervals were 117.235 and 107.212 ms. Shader
compilation overlapped 110.983 and 100.286 ms respectively; no disc read
overlapped either interval. FIFO completion occupied 109.475 and 97.038 ms.
This identifies shader creation as a concrete source of the captured stalls.
Other intervals overlapped background archive reads as well as compilation;
overlap alone does not establish a storage bottleneck.

The first VS run's final log reported 100 pipelines created in 4,246 ms total,
278 Dawn misses and 278 stores. The repeat reported 95 pipelines created in
413 ms total, 426 Dawn hits and zero misses/stores. Pipeline counts differ;
these totals are supporting cache evidence, not a controlled speedup ratio.
The repeat's longest captured compile was 16.265 ms and overlapped its longest
27.812 ms simulation interval.

First-entry logs also recorded a 7,172.4 ms interval before sustained play;
the retained trace cannot decompose that entire startup interval. Archive
prewarm cached 894 archives / 286 MB over 24,305 ms in the first VS run.

These are simulation-start intervals, not displayed FPS. FrameTimeline data
covered the activity decor layer and did not establish the presentation timing
of the game's SurfaceView. GPU timestamp profiling was intentionally off.

## Interaction checks and final state

- Launcher and game boot rendered. Both timed benchmark processes exited via
  the requested frame limit; no native fatal signal was identified in their
  captured logs. Device-destroyed messages occurred during normal teardown.
- Touch Menu opened the Android dialog; Game Settings opened the in-game menu.
  Retesting confirmed touch navigation to Audio, a master-volume drag saved
  `volume 0.55`, and touch display-mode switching saved `fullscreen 1`.
  Earlier unsuccessful tap attempts are not counted as passed checks.
- Keyboard navigation also switched Fullscreen to System bars shown. Home and
  return retained PID 30335 without a captured Vulkan surface error.
- Injected Android volume-key events did not produce a verified media-volume
  change. Physical volume buttons and audible audio quality remain unchecked.
- Restored the normal APK, removing benchmark manifest overrides. Relaunched
  it with its private disc copy selected, fullscreen enabled and 1x
  resolution. Restored master/music/SFX volume preferences to 1. Existing
  original-app environment files were not edited.

Local ignored evidence: `local/pixel8pro-20261020/` contains traces, PID files,
logs, analysis scripts, JSON summaries and interval-overlap results. Larger
buffers or narrower trace sources are needed for complete startup captures.

This short test does not cover every fighter/stage, multiplayer, long-session
thermal behavior, ADPF effectiveness, GPU execution cost, physical controls,
or a 16 KB runtime. It supports further work on shader readiness and warm-up;
it does not establish that an ubershader will improve this GPU's performance.
