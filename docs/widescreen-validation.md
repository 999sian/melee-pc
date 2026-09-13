# Widescreen validation — 2026-09-13

## Delivered

Opt-in widescreen for ordinary VS and sudden-death VS: **Original 4:3** (default),
**Widescreen 16:9** and **Auto (window aspect)**, selected from the F1 settings
menu and persisted as `widescreen` in `launcher.cfg`.

Widening is purely presentational. Every on-screen camera of an eligible scene gets
the same horizontal factor applied to both the submitted projection and the render
viewport, so each drawn pixel keeps its position relative to the frame centre and
only the field of view grows sideways. Nothing is stretched, the HUD keeps its
classic 4:3 geometry, and simulation state (camera bounds, blast zones, physics,
60 Hz timing) is untouched — `HSD_CObj` fields and camera queries keep original
semantics; only the matrix handed to `GXSetProjection` changes.

Scenes that are not eligible (menus, CSS/SSS, every non-VS mode) render in the
original aspect, centred, with the widescreen setting still remembered.

## Implementation notes

- `src/pc/widescreen.c`: aspect/fit maths, the shared widening factor, and the
  render viewport/scissor mapping. `src/pc/widescreen.h` is the whole API.
- `setupNormalCamera` (`cobj.c`) applies the mapping; `HSD_CObjEraseScreen` widens
  its erase rectangle by the same factor.
- The cameraless SIS text path (`hsd_3A76.c`) sets its own logical viewport and
  builds its ortho projection inline, so it is mapped into the centred
  original-aspect rectangle instead of being widened.
- `HSD_Init_803755A8` (`initialize.c`) now really ends the offscreen pass under
  `TARGET_PC`. The retail build leaves `current_render_pass` flagged offscreen
  after the first fighter shadow, which is harmless on GameCube (both camera
  setups agree at 640×480 with no field rendering) but meant every camera after
  the first shadow bypassed the PC screen-space path. Offscreen passes keep their
  original projections and viewports.
- Aurora's viewport policy is `STRETCH` whenever a widescreen mode is active
  (the port fits frames itself) and `FIT` in Original mode.

## Evidence

- `python3 tools/test_widescreen.py`: compiles `src/pc/widescreen.c` with the real
  build flags and checks fit maths (16:9, 21:9, portrait, zero-size), mode
  selection including invalid values, widening of both perspective and ortho
  screen cameras, the centred cameraless-text rectangle, offscreen passes being
  left alone, and the unsupported-scene fallback.
- `build/launcher_data_test` (via `tools/test_launcher.py`): `widescreen`
  round-trips for all three modes and malformed values fall back to Original.
- `python3 tools/test_launcher.py --disc <USA-rev2-ciso>`: nine cases passed,
  including `f1-menu`, which cycles the aspect control in-game and asserts the
  saved preference.
- `python3 tools/test_widescreen_runtime.py <disc> --aspect {0,1,2}`: real CPU-vs-CPU
  VS matches under GDB on Final Destination (31), Kongo Jungle (4), Yoshi's Story (8)
  and Big Blue (24), at 1280×720 and 1280×540 (21:9). Screenshots verified:
  wider field of view, no distortion, classic HUD placement, working shadows, and
  backgrounds still covering the frame.
- HUD geometry checked numerically: the damage panels start at logical x ≈ −225
  from centre in 4:3, 16:9 and 21:9 alike (303 px at 1280×720 4:3, 308 px at
  1280×720 16:9, 387 px at 1280×540) — identical placement, only the frame grows.
- Original mode is unchanged: pillarboxed 4:3 output, same HUD, same framing.
- CSS screenshot in 16:9 mode confirms ineligible scenes stay 4:3 and centred.
- Regression smoke tests after the render-pass fix:
  `tools/test_special_smash.py --mode camera --mode lightning` and
  `tools/test_stadium.py --mode home-run` passed.

## Known limitations

- Only ordinary VS and sudden-death VS widen. Classic/Adventure/All-Star, Special
  Smash, Stadium, menus and cinematics keep the original aspect.
- The HUD is not re-laid out for wide frames: it stays in the classic 4:3 area
  rather than spreading to the screen edges. HUD editing was deliberately left
  out of this delivery.
- Automatic mode never narrows below 4:3; narrower windows letterbox instead.
- Stage backgrounds were spot-checked on four stages, not all of them. Stages with
  flat backdrops sized for 4:3 could in principle reveal an edge at extreme widths.
- Frame interpolation and any HUD rework remain out of scope.
- The cameraless SIS text path is only reached by the boot/error message screen
  (`lb_0192.c`); its centred mapping is implemented and unit-tested but was not
  reproduced at runtime.

## Commands

```sh
cmake --build build
python3 tools/test_widescreen.py
python3 tools/test_widescreen_runtime.py <disc> --aspect 1 --width 1280 --height 720
python3 tools/test_widescreen_runtime.py <disc> --aspect 2 --width 1280 --height 540 --stage 8
python3 tools/test_launcher.py --disc <disc>
```

Screenshots and GDB logs are written to the `--output` directory (default
`/tmp/melee-widescreen`).
