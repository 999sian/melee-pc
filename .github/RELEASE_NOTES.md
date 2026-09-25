**Beta, for testing only.** Expect crashes and missing features.

You need your own Super Smash Bros. Melee disc image. **No game data ships in
these artifacts** — the port reads everything, including its font atlases, from
the image you supply at runtime.

**USA revision 2 (NTSC-U 1.02, GALE01)** is the supported disc. A Europe (PAL,
GALP01) image boots experimentally, running the USA game code on PAL data with
English (UK) text.

<!-- Published verbatim by the release job (gh release create --notes-file).
     Per release: replace Highlights and Fixes, refresh Known issues and
     Requirements, link earlier releases, and update the feature status table
     in README.md (it is the single source
     of truth for what works; nothing here may contradict it). -->

## Highlights

- **Direct Connect** accepts a friend's eight-character code in the game,
  offers clipboard and recent opponents, and lets either player call.
- Set `MELEE_SLP_DIR` to record offline or online VS matches as `.slp` replays
  (off by default).

## Fixes

- Fixed Items: Off online handshakes, sped up matchmaking, and corrected LAN
  discovery on non-release builds and Windows VPN setups (thanks @Joyastick,
  #91–#93).
- Improved rollback, desync handling and netplay sound effects.
- Fixed the Android startup library mismatch. Android now warms known graphics
  pipelines while Direct Connect waits; occasional frame stalls remain.

## Known issues

Open reports are tracked on the [issue tracker](https://github.com/999sian/melee-pc/issues);
the numbers below link there.

**All platforms**

- Online play is a prototype. LAN Play, Direct Connect, Unranked and Ranked are
  implemented and rendezvous through the public DHT, but pairing across two
  NATs in general and live ranked acceptance are unproven, and cross-platform simulation
  determinism is not claimed. Ratings are community-computed and unverified.
  Every datagram is authenticated (protocol 9), and the LAN lobby
  only pairs identical builds, so both players should update to v0.2.2. A
  connect code is 8 characters after the `#`.
- Widescreen applies to fights (VS, Sudden Death, Training); menus, results and
  cutscenes stay at the original aspect. The wide HUD is a separate toggle and
  only moves the timer and the 2-4 player HUD groups.
- A move, stage or effect whose shader is still compiling is missing from the
  picture for a few frames (previously a freeze, #46). The bundled seed is
  queued for compilation on worker threads when the game starts, and the
  launcher waits for that queue when you press Play -- press Play again to skip
  the wait. `MELEE_PIPELINE_SYNC=1` restores the old wait-for-compile
  behaviour, and `MELEE_PIPELINE_JOBS=<n>` sets the compile thread count.
  On Android, Qualcomm Adreno GPUs still compile shaders inline, because the
  Adreno 750 driver fails when pipelines are created on a second thread, so
  expect some first-use stutter there.
- Only Vulkan, Direct3D 12/11 and Metal backends are shipped; OpenGL and
  OpenGL ES-only GPUs and drivers do not run the game (#64, #66).
- Gamepad buttons, sticks and triggers can be remapped; dash sensitivity,
  tap-jump disable and input buffer settings are not available (#3).
- Some users cannot get past the launcher; if the launcher shows but the game
  never starts, run with the log enabled and attach it (#40).

**Browser**

- Tested in Chrome only; Firefox and Safari WebGPU support is incomplete.
- Only a raw, uncompressed USA revision 2 (GALE01) image loads: no `.ciso`,
  `.rvz` or PAL.
- No online play, and no gamepad remapping (the default mapping is used; the
  keys are listed under the canvas).

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
  Android: arm64 with Vulkan 1.1. iOS 14+: arm64, Metal. Browser: Chrome or
  Edge with WebGPU.
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
| Browser | [999sian.github.io/melee-pc/play](https://999sian.github.io/melee-pc/play/) | Chrome or Edge with WebGPU. Raw `.iso`/`.gcm` only; no online play. |

Launch with no arguments to open the launcher and pick a disc, or pass the
image path directly:

```sh
./Melee-x86_64.AppImage /path/to/melee.iso
```

## Previous releases

See [earlier releases](https://github.com/999sian/melee-pc/releases) for prior changelogs.

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
- **@Joyastick** — Real two-machine netplay testing and fixes (#91, #92, #93).

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

