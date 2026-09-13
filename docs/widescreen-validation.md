# Widescreen validation — 2026-09-13

## Delivered

Opt-in widescreen for every fight: **Original 4:3** (default), **Widescreen 16:9**
and **Auto (window aspect)**, selected from the F1 settings menu and persisted as
`widescreen` in `launcher.cfg`. VS, sudden death, Training, and the fights of
Classic, Adventure, All-Star, Event, Stadium and Special Smash all widen, because
every mode fights in a `GS_VS` scene. Menus, results screens and cutscenes keep
the original aspect, with the setting remembered.

Widescreen is a framebuffer-shape change, not a viewport trick. Aurora sizes the
content framebuffer to the presented aspect and letterboxes it inside the window,
so logical 640×480 always maps onto the whole framebuffer with no offset; every
mapping Aurora derives from that — viewports, scissors and EFB copy regions —
stays consistent. The game's only contribution is dividing the submitted
horizontal projection term by the same factor, which keeps each drawn pixel where
it was relative to the frame centre: the field of view grows sideways, nothing is
stretched, and the HUD keeps its classic 4:3 geometry.

Simulation state (camera bounds, blast zones, physics, 60 Hz timing) is untouched.
`HSD_CObj` fields and camera queries keep original semantics; only the matrix
handed to `GXSetProjection` changes.

## Implementation notes

- `src/pc/widescreen.c` is the whole feature: pick the presented aspect from the
  mode, scene eligibility and window size; hand it to Aurora once per frame; and
  report the widening factor. No viewport or scissor overrides.
- `setupNormalCamera` (`cobj.c`) divides `p[0][0]` by that factor;
  `HSD_CObjEraseScreen` widens its erase rectangle by the same amount.
- `AuroraSetPresentationAspect` / `AuroraGetWindowSize` (new) generalise Aurora's
  4:3-only `set_frame_buffer_aspect_fit` into an arbitrary framebuffer aspect.
- `HSD_Init_803755A8` (`initialize.c`) now really ends the offscreen pass under
  `TARGET_PC`. The retail build leaves `current_render_pass` flagged offscreen
  after the first fighter shadow, which is harmless on GameCube (both camera
  setups agree at 640×480 with no field rendering) but meant fighter shadows and
  every later camera were classified wrongly on PC.

An earlier attempt letterboxed inside a stretched framebuffer using
`GXSetViewportRender`/`GXSetScissorRender`. It rendered correctly only when the
letterbox happened to be empty: whenever the window aspect differed from the
requested aspect (for example 16:9 in a 2880×1671 window), the offset was invisible
to Aurora's EFB-copy mapping, the projected shadow copy sampled the wrong region
and stage floors turned black. Sizing the framebuffer removes the offset entirely.

## Evidence

- `python3 tools/test_widescreen.py`: compiles `src/pc/widescreen.c` with the real
  build flags and checks the requested aspect and resulting framebuffer for
  Original, 16:9 (in wider, equal and narrower windows), Auto (ultrawide and
  portrait), invalid modes, zero-size windows, ineligible scenes, and that
  offscreen passes are never widened.
- `python3 tools/test_widescreen_runtime.py <disc> --aspect {0,1,2} [--mode N]`:
  real CPU-vs-CPU matches under GDB at 4:3, 16:9 and 21:9, on Final Destination,
  Kongo Jungle, Yoshi's Story and Big Blue, in VS and in Giant Melee (a non-VS
  game mode). Screenshots verified: wider field of view, no distortion, classic
  HUD, intact stage floors and working shadows.
- The configuration that previously produced black floors (2880×1671 window,
  16:9 requested) now renders correctly, letterboxed top and bottom.
- HUD geometry is unchanged across aspects: the damage panels start at the same
  logical offset from the frame centre in 4:3, 16:9 and 21:9.
- Original mode is unchanged: pillarboxed 4:3, same HUD, same framing.
- `python3 tools/test_launcher.py --disc <disc>`: nine cases passed, including
  `f1-menu`, which cycles the aspect control in-game and asserts the saved
  preference; `build/launcher_data_test` round-trips all three modes and rejects
  malformed values.
- Regression smoke tests after the render-pass and Aurora changes:
  `tools/test_special_smash.py --mode camera --mode giant` and
  `tools/test_stadium.py --mode home-run` passed.

## Known limitations

- Menus, CSS/SSS, results screens and cutscenes keep 4:3. Only scenes that are
  actual fights widen.
- The HUD is not re-laid out for wide frames: it stays in the classic 4:3 area
  rather than spreading to the screen edges. HUD editing was deliberately left
  out of this delivery.
- Automatic mode never narrows below 4:3; narrower windows letterbox instead.
- Stage backgrounds were spot-checked on four stages, not all of them. Stages with
  flat backdrops sized for 4:3 could in principle reveal an edge at extreme widths.
- Scene transitions resize the framebuffer when eligibility changes; a widened
  frame may persist for a frame at the boundary.
- Frame interpolation and any HUD rework remain out of scope.

## Commands

```sh
cmake --build build
python3 tools/test_widescreen.py
python3 tools/test_widescreen_runtime.py <disc> --aspect 1 --width 1280 --height 720
python3 tools/test_widescreen_runtime.py <disc> --aspect 2 --width 1280 --height 540 --stage 8
python3 tools/test_widescreen_runtime.py <disc> --aspect 1 --mode 30 --stage 8
python3 tools/test_launcher.py --disc <disc>
```

Screenshots and GDB logs are written to the `--output` directory (default
`/tmp/melee-widescreen`).
