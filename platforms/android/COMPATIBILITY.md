# Android compatibility audit and fixes

Date: 20 October 2026. Baseline: `release/0.4-beta`, commit `253d47a14`.

## Scope and evidence

This audit compares the repository's Android build, native runtime, renderer,
Java activity and input paths with the official sources linked below. It uses
code inspection, targeted regression tests, compiler probes and local builds.
AI-assisted research tools were used. Device-specific explanations are marked
as hypotheses; a successful desktop build does not establish Android support.

The supplied Odin2 report identifies Android 13/API 33, Adreno 740 and the
Qualcomm 512.676.0 Vulkan driver. The reporter describes VS graphical glitches
and occasional frame drops. Four October 16 SIGABRT lines lack a reason or
backtrace; they do not establish a crash during the October 19 sessions.

In the first recorded session, 775 pipelines were created in 15,216 ms total
(19.6 ms average, 113.1 ms maximum); 241 were requested by draws before ready.
These are cumulative pipeline metrics, not measured frame times. They show
compilation work large enough to exceed a 16.67 ms frame budget when performed
inline. They do not identify the particular visual artifacts or prove every
frame drop has this cause. The subsequent launch queued 512 cached configs.

## Implemented changes

| Area | Confirmed defect and correction | Evidence/check |
| --- | --- | --- |
| Native Android ABI | The custom GNU/Linux GCC bridge did not reserve Android's `x18` register. Added `-ffixed-x18` for AArch64 only. | Official Android ABI rule [1]; a register-pressure compiler probe used `x18` before the flag and stopped using it afterward. Wrapper regression checks emitted flags and unchanged x86/Clang forwarding. |
| CPU baseline | Android flags unconditionally enabled CRC/crypto instructions beyond baseline ARMv8-A, including the native target. Use baseline ARMv8-A in the GCC bridge and let NDK Clang select its ABI defaults. | ABI requirements [1]; inspect `tools/gcc_launcher.py` and `CMakeLists.txt`. No claim that unsupported instructions caused this Odin2 report. |
| Cached graphics pipelines | A pending normal-priority draw on a platform without compiler workers only promoted its pipeline, even when the current frame had compilation budget available. It could miss the draw unnecessarily. Compile it using the same budget as a new pipeline. | Production scheduling harness fails before the fix and passes afterward; checks budget exhaustion, cache reuse, blocking requests and worker behavior. |
| Android environment settings | `MELEE_PIPELINE_SYNC` was read by static initialization before `main` loads `melee-env.txt`, so the file could not enable it. Read it during cache initialization. | Production initialization harness sets the environment after static initialization and verifies both on/off values. |
| 16 KB ELF settings | The build script set only `max-page-size`. Added `common-page-size=16384` to shared-library link flags, covering dependencies built through that configuration. | Official r27-and-older guidance [2]. Final native library LOAD segments use 0x4000 alignment; final APK passes zipalign 16 KB and signature verification. Runtime checked on a 4 KB tablet only. |
| Release signing | The script ignored `MELEE_KEYSTORE_FILE`, although Gradle supported it. Resolve and export the selected absolute path. | Five shell regression scenarios, including a custom path with spaces, missing key, default key, base64 key and preservation of explicit credentials when local defaults exist. |
| Android key handling | Melee's dispatcher bypassed SDL's normal dispatch and consumed every key whenever a surface existed, including system/dialog keys. Remove it and use SDL's inherited handling. | Parent SDL dispatcher and SDLSurface key forwarding inspected; host regression and SDK 34 Java compilation. Android input guidance [3]. Connected-tablet match confirms Volume Up/Down changes media volume; old installed app swallowed them. |
| Touch overlay lifecycle | Queued controller-state updates could reactivate touch while the launcher owned input. Listener registration was lost after detach/reattach. Preserve launcher state and register on attach. | Executable Java regression covers queued updates, launcher transitions, controller connect/disconnect, hidden controls and reattachment. |
| Android touch ownership | Pass-through SDL finger events could reactivate the raw gameplay pad while Java disabled the overlay, or overwrite the JNI pad. Use Java/JNI alone for Android gameplay touch; retain SDL events for launcher UI and the existing fallback on other platforms. | Production C regression verifies disabled touch stays disabled and JNI pad state survives pass-through taps. |
| Input worker failure | A failed `SDL_CreateThread` left the public running flag true. Reset it in the existing failure branch. | Production C regression checks failure, retry, repeated initialization and shutdown. |

The patch preserves Android's existing driver workaround: pipeline compilation
stays inline on the drawing path. It does not re-enable worker compilation on
Adreno or ship the unrestricted desktop pipeline seed. A separate
Android starter inventory is checked for current version/size before local compilation; this is not a
guarantee that every driver accepts every pipeline. Newly encountered shaders can
still stall a frame or have draws skipped after the per-frame budget is spent.

## Compatibility boundaries and follow-up

| Area | Current position | Next validation / implementation condition |
| --- | --- | --- |
| Supported build target | APK ABI is `arm64-v8a`; Java and native minimum are API 26. Compile/target SDK is 34. Hardware must satisfy Dawn/Vulkan requirements. | Minimum Android version alone is not sufficient: query adapter features and test each GPU/driver family [4,5]. No GLES renderer is provided. |
| Odin2 VS rendering | Pipeline scheduling defect fixed; exact artifacts are unidentified. | Retest the same stage/fighters/effects cold, then repeat with a warm cache. Capture screenshots/video and fresh diagnostics if artifacts persist. |
| Shader stalls | Log confirms slow compilation; this patch does not remove first-use compilation cost. | Compare cache-cold/warm frame-time traces. Profile before changing compilation threading or enabling GPU-specific workarounds. Vulkan apps must manage reuse [6]. |
| Surface lifecycle | Existing code tracks surface readiness/background state, locks Android activity access and recreates a released surface. One present warning alone does not prove a lifecycle bug. | Repeated Home/resume, screen lock/unlock, orientation and document-picker round trips on hardware. |
| Audio and thermals | SDL manages Android pause/resume. No demonstrated need to change sample rate, buffers, polling precision or controller calibration. | Measure audio underruns and CPU/GPU/thermal behavior before altering them. `SDL_DelayPrecise` can busy-wait [7,8]. |
| Storage | Disc picker uses SAF/content URIs; native open validates seek/read errors. Legacy storage permissions remain unchanged. | Test local and removable media, persisted URI grants after restart, revoked grants and nonseekable providers. SAF is the appropriate document-access mechanism [9]. |
| JNI failures | Some optional method-lookup failure paths could leave exceptions pending. Bundled Java supplies the expected methods; no normal failure reproduced. | Add injected JNI-failure tests before changing these paths. JNI requires pending exceptions to be handled [10]. |
| Crash diagnostics | Existing export reads the app UID's buffered logcat records. This report has no native crash stack. | Obtain a fresh export immediately after an actual failure; consider Android exit-history traces if log rotation continues to lose evidence [11]. |
| Android 15+ 16 KB | Final arm64 ELF and APK alignment/signature validated; hardware test used 4 KB pages. | Check every packaged `.so` with `llvm-readelf -lW`/`-d`, run `zipalign -c -P 16 -v 4` and `apksigner verify`, then test with `getconf PAGE_SIZE` reporting 16384 [2]. |
| SDK/toolchain updates | CI pins NDK r26; GCC is required for `scalar_storage_order`. Existing Gradle/AGP configuration succeeds locally. | Validate a complete APK and new target-SDK behavior before a toolchain/store migration; do not remove the GCC endianness requirement. |

Recommended device coverage: the reporter's Odin2/Adreno 740 first; an additional
Adreno driver generation; a Mali device; an API 26/28 baseline device; and an
Android 15+ 16 KB device/emulator. This is a proposed test matrix, not a list of
verified supported devices. Hardware comparisons should retain the same game
revision, scene, graphics settings and cache state.

## Verification

Final checks on 20 October 2026:

- `ninja -C build melee unit_tests`: passed, including rollback-section verification.
- `ctest --test-dir build -L melee --output-on-failure`, with JDK 17 available:
  all 60 CTest cases passed, including the new pipeline, compiler, signing,
  Java input, native touch and polling checks.
- `python3 tools/smoke_test.py --no-disc`: all six CLI smoke cases passed.
- Android SDK 34 / JDK 17, offline Gradle
  `:app:compileDebugJavaWithJavac`: build successful.
- `bash -n tools/build_android.sh`, `git diff --check`, and style checks on
  changed native sources/new C test: passed.
- Independent code review found a custom-signing credential precedence issue;
  it was reproduced with a failing test, corrected and reviewed again. No
  actionable findings remained in that review.

Regression cases are registered under the `melee` CTest label. These checks are supplemented by the connected-device run below. Neither a
fix for every Odin2 artifact nor a comparative Android frame-rate improvement
is established.

## Connected-device verification

A fresh arm64 Release native build using NDK 26.3.11579264 and the GCC bridge
linked successfully. Native/Java SDL versions match. The only packaged native
library is `libmelee.so`; its three LOAD segments have `0x4000` alignment and
DT_NEEDED entries are Android system libraries. The final debug APK passes
`zipalign -c -P 16 -v 4` and `apksigner verify` (v2 signature).

Installed the current workspace build, version `v0.4.0-beta` / code 13, as
`dev.melee.game.compat`, labeled **Melee Compatibility Test**, on Samsung
SM-T505 / Android 16 / Adreno 610, driver 512.502.0, 4096-byte pages.
The existing `dev.melee.game` installation and its saves were preserved.
This is a debug-signed local test APK, not a production-signed release.

- SAF successfully selected `/storage/FFBE-E0CB/melee.ciso`. The content URI
  remained readable after installing an update and restarting the app.
- A temporary test-only manifest enabled a CPU VS match and FPS logging.
  Both fighters and the stage rendered, with gameplay touch controls visible.
  The launcher correctly hid the gameplay overlay.
- The old app ignored Volume Up during a match and left STREAM_MUSIC at zero
  and muted. The patched app displayed Android's volume panel and changed
  STREAM_MUSIC from 10 to 11 and back to 10 during the match.
- One Home/resume cycle retained the same process and the match remained
  visible afterward. No fatal signal or Java fatal exception appeared in the
  captured patched-process log. Audio stream start returned success; audible
  quality was not evaluated.
- Cold first-use work produced multi-second startup stalls and intervals of
  about 2.5–8.8 FPS. Later sampled intervals often reached 58–60 FPS, with
  drops into roughly 33–48 FPS and worst frames around 160–282 ms. These
  observations are not a controlled benchmark or proof of improved speed.
- The final installed APK removes all automatic VS/FPS manifest overrides and
  opens the normal launcher with the selected disc. Its SHA-256 is
  `1ee0508b9235a87bb546025945ed6e4f230928c5f54aeb1de7a0bbc8bd203852`.
  Local artifact: `dist/Melee-Android-compat-test.apk`.

Evidence is saved under `local/android-device-20261020/` (ignored by Git).
Odin2, Mali, 16 KB runtime, physical controllers, audio listening quality,
long-session thermals, lock/unlock and broad stage/effect coverage remain
untested. First-use shader stalls remain a known limitation.

## Android presentation performance follow-up

On the same SM-T505, compared short runs of the Link/Mario CPU VS fixture
(`MELEE_BOOT_SCENE=vs`, `MELEE_DEBUG_VS=cpu`, `MELEE_SEED=1`, exit at 1,200
retraces), with the same 1x render scale, no MSAA, 1x anisotropy, reverb off,
SD-card content URI and warmed 506-pipeline cache. All comparison runs rebuilt
those pipelines with 1,830 Dawn cache hits, zero misses and zero stores.
The first warm-up run used the fixture without a fixed seed and is excluded
from the following comparison.

| Run | Present mode | Mean sampled FPS | Sample intervals | Frames >20 ms | Frames >33 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Archive preload on, VSync on | Fifo | 57.15 | 20 | 101 | 18 |
| Repeat preload on, VSync on | Fifo | 58.02 | 20 | 93 | 10 |
| Preload on, VSync off | Mailbox | 59.54 | 20 | 23 | 5 |
| Repeat preload on, VSync off | Mailbox | 59.96 | 19 | 10 | 1 |

Means are averages of the existing approximately one-second FPS log samples.
Counts cover those complete intervals, excluding the initial startup sample
and the unreported partial tail; these are not per-frame percentile traces.
The runs share warmed caches, but OS page-cache state, clocks and scheduling
were not controlled. They are evidence for a local presentation-setting
improvement, not a universal Android FPS guarantee. A single preload-disabled
run averaged 35.82 sampled FPS; it does not justify disabling archive caching.
Reported thermal status remained 0 and battery temperature 27.5 degrees C.

The existing Android **Performance preset** now also switches VSync off,
unless `MELEE_VSYNC` explicitly controls it. The existing backend selects
Mailbox where supported, then Immediate, then Fifo as a fallback. Simulation
continues to use its existing 60 Hz pacing. This is opt-in: initial defaults
and desktop preset behavior are unchanged. Android's frame-pacing guidance
explains why presentation scheduling matters separately from rendering cost
[12]; no frame-pacing SDK or background shader worker was added.

Validation: Android and desktop native rebuilds passed; `launcher_data`,
`inline_pipeline` and `android_gcc_launcher` CTests passed. On the tablet,
restored VSync-on preferences, pressed the real preset Apply button and
asserted the saved values: VSync 0, render scale 1, MSAA 1, anisotropy 1,
reverb 0 and unchanged disc URI. A cold restart retained those preferences
and logged Mailbox presentation. Reinstalled the normal debug APK without
benchmark manifest overrides; APK alignment and signature checks pass.
The selected preset is left active in **Melee Compatibility Test**.

Evidence and measurement summary: `local/android-performance-20261020/`.
First-use shader compilation, other stages/effects, sustained thermal load,
Odin2/Mali devices and drivers without Mailbox remain outside this check.

## Primary sources

Sources checked on 20 October 2026:

1. [Android NDK ABI guide](https://developer.android.com/ndk/guides/abis): ARM64 baseline and reserved `x18`.
2. [Support 16 KB page sizes](https://developer.android.com/guide/practices/page-sizes): ELF/linker settings, APK alignment and runtime testing.
3. [Handle controller actions](https://developer.android.com/games/sdk/game-controller/controller-input): key and motion routing for Android controllers.
4. [Native engines and Vulkan](https://developer.android.com/games/develop/vulkan/native-engine-support): runtime driver/capability selection and testing.
5. [Dawn platform/API support](https://github.com/google/dawn/blob/main/docs/support.md): upstream backend support and feature requirements; the repository uses a pinned implementation.
6. [Vulkan design guidelines](https://developer.android.com/ndk/guides/graphics/design-notes): app-managed reuse and mobile render-pass costs.
7. [SDL Android blocking-on-pause hint](https://wiki.libsdl.org/SDL3/SDL_HINT_ANDROID_BLOCK_ON_PAUSE): pause behavior.
8. [SDL_DelayPrecise](https://wiki.libsdl.org/SDL3/SDL_DelayPrecise): precision/busy-wait tradeoff.
9. [Access documents with SAF](https://developer.android.com/training/data-storage/shared/documents-files): document URIs and retained access.
10. [JNI tips](https://developer.android.com/training/articles/perf-jni): pending-exception restrictions.
11. [ApplicationExitInfo](https://developer.android.com/reference/android/app/ApplicationExitInfo): exit reasons and trace availability.

12. [Android Frame Pacing](https://developer.android.com/games/sdk/frame-pacing): display synchronization, late-frame presentation and buffer queueing.
