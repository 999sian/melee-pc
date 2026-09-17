**Beta, for testing only.** Expect crashes and missing features.

You need your own Super Smash Bros. Melee disc image. **No game data ships in
these artifacts** — the port reads everything, including its font atlases, from
the image you supply at runtime.

Only **USA revision 2 (NTSC-U 1.02, GALE01)** is supported.

## Downloads

| Platform | File | Notes |
|---|---|---|
| Linux x86-64 | `Melee-x86_64.AppImage` | Needs a Vulkan driver. `chmod +x`, then run. |
| Linux x86-64 | `melee-linux-x86_64.tar.gz` | Portable directory; run `run.sh`. |
| Linux aarch64 (ARM64) | `Melee-aarch64.AppImage` | For 64-bit ARM Linux (Raspberry Pi 5, Asahi Linux, Orange Pi). |
| Linux aarch64 (ARM64) | `melee-linux-aarch64.tar.gz` | Portable directory for 64-bit ARM Linux; run `run.sh`. |
| Windows x86-64 | `Melee-Windows-x86_64.zip` | Extract and run `melee.exe`. Keep the DLLs and `resources/` beside it. |
| Android arm64 | `Melee-Android-arm64.apk` | Release build, signed. Allow install from unknown sources. |

Launch with no arguments to open the launcher and pick a disc, or pass the
image path directly:

```sh
./Melee-x86_64.AppImage /path/to/melee.iso
```

## Changes since v0.1.5-beta

- **Linux aarch64 (ARM64) Support:**
  - Added native Linux ARM64 AppImage (`Melee-aarch64.AppImage`) and portable tarball (`melee-linux-aarch64.tar.gz`) builds via GitHub Actions on Ubuntu ARM runners.
  - Supports 64-bit ARM Linux platforms including Raspberry Pi 5, Asahi Linux on Apple Silicon, Rockchip RK3588, and ARM64 handhelds.

- **Android 60 FPS First-Play Intro Optimization:**
  - **Eliminated main-thread background shader compilation stalls:** Fixed the low frame rate (~10 FPS) stutter that occurred during the first play of `MvOpen.mth` on Android devices. When `!g_hasPipelineThread`, Aurora now only processes active draw pipelines in `pipeline_worker()`, never stalling presentation to compile unneeded background pipelines.
  - **Accelerated boot times:** Added a fast-path check in `seed_pipeline_cache()` that skips re-querying and re-upserting 11,905 seed rows into SQLite if the cache database is already populated, cutting 1.5 to 3.0 seconds off warm launches across all Android devices.

- **Decoupled Audio Pipeline & Concurrency:**
  - **Decoupled audio frame rendering:** Removed the global `OSDisableInterrupts()` lock from `render_frame()`, restricting interrupt disabling strictly to the 5 ms game synth tick.
  - **Dedicated audio mutex:** Protected voice parameter modifications under a dedicated recursive `s_audio_mutex`, ensuring audio mixing and reverb never contend with controller polling (`HSD_PadRead`), alarms, or GX rendering.

- **Pre-Seeded Vulkan Pipeline Cache:**
  - Automated extraction of `tools/initial_pipeline_cache.db.gz` via CMake during build and packaging across Linux, Windows, and Android, pre-seeding pipeline configurations to minimize shader pop-in.

- **C++20 Endian Helpers & Clang Portability:**
  - Expanded endian conversion helpers in `endian.hpp` (`BE<Mtx>`, `BE<Mtx44>`, `BE<Mtx23>`, array swaps) and provided Clang C++20 `BE<T>` wrappers in `disc.h` to establish the foundation for pure Clang builds on ARM architectures.

- **Upstream Decomp Sync & Bugfixes:**
  - Synced codebase with upstream Melee decomp up to `194350655ef3c2c301359fc06487227098732f71`.
  - Fixed #54: Fixed flickering reflection texture on Great Bay hook model.
  - Fixed intro movie boot failure caused by memory card struct mismatch.
  - Fixed #55: Fixed stage clear screenshot opacity.
  - Fixed #53: Fixed trophy fall depth copy crash.
  - Fixed #51: Fixed fanfare silence on achievement popups.
  - Fixed #52: Fixed fullscreen crash with overlays.
  - Fixed #50: Fixed Adventure Mode character loading crash by translating ARAM addresses in file cache.
  - Fixed #48: Fixed crash when Kirby swallows and spits Sandbag in Home Run Contest.
  - Fixed #45: Fixed Event 23 / Venom stage crash.
  - Added Discord community link and icon to launcher.

## Changes since v0.1.4-beta

- **Android Performance & Frame Pacing Overhaul (Full 60 FPS):**
  - **Eliminated GX FIFO futex wake storm:** Slashed kernel context-switching overhead by over 95% by increasing `kDrawBatchSize` from 1 to 16 and gating thread wakeups on active waiter state (`sWorkerWaiting` / `sMainThreadWaitingForProcessed`), completely removing the 155% sys CPU time lockup on mobile GPUs.
  - **Unpinned CPU threads for mobile schedulers (EAS):** Disabled strict core cache domain affinity on Android so the Linux kernel Energy Aware Scheduler (EAS) can dynamically migrate audio, video, and render threads across prime and performance cores without triggering Qualcomm CPU frequency throttling down to 600 MHz.
  - **Locked 60.0 Hz display refresh mode:** Configured Android window attributes to explicitly request a 60 Hz display refresh rate, eliminating frame cadence judder and swapchain pacing mismatches on 120 Hz and 144 Hz mobile displays.
  - **Native Android logging:** Integrated Aurora engine diagnostics directly into Android logcat (`__android_log_print` under tag `Aurora`).

- **On-Screen Touch Controls & Controller Auto-Detection (Android):**
  - **Complete GameCube touch layout:** Added an ergonomic, responsive on-screen overlay featuring the analog Control Stick, C-Stick, A, B, X, Y, Z, L, R, D-Pad, and Start buttons.
  - **Quick Settings modal:** Added a dedicated overlay gear button to dynamically toggle touch controls, invert C-Stick Y axis, toggle haptic vibration, adjust stick deadzones, and calibrate overlay opacity.
  - **Automatic physical controller detection:** Touch controls automatically hide when a physical Bluetooth or USB gamepad is connected and actively used, and seamlessly reappear as soon as the touchscreen is tapped.
  - **Persistent settings:** Touch settings and calibration are saved to `launcher.cfg` across app launches.

- **Zero-Copy Disc Streaming & Loading Speed:**
  - Implemented zero-copy memory-mapped (`mmap`) streaming in Aurora's DVD reader for uncompressed raw ISO images.
  - Enlarged DevCom I/O buffers and optimized background disc streaming threads, drastically cutting synchronous read hitches and audio desyncs during match transitions and movies.

- **Fighter, Stage & Engine Fixes:**
  - Synced with upstream Melee decomp (`662250b9`).
  - Fixed #18: Corrected Melee display aspect ratio (73:60) and 16:9 widescreen projection scaling.
  - Fixed #19: Fixed Mute City particle generator leak and subsequent FPS drop.
  - Fixed #27: Fixed Classic mode Mario trophy reward crash caused by incorrect return type in `gm_1736`.
  - Fixed #29: Corrected Giga Bowser KO bonus endian bitfield layout.
  - Fixed #12, #30, #38: Corrected collision bitmask types in item ground collision (`itgroundcoll`).
  - Fixed #39: Corrected Corneria Star Fox dialogue cutscene argument types.
  - Fixed #32: Clamped Wobbuffet damage underflow when frozen.
  - Fixed #25, #35: Added null-safety checks in `grzakogenerator` and `itoldottosea`.
  - Fixed #16: Corrected Bowser fire breath animation loop condition.
  - Fixed #31: Fixed Yoshi egg breakout particle effect scalar storage order.
  - Fixed disc pointer camera/light animations and memory free safety in trophy scene.

## Changes in v0.1.4-beta

- **Fighter & Gameplay Fixes:**
  - Fixed #16: Fixed Bowser's Neutral Special (Fire Breath) getting stuck permanently. Frame counter `xC` in `ftKoopa_SpecialNVars` was previously declared as `bool`, preventing the timer from reaching the 40-frame threshold required to detect B-button release and transition into `SpecialNEnd`.
  - Fixed #15: Fixed Bunny Hood attachment rendering on fighter heads by loading ear offset vectors through `DISC_VEC3_GET` in `ftCommon_8007FA00`, restoring big-endian float byte-swapping.
  - Fixed #14: Fixed Giant Melee sound effects playing at high pitch instead of low pitch by correcting `Player_GetMoreFlagsBit6` return type from `bool` to `u8`, preserving the Giant flag without truncating it to Tiny.
  - Fixed #12: Fixed reflector and shield item behavior by correcting `ReflectDesc.x20_behavior` to `s32`.
  - Fixed #11: Fixed item capsule drop crash caused by `ItCapsuleAttr.x0` being typed as `bool` instead of `s32`.
  - Fixed #10: Fixed Tournament Mode crash caused by big-endian `u16` table access in `lbl_803D9F80`.
  - Corrected big-endian disc layouts for `itOldottoseaAttributes`, `ScopeBeamAttrs`, `itToolsMotionAttrs`, `GroundParam`, `itWhiteBeaAttributes`, and `ftKb_DatAttrs`.
  - Fixed command stream endian decoding in `grMaterial_801C9490`.

- **Platform & System Stability:**
  - Fixed #9: Fixed Android MEM1 mapping crash (`Failed to map MEM1 at 0x80000000`) on Android 11+ by scanning candidate ranges strictly below 4GB when the default base address is occupied.
  - Fixed #13: Fixed Android crash shortly after launch by disabling pre-warmed background Vulkan pipeline worker threads that conflicted with Qualcomm Adreno drivers during asset loading.
  - Fixed 64-bit pointer truncation in `OSRoundUp32B` and `OSRoundDown32B` using `uintptr_t`.
  - Fixed 64-bit pointer safety across `HSD_SisLib_803A84BC`, screenshot saves, Sheik chain joint creation, Green Greens blocks, and fighter accessory cleanup.
  - Clamped audio pitch ratio to 4.0f to eliminate `cvttss2si` signed integer overflow undefined behavior on x86-64.
  - Fixed JPEG Huffman AC code byte-swapping in snapshot saving (`hsd_3B34.c`, `hsd_3B5C.c`) and added `DISC_STRUCT` to snapshot save headers.
  - Fixed Event Mode text color RGBA channel ordering on little-endian platforms.

## Changes in v0.1.3-beta

- **Performance & Stuttering:**
  - Deconflicted hardware VSync and software frame pacing in `vi.c`. Manual `SDL_DelayPrecise` sleep now only runs when VSync is disabled, preventing monitor refresh rate drift from tripping sudden 30 FPS drops under strict FIFO VSync.
  - Bundled the pre-recorded pipeline cache seed (`initial_pipeline_cache.db`) into the Linux AppImage, Linux portable tarball, and Android APK assets (previously only shipped on Windows), eliminating first-run shader compilation pop-in and stutter across all platforms.
  - Added user-facing Graphics Backend selection (Direct3D 12 vs. Vulkan vs. Auto) in the Launcher settings, in-game F1 overlay, and `launcher.cfg`.
  - Scaled disc preloader threads dynamically up to 4 concurrent threads in Aurora's DVD reader, parallelizing block decompression for compressed `.ciso` and `.rvz` disc images to reduce synchronous asset load hitches.
  - Added CMake support for Link-Time Optimization (`MELEE_ENABLE_LTO`).
- **Game & Platform Fixes:**
  - Fixed #5: Prevented memory corruption and crash when backing out of Tournament mode by properly typing archive handles.
  - Fixed #4: Set stage clear flag on 100-man melee completion so Falco challenger approach is triggered.
  - Fixed #8: Fixed infinite sparkle loop on Final Destination.
  - Fixed #7: Scanned user space under 4GB for MEM1 allocation on Windows, and auto-detected ISOs in `RUN-AND-LOG.bat`.
  - Added GameCube ISO file selection support on Android.
  - Statically linked `libstdc++` and `libgcc` on Windows and removed mismatched compiler runtime DLLs.

## Changes in v0.1.2-beta

- Fixed disc pointers being used without resolution in the HSD object
  loaders. `HSD_IDGetData` is keyed on the resolved host pointer, but jobj,
  pobj and robj looked up with the raw 32-bit disc slot, so those lookups
  always missed and left child joints and envelope references null.
- Rewrote the AObj animation callback dispatch. It previously guessed at
  four signatures, reading a float out of parameters that hold an integer
  or a pointer and dropping arguments entirely in other cases; it now
  dispatches on the real calling convention.
- Kept MEM1 below 4GB on Windows. The allocator fell back to letting the OS
  place it anywhere, which on 64-bit Windows means above 4GB, and every
  32-bit disc pointer slot into it then truncates.
- `MELEE_BACKEND` pins the graphics backend (`vulkan`, `d3d12`, `null`, ...)
  and the log now records which one a run selected. Thanks to
  @alexscott2718-gif.
- The log is timestamped, records a marker for any frame over 50ms, and
  survives a crash: output is flushed per line, Windows writes
  `melee-pc.log` beside the exe, and a fault logs a backtrace naming the
  module it came from.
- The Windows zip ships a pipeline cache seed, so shaders are not all
  compiled the first time each one is used.

## Changes in v0.1.1-beta

- Fixed a crash in the attract demo. Kirby's and Jigglypuff's multi-jump
  attributes are read straight off the disc, but were decoded in the wrong
  byte order, so the second jump looked up motion state `0x55010000` instead
  of `341` and faulted.
- Fixed the remaining places where a pointer was stashed in a 32-bit field
  and truncated on 64-bit builds: the HSD id table and object heap, the
  sislib text cursor stack, the THP video decoder, and pointer slots in the
  Hyrule Castle, Brinstar, Big Blue and Fountain of Dreams stage state.
- Fixed the Windows build failing to start on a real Windows PC. The zip did
  not ship the Visual C++ runtime that Dawn, dxcompiler, SDL3 and nod import,
  so Windows refused to load it with "VCRUNTIME140.dll was not found". Wine
  and Proton supply that runtime themselves, which is why it only broke on
  actual Windows. Those DLLs now ship in the zip, and packaging fails if any
  import is left unresolved.
- The Android APK is now a signed release build rather than a debug build,
  and is named `Melee-Android-arm64.apk`.
- Fixed the Android CI build, which depended on a toolchain path that only
  existed on one machine.

## What works

Boot and opening movie, memory card create/load, title and attract demos, main
menu, VS Mode with character and stage select, 1-P Classic and Adventure to
completion with results saved, Training, Stadium (Target Test, Home-Run
Contest, 10-Man Melee), Trophy gallery, Event Match list, music and sound.

## What does not

Online play with rollback netcode is **not implemented**. All-Star is
unreachable until the roster is unlocked. Widescreen camera and HUD are
incomplete. There is no macOS build: the game code depends on GCC's
`scalar_storage_order`, which Clang does not implement.

## Controls

Arrows = stick, IJKL = C-stick, X = A, Z = B, C = X, V = Y, Q/E = L/R,
Tab = Z, Enter = Start, TFGH = D-pad. Gamepads work through SDL and can be
remapped. **F1** opens the settings overlay.
