# Android Build Guide for Melee PC

This directory contains the Android application wrapper for Super Smash Bros. Melee PC.

## Architecture & Requirements

The APK is packaged for **arm64-v8a with a minimum of Android 8.0 (API 26)**.
A compatible Vulkan GPU/driver is also required; OS version alone does not
establish compatibility. See [the compatibility audit and remaining checks](COMPATIBILITY.md).
The native and Java minimum API levels are both 26; the current target/compile
SDK is 34. Building requires Linux, JDK 17, Android SDK platform/build-tools 34,
CMake, Ninja, Rust's `aarch64-linux-android` target, and an AArch64 cross GCC.
Use NDK **26.3.11579264**, matching CI; select it with `ANDROID_NDK_HOME`.

1. **Big-Endian Data Structures**:
   - The decompiled Melee codebase relies on GCC's `__attribute__((scalar_storage_order("big-endian")))` to transparently load and read GameCube big-endian disc structures.
   - Modern Android NDK ships only with Clang (which does not support `scalar_storage_order`).
   - `tools/gcc_launcher.py` compiles decompiled C with `aarch64-linux-gnu-gcc` and NDK headers; the NDK Clang/linker builds the rest. Install `gcc-aarch64-linux-gnu`, or set `GCC_AARCH64_BIN`. This bridge is project-specific, not an NDK-supported GCC toolchain.

2. **Native Libraries**:
   - The build script stages `libmelee.so` and any shared `libpng16.so` in
     `platforms/android/app/src/main/jniLibs/arm64-v8a/`. SDL3 is linked statically.
   - The script applies 16 KB ELF page alignment to shared libraries, including
     dependencies. Android 15+ devices can use 16 KB pages; verify the finished
     APK and test on a 16 KB emulator/device before claiming runtime support.
     See [Android page-size guidance](https://developer.android.com/guide/practices/page-sizes).

3. **Building the APK**:
   ```bash
   tools/build_android.sh
   ```
   This builds the native library, stages assets, and produces a signed
   release APK at `dist/Melee-Android-arm64.apk`.

   Signing key resolution, in order:
   - `MELEE_KEYSTORE_BASE64` (what CI sets from repository secrets), or
   - `MELEE_KEYSTORE_FILE` plus the signing password/alias environment variables, or
   - `platforms/android/melee-release.keystore` plus a
     `platforms/android/release-signing.env` holding
     `MELEE_KEYSTORE_PASSWORD`, `MELEE_KEY_ALIAS`, and `MELEE_KEY_PASSWORD`.

   Both are gitignored. Generate one with:
   ```bash
   keytool -genkeypair -keystore platforms/android/melee-release.keystore \
       -storetype PKCS12 -alias melee -keyalg RSA -keysize 4096 -validity 10000
   ```

4. **Running on Device**:
   ```bash
   adb install -r dist/Melee-Android-arm64.apk
   ```

## Performance settings

Use **Settings > Graphics > Performance preset > Apply** for 1x rendering,
MSAA off, 1x filtering and reverb off. On Android this also turns VSync off
(unless explicitly overridden), allowing Mailbox presentation where supported
while simulation remains paced at 60 Hz. If presentation looks worse on your
device, turn Vertical sync back on. Keep shader and archive caches enabled;
new shaders can still cause first-use stalls. See the measured tablet results
and limitations in [COMPATIBILITY.md](COMPATIBILITY.md).

See [Android performance research](PERFORMANCE_RESEARCH.md) for ranked follow-up
work, official sources and a repeatable profiling plan.
The [Dusklight comparison](DUSKLIGHT_COMPARISON.md) checks its latest GitHub
release/main and pinned Android dependencies against this port.
See [profiling and trial results](PERFORMANCE_RESULTS.md) for measured timings,
the installed build and the limits of the current experiments.

See [laptop and tablet thread profiling](THREAD_PROFILING_RESULTS.md) for the
measured CPU headroom, shader-related stalls and expanded starter inventory.

## First-launch shader preparation

The APK includes a starter inventory of pipeline configurations, not
GPU binaries. On first launch, Android filters this inventory and builds it
locally while the launcher shows the remaining count. Play and direct disc
launches wait for the queue; pressing Play again can skip the remaining wait.
Successful pipelines are marked for future warm-up and Dawn keeps local driver
binaries. Newly encountered effects can still compile during gameplay.

Linux, Windows and macOS packages retain their larger existing inventory and
use the same launcher wait, including command-line disc launches. Their native
backends compile for the local GPU. There is no single GPU binary cache shared
between operating systems. See [startup validation](SHADER_STARTUP_RESULTS.md).

## Developer performance trials

Android shader history learns the stage/fighter/costume combination from actual
matches. At a later match entry it prioritizes already-known configurations
for that combination and spends up to 250 ms warming them (one driver compile
can overrun the budget). It keeps the device-built/starter filter and inline
compiler. Fully warmed launcher caches need no additional match-entry work;
unseen effects still compile on first use. `MELEE_SCENE_PREWARM=0` disables
learning and selection for comparison.

`MELEE_ANDROID_PERF_HINTS=1` enables an experimental native ADPF session on
the game thread and render worker. It is off by default. API 33 symbols are
loaded optionally, preserving minimum API 26; older or unsupported devices
use normal scheduling. Reported time excludes deliberate pacing sleeps and
render queue idle time, but includes waits inside active work/driver calls.
Sessions close on pause/inactivity, worker shutdown or a reporting failure.
This trial does not implement the Java API 31–32 bridge.

To capture native trace markers on a debug installation:

```bash
adb shell perfetto -o /data/misc/perfetto-traces/melee.perfetto-trace \
  -t 30s -b 64mb -a dev.melee.game --background sched freq idle disk gfx view am wm
# Reproduce the scene, then pull after the capture duration.
adb pull /data/misc/perfetto-traces/melee.perfetto-trace
```

Use `dev.melee.game.compat` for the separate compatibility-test app. Markers
identify game work, FIFO drain, frame-slot waits, pacing, shader compilation,
disc reads, render-worker work, surface acquisition, queue submission and
presentation. They record when Android tracing enables this app.

`MELEE_GPU_PROFILE=1` additionally reuses the asynchronous GPU timestamp
profiler and writes `GPU profile: work ... ns, ... passes` to diagnostics.
Unsupported timestamp queries disable GPU profiling. The measured span runs
between GPU pass timestamps and can include gaps between passes; it is not
GPU utilization or input latency. This option changes requested GPU features,
can change cache keys, and adds instrumentation overhead: warm that profiling
configuration separately and keep it off for ordinary FPS comparisons.

### In-game settings and fullscreen

Tap **Menu** at the top of the game, then **Game Settings** to open graphics,
audio and other port settings. Tap **Resume** to return to play. The Menu button
stays available with a physical controller or hidden touch controls; gameplay
touch controls yield input while the settings screen is open.

Under **Graphics → Display mode**, choose **Fullscreen** to hide Android's status
and navigation bars, or **System bars shown** to keep them visible. This choice
is saved with launcher preferences and reapplied after restarting or resuming the
app. Fullscreen preserves the selected game aspect ratio; use Aspect ratio to
change how fights fill the display.
