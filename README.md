# melee-pc

**Beta, for testing only.** "melee-pc" is a working name. Online play with
rollback netcode is planned and **not implemented yet**.

A native PC port of Super Smash Bros. Melee (NTSC-U 1.02), built from
[doldecomp/melee](https://github.com/doldecomp/melee) on top of
[aurora](https://github.com/encounter/aurora) (GX/OS/PAD/DVD/CARD/THP
compatibility layer with a WebGPU backend) and SDL3. Same approach as
[dusklight](https://github.com/TwilitRealm/dusklight).

You need your own disc image. **No game data ships here.** The decompiled game
code is not licensed and is not relicensed by this project; only the port code
is GPL-3.0-or-later. Details under [License](#license).

> New here? The **[project site](https://999sian.github.io/melee-pc/)** has the
> five-step setup, the FAQ (supported disc, Windows first run, older Intel GPUs,
> first-use shader stutter, Android requirements, where the log and settings
> live) and the per-platform known-issues list. Bugs go through the
> [bug report form](https://github.com/999sian/melee-pc/issues/new?template=bug_report.yml);
> questions on [Discord](https://discord.gg/aurt34svq).

## Features

- Native builds for Linux (x86-64, aarch64), Windows (x86-64, ARM64), macOS,
  Android and iOS, rendered through Dawn/WebGPU (Vulkan, D3D12, D3D11, Metal)
  and SDL3.
- RmlUi launcher with disc selection and SHA-1 verification against the Redump
  database before boot.
- In-game settings overlay on **F1**, with the game paused underneath.
- Internal resolution from Auto to 10x native (6400x4800).
- Post-processing shaders: area sampling, CRT scanlines, vibrant.
- 4x MSAA and anisotropic filtering up to 16x.
- Gamepad remapping, including C-stick directions, saved per device.
- Software AX audio mixer with Master, Music and SFX volume controls.
- User `.ogg` / `.wav` tracks replace stage BGM.
- Dolphin-compatible `.gci` memory cards and Dolphin-format HD texture packs.
- Cheats: Unlock Everything, hazardless (Frozen) Pokémon Stadium, free pause
  camera, Wide 16:9 HUD.
- UCF 0.8x dashback and shield drop, and a raw 1000 Hz read path for the
  official GameCube controller adapter.

What each of those actually covers, including the parts that are unfinished, is
in the [status table](#status) below.

## Screenshots

![Title screen](docs/screenshots/title.png)

| | |
|---|---|
| ![Main menu](docs/screenshots/main-menu.png) | ![Character select](docs/screenshots/character-select.png) |
| Main menu | Character select |
| ![Stage select](docs/screenshots/stage-select.png) | ![Gameplay](docs/screenshots/gameplay-4p.png) |
| Stage select | Four-player match |
| ![Gameplay](docs/screenshots/gameplay-onett.png) | ![Settings](docs/screenshots/pc-settings.png) |
| Onett | F1 settings overlay |

![Launcher](docs/screenshots/launcher.png)

## Status

Every mode boots and plays: VS, 1-P Classic / Adventure / All-Star to
completion with results and score saved, Training, Stadium (Target Test,
Home-Run Contest, 10-Man Melee), Event Match, Trophy gallery, memory card
create / load, opening movie and attract demos.

**This table is the single source of truth for feature status.** The release
notes, the project site and `ROADMAP.md` defer to it; when they disagree, this
table is right and the other one is stale.

| Feature | Status | Note |
|---|---|---|
| Linux x86-64 / aarch64 | done | AppImage and tarball, both built in CI. |
| Windows x86-64 / ARM64 | done | D3D12 or Vulkan; ARM64 via llvm-mingw. |
| Direct3D 11 backend (Windows) | partial | Compiled into the shipped Dawn for both architectures, ordered after D3D12 and selectable as `MELEE_BACKEND=d3d11`. The adapter enumerates and the fail-over to D3D12 is proven, but no working D3D11 device has been observed; Wine/Proton cannot create one (`CreateDeviceContextState` returns `E_INVALIDARG`), so it is unverified on real Windows and on the Intel Gen7 hardware it exists for. |
| Android arm64 | done | Drawn on-screen GameCube overlay with opacity, deadzone and haptics settings; hides itself when a physical gamepad is connected. |
| iOS arm64 | partial | Sideloadable IPA on Metal, cross-built from Linux. Touch input is fixed invisible screen regions (stick on the left half, face buttons bottom right) with no drawn overlay, no calibration and no gamepad auto-hide -- the Android overlay is Android-only. |
| macOS Apple Silicon / Intel | partial | Apple Silicon tested; the Intel job is `continue-on-error` in CI, so a release can ship without an Intel build and none has been run on Intel hardware. |
| PAL disc (GALP01) | partial | Experimental: USA game code on PAL data, English (UK) text, NTSC 60 Hz. Trophy tables are stubbed out rather than read, and there is no reference hash, so PAL images always verify as unknown. |
| Widescreen 16:9 / window aspect | partial | VS, Sudden Death and Training only; menus, results and cutscenes stay at the original 73:60. |
| Wide HUD anchoring | done | Separate on/off toggle from the aspect setting, and only moves anything while widescreen is on. Anchors the timer and the 2-4 player HUD groups (damage, stocks, tags); a 1-player HUD keeps its original placement. No configurable margins. |
| Custom texture packs (Dolphin format) | done | `tex1_*` `.dds` / `.png` including sidecar mips and TLUT hashes, scanned recursively, reloadable from the F1 menu. |
| Custom soundtrack (`.ogg` / `.wav`) | done | Replaces any track the game streams, not just stage BGM. Files are decoded whole into RAM (not streamed) and loop end to end, so a track's own loop point is ignored. |
| Unlock Everything / Frozen Stadium / Free camera | done | Cheats tab in the launcher and the F1 menu. |
| Multi-bus audio (Master / Music / SFX) | done | Three sliders; Master is the output stream gain, Music and SFX are per-voice. |
| In-app update check | done | Polls GitHub releases, downloads with progress. |
| Controller rumble | done | SDL gamepads through the game's own `PADControlMotor` calls, and the Android device vibrator when the pad has no rumble. Controller LED / port-colour sync is not implemented. |
| 1000 Hz GameCube adapter (WUP-028) | partial | Implemented and wired, not yet confirmed against a physical adapter. Raw 0x21 reports are read through SDL's hidapi on the 1000 Hz input thread, so the game would see the controller's real 8-bit values instead of SDL's rescaled ones; adapter slot N is PAD port N, and the slot motors are driven from the game's rumble state. `MELEE_GC_ADAPTER=0` hands the device back to SDL's driver. Linux needs a udev rule; the log prints it. |
| UCF (dashback, shield drop) | done | UCF 0.8x rules; launcher Gameplay page / F1 port menu, default off, `MELEE_UCF=1`. Reads the octagon-clamped stick rather than UCF's pre-clamp raw queue, which only differs past the 80-unit rim. |
| Discord Rich Presence | planned | Deferred until API credentials are available. |
| Extended hazardless stages | planned | Whispy, Randall, FoD platforms. Only Pokémon Stadium is implemented. |
| 2-player keyboard remapping | planned | The keyboard is port 1 on a fixed layout. |
| High-refresh interpolation | planned | |
| Training tools (hitboxes, savestates, frame advance) | planned | |
| Replay recording (`.slp`) | planned | `src/pc/slp.h` defines the hook points; nothing implements them. |
| Online play (rollback, DHT matchmaking) | planned | Not implemented. |
| RetroAchievements | planned | |

The phases behind the planned rows, and why they are ordered that way, are in
[ROADMAP.md](ROADMAP.md).

## Download

Builds for every platform are on the
[releases page](https://github.com/999sian/melee-pc/releases). Release notes
list the per-platform files, known issues and requirements.

```sh
./Melee-x86_64.AppImage                  # open the launcher
./Melee-x86_64.AppImage /path/to/melee.iso
```

**Melee USA revision 2 (NTSC-U 1.02, GALE01)** is the supported disc. A
**Europe (PAL, GALP01)** image also boots, with the limits listed in the
[status table](#status) and the mechanics in
[porting-notes.md](docs/porting-notes.md#regions). `.iso`,
`.gcm`, `.ciso` and `.rvz` images are accepted. A valid disc path on the
command line boots straight in; a missing or invalid one returns to the
launcher. Settings and the selected path live in `launcher.cfg` in SDL's
`melee-pc` preference directory (usually `~/.local/share/melee-pc`,
`%APPDATA%\melee-pc` on Windows, or `~/Library/Application Support/melee-pc`
on macOS).

Verification reads the disc through nod, compressed images included, and compares
SHA-1 against the
[Redump DAT](https://github.com/libretro/libretro-database/blob/master/metadat/redump/Nintendo%20-%20GameCube.dat):
`d4e70c064cc714ba8400a849cf299dbd1aa326fc`, 1,459,978,240 bytes. It supports
progress and cancellation, and is not cached between launches. Unverified images
still play; PAL images have no reference hash and always report as unverified.

Building from source: [docs/building.md](docs/building.md).

## Requirements

The renderer is WebGPU (Dawn) at its compatibility level, so the floor is
Dawn's per-backend floor:

| Platform | API tried, in order | Floor |
|---|---|---|
| Windows 10/11 (x86-64, ARM64) | Direct3D 12 → Direct3D 11 → Vulkan | Feature level 11_0. Dawn refuses D3D12 on Intel Gen7 (HD 4000/4400/4600, Ivy Bridge/Haswell); the intended fallback for those is Direct3D 11, which is untested on that hardware (see the status table). Vulkan 1.1 with a vendor ICD. |
| Linux (x86-64, aarch64) | Vulkan | Vulkan 1.1 (Mesa radv/anv/hasvk, NVIDIA proprietary or NVK). |
| macOS / iOS | Metal | Any Metal GPU; Apple Silicon tested, iOS 14+. |
| Android | Vulkan | Vulkan 1.1, arm64. |

On Windows that means any Intel Gen8 (Broadwell, 2014) or newer, AMD GCN or
newer, NVIDIA Fermi or newer runs on Direct3D 12. Direct3D 11 is a
compatibility path, not a performance one (FXC shaders, no DXC). OpenGL is not
built. The log records every backend that was skipped and why, then one summary
line with the adapter and driver.

- Keep `resources/` (and on Windows the DLLs: `webgpu_dawn.dll`,
  `dxcompiler.dll`, `dxil.dll`, `SDL3.dll`, the VC++ runtime) beside the
  executable. `dxcompiler.dll` and `dxil.dll` are the D3D12 shader compiler;
  D3D11 needs no extra DLL, since `d3d11.dll`, `dxgi.dll` and the FXC
  compiler are Windows components.
- Settings, memory cards, `music/` and `textures/` live in the `melee-pc`
  preference directory above.

## Controls

Keyboard: arrows or WASD = stick, IJKL = C-stick, X = A, Z = B, C = X, V = Y,
Q/E = L/R, Tab = Z, Enter = Start, TFGH = D-pad. Gamepads work through SDL; an
official GameCube adapter is read directly instead (see the status table).

| | Keyboard | Gamepad |
|---|---|---|
| Navigate | Up/Down, Tab | D-pad or left stick |
| Adjust | Left/Right | D-pad left/right |
| Change tab | Left/Right on the tab strip | L/R shoulders |
| Select | Enter | A |
| Close overlay | Escape, F1 | B, Start, Back |

## Settings overlay

**F1**, or Back/Select on a gamepad, opens the overlay. The game pauses while it
is open.

- Display: fullscreen/windowed and VSync apply immediately. `MELEE_VSYNC`
  overrides the saved preference.
- Internal resolution and UI scale are sliders. UI scale covers 75% to 150%.
- Post-processing picks the presentation shader and applies immediately.
- Anti-aliasing and anisotropic filtering apply on the next launch. MSAA offers
  only off and 4x because WebGPU guarantees sample counts 1 and 4.
- Audio: master volume, mute, FPS counter, all immediate.
- Controls remaps a gamepad. Pick the port, select a GameCube button, then press
  the physical button. Escape cancels, Restore resets the port. Back cannot be
  bound since it opens the menu. Sticks and triggers remap the same way, and a
  direction accepts either a stick axis or a button.

Melee's own menu sounds play in the overlay. Bindings are stored in aurora's
per-device `.controller` files; everything else shares `launcher.cfg`.

## Environment variables

| Variable | Effect |
|---|---|
| `MELEE_BACKEND=<name>` | Pin the graphics backend (`vulkan`, `d3d12`, `d3d11`, `metal`, ...) instead of the platform's preferred order; an unknown name lists the valid ones. |
| `MELEE_VSYNC=0\|1` | Override the saved VSync preference. |
| `MELEE_LOG_FILE=<path>` | Write the log to a file (default `melee-pc.log` beside `melee.exe` on Windows; empty disables). |
| `MELEE_WINDOW_TITLE=<t>` | Window title. |
| `MELEE_FILES_DIR=<dir>` | Loose-file overlay: files here (or in `./files/`) replace the disc's. |
| `MELEE_CACHE_MAX_MB=<n>` | In-memory archive cache budget (default picked from installed RAM). |
| `MELEE_PREWARM=0` | Skip the background asset pre-warm after boot. |
| `MELEE_FAST_FADES=1` | Clamp scene fade delays. |
| `MELEE_PIPELINE_JOBS=<n>` | Background shader-pipeline compile threads (default half the hardware threads, 1..8). |
| `MELEE_UCF=1` | Universal Controller Fix (UCF 0.8x dashback and shield-drop rules); overrides the `ucf` launcher.cfg pref. |
| `MELEE_GC_ADAPTER=0` | Hand the GameCube adapter (WUP-028) back to SDL's gamepad driver instead of reading it raw. |
| `--no-card` | Boot without a memory card. |
| `--dvd <image>` | Explicit form of the positional disc argument. |
| `--version` | Print the build version and exit. |

Diagnostic knobs (`MELEE_DEBUG`, `MELEE_FPS`, `MELEE_HEAP_CHECK`, the
`AURORA_*` draw filters, ...) are listed in
[docs/debugging.md](docs/debugging.md#diagnostic-environment-variables).

## Community

- [Discord](https://discord.gg/aurt34svq) for questions and testing.
- [Project site](https://999sian.github.io/melee-pc/) for setup, FAQ and
  known issues.
- [Bug report form](https://github.com/999sian/melee-pc/issues/new?template=bug_report.yml);
  attach the log (`melee-pc.log` on Windows, see
  [docs/debugging.md](docs/debugging.md#log-files)).

## Documentation

- [docs/building.md](docs/building.md) - toolchain, packaging, cross-compiling
  for Windows, Android, iOS and macOS.
- [docs/testing.md](docs/testing.md) - unit tests, drive/capture tools,
  port-bug harnesses.
- [docs/debugging.md](docs/debugging.md) - log files, crash handler, gdb, heap
  check, diagnostic environment variables.
- [docs/porting-notes.md](docs/porting-notes.md) - the big-endian data model,
  LP64 bug classes, PAL support.
- [docs/architecture.md](docs/architecture.md) - layers, threads, memory map,
  aurora.
- [CODING_STYLE.md](CODING_STYLE.md) - coding standards and verification
  procedure; run `python3 tools/check_style.py` before opening pull requests.
- [ROADMAP.md](ROADMAP.md) - scope and sequencing of the remaining phases. It
  carries no status; the [table above](#status) does.

## License

Three situations, spelled out in [LICENSE.md](LICENSE.md): the decompiled
game code in `src/melee` and `src/sysdolphin` is **not licensed** and remains
the property of its copyright holders; the port code in `src/pc`, `tools`,
`platforms`, `cmake` and `.github` is **GPL-3.0-or-later** ([COPYING](COPYING));
bundled third-party components keep their own licenses. Because the game code
cannot be relicensed, the repository as a whole is not distributable under the
GPL. No game assets are in this repository.
