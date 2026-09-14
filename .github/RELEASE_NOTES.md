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
| Windows x86-64 | `Melee-Windows-x86_64.zip` | Extract and run `melee.exe`. Keep the DLLs and `resources/` beside it. |
| Android arm64 | `Melee-Android-arm64-debug.apk` | Debug-signed. |

Launch with no arguments to open the launcher and pick a disc, or pass the
image path directly:

```sh
./Melee-x86_64.AppImage /path/to/melee.iso
```

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
