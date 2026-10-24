# Shader startup validation — 20 October 2026

## Behavior

Valid direct-disc launches previously returned before entering the launcher.
They now use its existing Play/wait flow on Android, Linux, macOS and Windows.
Icon launches also compile known pipelines while displaying the launcher.
A second Play press retains the existing skip behavior.

Android previously had no first-install game inventory. The final compressed
starter contains 6,283 GX configurations exported from successfully built rows
on the connected tablet. They include the existing larger recorded inventory
plus tablet-built configurations. The loader checks the current configuration
version and structure size. No simple-TEV restriction or hybrid shader mode is
required. Configuration count does not prove exhaustive gameplay coverage.

Only successful local GX compilation marks a configuration as device-built.
Temporary starter eligibility preserves accepted rows through the Android prune;
arbitrary unbuilt rows are still removed. Android driver compilation stays
serialized. Validation/internal/out-of-memory scopes report a rejected GX
pipeline instead of allowing that error to reach the fatal uncaptured handler.
They cannot recover a native driver crash or a lost GPU device.

## Tablet checks

Samsung SM-T505, Android 16 / LineageOS 23.2, Adreno 610, 4 KB pages.
Test package `dev.melee.game.compat`; original application left installed.
Graphics cache files were moved into a private backup while the test app was
stopped; disc permissions, preferences and saves were retained. This simulates
empty graphics caches rather than reinstalling the whole application.

| Run | Observation |
| --- | --- |
| Initial limited trial, empty graphics caches, direct SD-card URI | 312 starter configurations queued at 06:31:41.385; Play waited at 06:31:42.028; queue finished and game started at 06:32:02.043 (20.015 s wait). Nintendo/HAL opening screen rendered. |
| Initial limited trial, repeat direct URI, local binary cache present | 375 known configurations queued (includes effects encountered after the first startup); Play waited at 06:32:21.717; game started at 06:32:24.839 (3.122 s wait). |

The expanded trial imported 6,001 current configurations from the existing
recorded inventory, rejecting 5,904 obsolete-size/non-GX rows. With the tablet's
existing rows it queued 6,280 configurations at 06:35:50.044. The launcher showed
“Cached shaders ready” by the 06:40 inspection. This was a mixed-cache run,
not a cold-cache timing. Export afterward found 6,283 successfully built GX
rows (new launcher activity contributed additional configurations).

The final APK was checked for signature and 16 KB ZIP alignment and installed
as `dev.melee.game.compat`. It imported all 6,283 bundled configurations with
zero skipped rows. Its repeat startup waited from 06:42:09.419 to
06:42:50.436 (41.017 seconds) before starting the game; this larger inventory
has a substantial startup cost even with local driver binaries. Original
graphics caches remain backed up in the app's
private `files/startup-cache-backup-20261020/`; the active cache now contains the
expanded locally compiled inventory. Saves, settings, the disc and original app
were retained.

No GX pipeline rejection or application fatal signal was logged in these runs.
These are startup times, not gameplay FPS or a full-match graphics validation.
Raw logs/screenshots are retained locally under
`local/android-shader-startup-20261020/`.

## Laptop checks

Intel Meteor Lake / Arc Graphics, Mesa 26.2.4, Vulkan, Linux. Isolated
`XDG_DATA_HOME` preserved the user's usual configuration, caches and saves.

- Empty graphics caches under Wayland: 6,021 current desktop configurations
  queued, launcher waited, debug VS entered on Final Destination, process
  exited normally after the bounded run.
- Repeat run under X11: launcher waited, then a four-CPU Pokémon Stadium match
  rendered Mario, Marth, Mewtwo and Ness. Captured overlay showed 59 FPS at that
  instant; this is not a sustained FPS measurement. The process exited normally.
- The renderer reported 115,149 ms aggregate compile time across eight workers
  on the first run and 16,164 ms on the repeat. These are cumulative worker time,
  not startup wall-clock times. Changing Wayland/X11 may affect cache reuse.
- Logs and match image: `local/laptop-shader-startup-20261020/`.
- Android/Linux native builds and all 65 Melee-labeled regression tests passed.

## Stage and fighter recording

`tools/gen_pipeline_cache.py --run DISC --coverage --secs 150` schedules 203
four-CPU recordings: every one of the 26 playable fighters on each of 29
playable VS stages. Akaneia and Icetop unused slots are excluded. The debug
fixture now accepts a fighter base and permits Final Destination explicitly.
The schedule and production fixture selection have regression coverage.

The full 203-run recording sweep has **not** been executed. Its runtime is over
eight hours at 150 seconds per scene, before startup overhead. It expands
recorded inputs; CPU play cannot guarantee every move, costume, item,
transformation or effect occurs. Those gaps need targeted recording and visual
checks before claiming exhaustive coverage. The new 6,283-row inventory should
not be described as proven coverage of all game content.

## Platform limits

Android release target remains arm64-v8a, API 26+, with a compatible Dawn/Vulkan
adapter. This change cannot support every Android device: 32-bit-only hardware
and devices without the required graphics features remain unsupported. It uses
no GPU model allowlist. Adreno 750/Odin2, Mali, other drivers, 16 KB runtime,
macOS and Windows still require hardware validation. Linux and Android builds
and host regression tests are checked locally; desktop package scripts already
include the desktop inventory.

## Profiling follow-up

The subsequent [laptop/tablet thread trial](THREAD_PROFILING_RESULTS.md) expanded
the inventory from 6,283 to 6,480 configurations after recording additional
four-player Pokémon Stadium effects. Counts above describe the earlier build.
