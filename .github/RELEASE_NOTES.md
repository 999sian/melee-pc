**Beta, for testing only.** Expect crashes and missing features.

You need your own Super Smash Bros. Melee disc image. **No game data ships in
these artifacts** — the port reads everything, including its font atlases, from
the image you supply at runtime.

**USA revision 2 (NTSC-U 1.02, GALE01)** is the supported disc. A Europe (PAL,
GALP01) image boots experimentally, running the USA game code on PAL data with
English (UK) text.

<!-- Published verbatim by the release job (gh release create --notes-file).
     Per release: replace Highlights and Fixes, refresh Known issues and
     Requirements, move the previous Highlights/Fixes under Previous releases,
     and update the feature status table in README.md (it is the single source
     of truth for what works; nothing here may contradict it). -->

## Highlights

- **Hitlag, SDI and DI are fixed.** Every build before this one gave *every*
  hit in the game exactly 3 frames of hitlag regardless of damage, instead of
  4-20. Hits had almost no freeze, SDI was effectively impossible (one input at
  best, usually none), and because DI is established from the stick at the
  moment hitlag ends, the DI window was 3 frames too, so launches landed at
  their raw undirected endpoint. That reads in play as "no hitlag, no SDI, and
  everybody gets sent way further than usual" — thanks to Syrox for the report
  that identified it. Hitlag is now `floor(floor(floor(d/3 + 3) * e) * c)`
  capped at 20, with the 1.5x electric multiplier on the victim and the
  0.666667x crouch-cancel multiplier, verified against 1634 measured hits.
  Knockback *magnitude* was never affected: 259 measured launches match the
  vanilla formula exactly.
- **Direct3D 11 backend (Windows):** a Direct3D 11 path is now built and ordered after D3D12, ahead of Vulkan, for the GPUs Dawn refuses on D3D12 (Intel Gen7 / Haswell-era iGPUs); it is also selectable in the launcher's *Graphics backend* setting and as `MELEE_BACKEND=d3d11`. **Untested on real Windows hardware**: the adapter enumerates and the fall-back to D3D12 works, but nobody has yet seen a D3D11 device created. Reports with a log are wanted. The log records each skipped backend and why, plus the adapter and driver chosen.
- **Universal Controller Fix (UCF 0.8x):** dashback and shield-drop rules, off by default; toggle on the launcher's Gameplay page or the F1 port menu ("Universal Controller Fix"), or force with `MELEE_UCF=1`.

## Fixes

- **First-use shader pipeline compiles no longer freeze the game (#46):** a draw whose pipeline is still compiling is skipped for a few frames, compiles run on a low-priority worker pool (`MELEE_PIPELINE_JOBS`), and the bundled seed is queued at the session's MSAA level. `MELEE_PIPELINE_SYNC=1` restores the old blocking behaviour.

## Known issues

Open reports are tracked on the [issue tracker](https://github.com/999sian/melee-pc/issues);
the numbers below link there.

**All platforms**

- Online play with rollback netcode is **not implemented**.
- Widescreen applies to fights (VS, Sudden Death, Training); menus, results and
  cutscenes stay at the original aspect. The wide HUD is a separate toggle and
  only moves the timer and the 2-4 player HUD groups.
- A move, stage or effect used for the first time may be missing from the
  picture for a few frames while its shader compiles (previously a freeze,
  #46); `MELEE_PIPELINE_SYNC=1` restores the old wait-for-compile behaviour.
- Only Vulkan, Direct3D 12/11 and Metal backends are shipped; OpenGL and
  OpenGL ES-only GPUs and drivers do not run the game (#64, #66).
- Gamepad buttons, sticks and triggers can be remapped; dash sensitivity,
  tap-jump disable and input buffer settings are not available (#3).
- Some users cannot get past the launcher; if the launcher shows but the game
  never starts, run with the log enabled and attach it (#40).

**Windows**

- v0.1.6-beta and later can close immediately when starting the game from the
  launcher on machines where v0.1.5-beta worked (#62, #63). Attach
  `melee-pc.log` (or run `RUN-AND-LOG.bat`).
- Intel Gen7 iGPUs (HD 4000/4400/4600, Ivy Bridge/Haswell, e.g. i5-4200U) are
  refused by D3D12, and the Direct3D 11 path they are meant to fall back to has
  not been verified on that hardware. A `DXGI_ERROR_DEVICE_HUNG` mid-match
  there (#67) is the 2020-era Intel driver timing out on the GPU; there is no
  further API to fall back to on it. The log names the adapter, backend and
  driver version.

**Android**

- Some devices close the app at launch before the launcher appears, reported
  on an Honor 200 (Snapdragon 7 Gen 3) and an AYN Odin 2 (#59, #23).
- Android 9 and devices without Vulkan are not supported (#66).

**macOS**

- The macOS builds are experimental; the Intel one is untested on hardware.
  The game's C is compiled with Homebrew GCC (`scalar_storage_order`), the
  C++ with Apple clang.

## Config / save compatibility

- `launcher.cfg` (key/value text in the `melee-pc` preference directory:
  `~/.local/share/melee-pc`, `%APPDATA%\melee-pc`, or
  `~/Library/Application Support/melee-pc`) loads across versions: unknown
  keys are ignored and missing keys take their defaults.
- Memory cards are Dolphin-compatible `.gci` files and are unchanged by this
  release.
- Gamepad bindings live in aurora's per-device `.controller` files next to
  `launcher.cfg`.
- The pipeline cache (`pipeline_cache.db` in the same directory) is rebuilt as
  needed; deleting it only costs first-use shader stutter.

## Requirements

- Windows 10/11 (x86-64 or ARM64): Direct3D 12 feature level 11_0 or Vulkan
  1.1; a Direct3D 11 fallback exists but is unverified.
  Linux (x86-64 or aarch64): Vulkan 1.1. macOS: Metal (Apple Silicon tested).
  Android: arm64 with Vulkan 1.1. iOS 14+: arm64, Metal.
- Keep `resources/` (and on Windows the DLLs) beside the executable.
- A Melee USA 1.02 (GALE01) disc image (`.iso`, `.gcm`, `.ciso` or `.rvz`).

## Downloads

| Platform | File | Notes |
|---|---|---|
| Linux x86-64 | `Melee-x86_64.AppImage` | Needs a Vulkan driver. `chmod +x`, then run. |
| Linux x86-64 | `melee-linux-x86_64.tar.gz` | Portable directory; run `run.sh`. |
| Linux aarch64 (ARM64) | `Melee-aarch64.AppImage` | For 64-bit ARM Linux (Raspberry Pi 5, Asahi Linux, Orange Pi). |
| Linux aarch64 (ARM64) | `melee-linux-aarch64.tar.gz` | Portable directory for 64-bit ARM Linux; run `run.sh`. |
| Windows x86-64 | `Melee-Windows-x86_64.zip` | Extract and run `melee.exe`. Keep the DLLs and `resources/` beside it. |
| Windows ARM64 | `Melee-Windows-arm64.zip` | Native 64-bit ARM build for Windows on ARM (Snapdragon X Elite, Surface Pro). |
| Android arm64 | `Melee-Android-arm64.apk` | Release build, signed. Allow install from unknown sources. |
| iOS arm64 | `Melee-iOS-arm64.ipa` | Sideloadable IPA (AltStore, Sideloadly, TrollStore) with Metal backend. |
| macOS arm64 | `Melee-macOS-arm64.zip` | Apple Silicon. Ad-hoc signed: right-click > Open on first launch. |
| macOS x86_64 | `Melee-macOS-x86_64.zip` | Intel. Same notes; built but not yet tested on Intel hardware. |

Launch with no arguments to open the launcher and pick a disc, or pass the
image path directly:

```sh
./Melee-x86_64.AppImage /path/to/melee.iso
```

## Previous releases

### Changes in v0.1.7-beta

- **Native Windows ARM64 Support:**
  - Added pure native ARM64 PE executable (`Melee-Windows-arm64.zip`) compiled with `llvm-mingw` and GCC-powered big-endian scalar storage order translation.
  - Bundles native ARM64 WebGPU/Dawn (`dxcompiler.dll`, `dxil.dll`, `webgpu_dawn.dll`), Nod (`nod.dll`), and app-local Visual C++ ARM64 runtime DLLs.
  - Tested on Windows 11 on ARM devices including Qualcomm Snapdragon X Elite, Snapdragon 8cx Gen 3, and Microsoft Surface Pro Copilot+ PCs.

- **Native iOS Support (arm64):**
  - Added native iOS app bundle and sideloadable package (`Melee-iOS-arm64.ipa`) targeting iOS 14.0+ arm64.
  - Metal graphics backend powered by WebGPU/Dawn with seamless resolution scaling and retina display support.
  - Touch input through fixed screen regions (stick on the left half, face buttons bottom right). There is no drawn overlay, no calibration and no controller auto-hide on iOS; that overlay is Android-only.
  - RmlUi settings launcher with auto-detection of game images in the app sandbox and Apple `os_log` system logging.

- **Memory Safety & Fighter Stability Fixes:**
  - **Falco & Fox Illusion Afterimage Crash Fix:** Fixed memory corruption and access violations in `ftafterimage.c` and `itfoxillusion.c` during Event 23 and fast multi-afterimage rendering.
  - **64-bit Disc Pointer Reconstruction (DP macro):** Reconstructed 64-bit host pointers across fighter, item, stage, and particle systems, ensuring safe referencing on 64-bit architectures.
  - **Windows/MinGW DECL_WEAK Symbol Resolution (fixes #61):** Resolved weak symbol linking behavior in MinGW to prevent null function pointer dereferencing on `OSReport` calls.
  - **Audio Thread Concurrency & Mutex Safety:** Resolved audio thread startup race condition and mutex re-entrancy in `audio.c`.

- **UI & Layout Synchronizations:**
  - Synchronized Cheats menu layout and resources between pre-game launcher and in-game F1 settings overlay.

### Changes in v0.1.6-beta

- **Linux aarch64 (ARM64) Support:**
  - Added native Linux ARM64 AppImage (`Melee-aarch64.AppImage`) and portable tarball (`melee-linux-aarch64.tar.gz`) builds via GitHub Actions on Ubuntu ARM runners.
  - Integrated Nod prebuilts for aarch64 Linux and configured automated cross-compilation pipeline.
  - Tested and verified on 64-bit ARM Linux platforms including Raspberry Pi 5, Asahi Linux on Apple Silicon, Orange Pi, Rockchip RK3588, and Linux ARM handhelds.

- **Phase 1 Feature Pack & Cheats Restructuring:**
  - **Custom Soundtrack Streaming:** Embedded `stb_vorbis` audio stream decoder supporting runtime `.ogg` and `.wav` file overrides for BGM. Automatically loads stage soundtrack replacements placed in `music/` or loose folders and blends them with in-game music volume controls.
  - **Free / Unlocked Pause Camera:** Added a Free Camera cheat toggle in settings and in-game F1 overlay, permitting full 360-degree pitch/yaw rotation and 0.5f–5000.0f zoom distance while paused.
  - **Wide HUD Anchoring (16:9):** Match timer, stock icons, damage percentages, and player tags are dynamically anchored outwards for native 16:9 widescreen viewports (toggleable between Classic 4:3 and Wide 16:9 in Graphics settings).
  - **Dedicated Cheats Tab:** Restructured launcher and in-game F1 menu with a dedicated "Cheats" tab housing *Unlock All*, *Frozen Stadium* (hazardless Pokémon Stadium), and *Free Camera*.

- **Multi-Core & Responsiveness Architecture:**
  - **1000 Hz Input Polling Thread:** Decoupled controller polling (`HSD_PadRead`) from the 60 Hz game loop into a dedicated 1000 Hz OS worker thread (`src/pc/input_poll.c`), minimizing input latency and polling jitter across all controllers.
  - **SIMD AX Voice Mixer:** Vectorized voice processing and mixing loops using ARM NEON and x86 AVX, significantly reducing CPU usage during heavy multi-player sound effect spam.
  - **Decoupled Audio Pipeline & Concurrency:** Removed global `OSDisableInterrupts()` lock from `render_frame()`, restricting interrupt disables strictly to the 5 ms synth tick, and protected voice parameter updates under a dedicated recursive `s_audio_mutex`.
  - **4-Core & Handheld / Switch Scheduling Fixes (fixes #47):** Removed restrictive 2-core thread pinning on Nintendo Switch (Tegra X1) and 4-core Linux/ARM SBCs, elevating FIFO and render worker thread priorities so all cores are utilized evenly without core thrashing.

- **In-Memory Persistent Asset Cache & Fast Loading:**
  - **In-Memory Persistent Asset Cache (`file_cache.cpp`):** Pristine raw disc archives (`.dat`, `.usd`) are cached in host RAM on first read, providing instant 0 ms loads on recurring character, stage, and menu transitions.
  - **Background Asset Pre-Warming:** Background worker preloads Tier 1 tournament files (core fighter files, tournament stages, common UI) on boot without hitching gameplay.
  - **Adaptive Low-End RAM Budgeting & LRU Eviction:** Dynamic cache budgeting (`PROFILE_LOW_RAM` <= 2GB, `PROFILE_HANDHELD` 2-4GB, `PROFILE_DESKTOP` > 4GB) with LRU eviction and I/O throttling for low-memory systems (Raspberry Pi 4, low-RAM SBCs).
  - **Loose Directory VFS Overlays:** Seamlessly load replacement game files from local loose folders (`MELEE_FILES_DIR`, `./files/`) without rebuilding ISOs.
  - **Snappy Transitions:** Fast fade delay clamping (optional `MELEE_FAST_FADES`).

- **Android & Mobile Optimizations:**
  - **Android 60 FPS First-Play Intro Optimization:** Eliminated main-thread pipeline compilation stalls during `MvOpen.mth` by stopping unrequested background shader queue drainage when `!g_hasPipelineThread`.
  - **Faster Android Boot Times:** Instant check in `seed_pipeline_cache()` skips SQLite re-seeding if already populated, cutting 1.5–3.0s off warm launches.
  - **Adreno GPU Color Correction (fixes #20):** Prefer RGBA8Unorm swapchain format to fix inverted red/blue colors on Qualcomm Adreno GPUs.

- **Memory Management & OS Startup Stability:**
  - **Early MEM1 Pre-Allocation (fixes #43):** Pre-allocates GameCube MEM1 at process startup via `OSInit()` before SDL and GPU drivers fragment low 32-bit virtual memory.
  - **Windows VirtualAlloc2 64KB Alignment (fixes #43):** Fixed 64KB alignment (`0xFFFF0000`) and added VirtualQuery scanning fallback to guarantee MEM1 sits strictly under 4GB on Windows 10/11.
  - **ARAM Address Translation in File Cache (fixes #50):** Fixed fatal access violations in Adventure Mode and character loading by translating ARAM addresses (`< 0x01000000`) via `aurora_aram_base()`.

- **Extensive Bug Fixes & Game Corrections:**
  - **fixes #48:** Fixed crash when Kirby swallows and spits Sandbag in Home Run Contest (joint validity and null checks).
  - **fixes #45:** Fixed Event 23 / Venom stage crash (`lb_8000B1CC` null guard, 64-bit joint pointer loop in `grVenom_8020454C`, Arwing slot bounds checks).
  - **fixes #24:** Fixed Falco crash caused by out-of-bounds `items[3].v` access and cleared `blasterGObj` on load.
  - **fixes #33, #34:** Fixed GX lighting bugs by preserving RGB when writing Alpha in `GXSetChanAmbColor` / `GXSetChanMatColor`.
  - **fixes #21, #36, #41:** Fixed soundbank eviction and SFX header load overflow checks.
  - **fixes #26:** Fixed Yoshi Egg breakout particle scalar storage order and Kirby accessory null checks.
  - **fixes #51:** Fixed fanfare silence on achievement popups by resetting audio stream fade counter and restoring stream gain.
  - **fixes #52:** Fixed fullscreen crash with active overlays (NVIDIA ShadowPlay / Discord overlay) by guarding 0x0 swapchain reconfiguration.
  - **fixes #53:** Fixed trophy fall depth copy crash.
  - **fixes #55:** Fixed stage clear screenshot opacity.
  - **fixes #54:** Fixed flickering reflection texture on Great Bay hook model.
  - Fixed intro movie boot failure caused by memory card struct mismatch between 32-bit and 64-bit definitions.

- **Upstream Decomp Sync & Tooling:**
  - Synchronized codebase with upstream Melee decomp up to commit `194350655ef3c2c301359fc06487227098732f71`.
  - Added Discord community link and icon to launcher and settings.
  - Pre-seeded Vulkan pipeline cache extracted across Linux, Windows, and Android builds.
  - Expanded C++20 endian helpers in `endian.hpp` and `disc.h`.
  - Established coding standards (`CODING_STYLE.md`, `.editorconfig`, `.clang-format`, `.clang-tidy`) with automated CI style checking (`tools/check_style.py`).

### Changes in v0.1.5-beta

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

### Changes in v0.1.4-beta

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

### Changes in v0.1.3-beta

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

### Changes in v0.1.2-beta

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

### Changes in v0.1.1-beta

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

Every game mode runs. The per-feature list, including what is only partly
done, is the status table in
[README.md](https://github.com/999sian/melee-pc#status); it is the single
source of truth and these notes defer to it.

## Controls

Arrows or WASD = stick, IJKL = C-stick, X = A, Z = B, C = X, V = Y, Q/E = L/R,
Tab = Z, Enter = Start, TFGH = D-pad. Gamepads work through SDL and can be
remapped; an official GameCube adapter is read directly. **F1** opens the
settings overlay.

## Contributors

### Project Contributors
- **@999sian** — Project Lead, Phase 1 features, multi-core optimizations, file cache, Android & Windows porting, and stability fixes.
- **@theofficialgman** — Linux aarch64 (ARM64) support, Nod aarch64 prebuilts, 4-core & ARM scheduling optimizations (#44, #47).
- **@alexscott2718-gif** — Graphics backend selection, command line overrides, and engine logging.
- **@r-burns** — Melee decompilation and 64-bit portability foundations.
- **@MarkMcCaskey** — Decompilation and core engine maintenance.
- **@ribbanya** (Robin Avery) — Decompilation and memory card subsystem.
- **@PsiLupan** (Will Carter) — Decompilation and subsystem typing.
- **@itsgrimetime** (Mike Grimes) — Decompilation foundations.

### Community Testers & Issue Reporters
Special thanks to our community members whose detailed bug reports and reproduction steps directly helped diagnose and resolve issues in these releases:
- **@jennywakeman-xj9** (#30, #31, #32, #33, #34, #35, #36, #38, #39, #51, #53, #54, #55, #56, #57, #58)
- **@omega-tuna** (#48)
- **@VTuberSkye** (#45)
- **@4zy1** (#49, #50)
- **@stevenstallone** (#52)
- **@mmedeiro1-a11y** (#43)
- **@Keithmccloud** (#59)
- **@Smashhacker** (#41, #60)
- **@whirlwindpedro** (#40)
- **@zamiba** (#42)
- **@nitrostemp** (#37)

### Upstream Projects & Foundations
- **[doldecomp/melee](https://github.com/doldecomp/melee)** — The Super Smash Bros. Melee decompilation team and contributors.
- **[encounter/aurora](https://github.com/encounter/aurora)** — Luke Street (@encounter) and contributors for the GameCube hardware emulation layer and WebGPU backend.
- **[TwilitRealm/dusklight](https://github.com/TwilitRealm/dusklight)** — Architectural inspiration for GameCube PC ports.
- **SDL3, RmlUi, stb_vorbis, and Dawn teams** for the runtime engine libraries.

