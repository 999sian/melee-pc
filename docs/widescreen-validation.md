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
- `setupNormalCamera` (`cobj.c`) divides the projection's whole horizontal row
  by that factor — `p[0][0]` and the offset term (`p[0][2]` for perspective and
  frustum cameras, `p[0][3]` for ortho). Dividing `p[0][0]` alone is only a
  centre-symmetric scale when the projection is already centred; every ortho
  HUD camera and every asymmetric frustum keeps its horizontal offset in that
  second term, and leaving it alone shifted those images sideways. The symptom
  was the P1/P2/CP nametags drifting ~200px left of their fighters at 16:9.
  `HSD_CObjEraseScreen` widens its erase rectangle by the same amount.
- **The fill-frame opt-out.** A camera whose projection box *is* the screen
  rectangle is geometrically indistinguishable from a HUD camera — only intent
  separates them, so intent is recorded explicitly. `PC_COBJ_FILL_FRAME`
  (`src/pc/widescreen.h`, bit 29 of `HSD_CObj::flags`; 0, 1, 30 and 31 are
  taken, and `CObjLoad` only reseeds the low bits from the disc desc, so a bit
  set after `HSD_CObjLoadDesc` survives) marks a camera that must cover the
  whole frame. `pc_widescreen_cobj_scale(cobj)` returns 1.0 for such a camera
  and `pc_widescreen_scale()` otherwise; everything that has to agree with the
  submitted matrix asks that one function, so there is a single scale factor in
  the port, not one per effect.
  - **Widens:** the world, stage backgrounds, effects, items — everything drawn
    through a normal scene camera.
  - **Deliberately does not widen:** the HUD (damage panels, nametags, stock
    icons, timer). Those cameras stay unflagged, which is exactly what keeps
    their classic 4:3 layout. Fighter shadows likewise: they run in
    `HSD_RP_OFFSCREEN`, where the scale is already 1.
  - **Flagged fill-frame:** the `lbbgflash` overlay camera (`lbl_803BB028`),
    whose only purpose is to make the plane z=0 the screen rectangle for the
    full-screen flash and fade-to-black quad. Before the flag, every KO fade
    and every heavy-hit element flash covered only ndc.x [−1/s, +1/s] — a live
    strip of gameplay 12.5% of the frame wide down each edge at 16:9, 21% at
    21:9. Widening the quad instead was rejected: 0..640 means "the screen" in
    both builds, and a baked-in factor would break the moment the eye position
    or fov animates.
- **`HSD_CObjEraseScreen` scales its rect about the rect's own centre**, not
  about the world origin. `left *= s; right *= s` is only a centre-symmetric
  scale when `left == -right`; for the Pokémon Stadium text-window camera
  (ortho 0..250) it also translated the rect, leaving the left 12.5% unerased
  and overhanging the right edge by 25% at 16:9. Symmetric frusta are
  bit-identical to before.
- **Projected-texture matrices are built from the projection that was actually
  submitted.** `C_MTXLightPerspective` sets `m[0][0] = scaleS·cot/aspect`, so
  passing `aspect · pc_widescreen_cobj_scale(cobj)` reproduces the rendered
  `p[0][0]` exactly; the `transS` term in `m[0][2]` is untouched. Fixed in
  `lbrefract.c` (refraction / heat-haze: Cloaking Device, Special Smash
  Invisible) and `grizumi.c` (Fountain of Dreams water reflection), which were
  sampling displaced by a factor of s about the frame centre. `lbrefract`'s
  frustum and ortho arms widen `left`/`right` about their midpoint, the same
  rule as the erase rect.
- **EFB captures that a model re-draws with fixed UVs copy the centred `1/s`
  rect.** `HSD_ImageDescCopyFromEFB` passes `idesc->width/height` as both src
  and dst, and Aurora's `copy_tex` then sizes the texture from the *mapped*
  render-target rect — `round(w·s) × h` — while the game declares `w × h`, so
  the image arrives squashed by s. `pc_widescreen_copy_efb()` copies the
  centred rect of width `w/s` instead, which is precisely the region the
  widened projection drew the 4:3 picture into; keeping src == dst there makes
  Aurora resolve it 1:1 rather than resample. Used by the off-screen-fighter
  magnifier (`ifmagnify.c`) and all three Pokémon Stadium captures
  (`grpstadium.c`: jumbotron feed, text window, vision sub-rect). The
  sub-rect capture contracts about the frame centre (320) rather than its own,
  because that is the centre its source camera widened about.
  - The alternative — making Aurora produce a logical-sized texture — was not
    taken: `copy_tex` cannot tell a capture that will be re-drawn with fixed
    UVs from one that is projected back with a matrix (`lbrefract`,
    `grizumi`), and the latter is correct as-is. The knowledge lives at the
    call site.
  - Not affected: `lbrefract`'s own copy (src 640×480, dst 320×240 takes
    Aurora's uniform-scale path) and `grizumi`'s reflection copy (sampled
    projectively, so the texture's pixel dimensions never matter).
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
- Screen-effect NDC-coverage probe (`/tmp/ws-gaps/probe_fix.c`): links Aurora's
  real `C_MTXPerspective` / `C_MTXOrtho` / `C_MTXLookAt` /
  `C_MTXLightPerspective` and replays `setupNormalCamera`'s widening,
  `HSD_CObjEraseScreen`'s compensation and Aurora's EFB-copy mapping, scoring
  before and after in one run. At 4:3 / 16:9 / 21:9:

  | effect | before | after |
  | --- | --- | --- |
  | `lbbgflash` flash/fade quad | ndc.x ±0.75 / ±0.5714 | ±1.0 at every aspect |
  | `EraseScreen`, ortho 0..250 | [−0.75, +1.25] / [−0.5714, +1.4286] | [−1.0, +1.0] |
  | `EraseScreen`, symmetric ±300 | [−1.0, +1.0] | unchanged |
  | HUD ortho 0..640 (control) | ±0.75 / ±0.5714 | **unchanged** |
  | magnifier fighter width | ×0.75 / ×0.5714 | ×1.0 |
  | `MTXLightPerspective` vs render | ×1.3333 / ×1.75 | ×1.0 |
  | jumbotron horizontal scale | ×0.75 / ×0.5714 | ×1.0 |

  Injection-validated as the audit was: one deliberately-broken row per aspect
  moves the flag count 0 → 3 when scoring the fixed column and 10 → 13 when
  scoring the original column, and the 10 original flags are exactly the five
  defect rows at the two widened aspects — the HUD and symmetric-frustum
  controls never flag.

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
- Two screen-space paths in files outside this change are still 4:3-only, and
  neither is reachable in a widening scene today: `lbspdisplay.c`'s
  EFB-capture replay camera (`lb_800138EC`, ortho 0..640) would need the same
  `PC_COBJ_FILL_FRAME` flag, and `cmsnap.c`'s camera-mode snapshot allocates a
  640×480 buffer for a copy Aurora sizes wider. Both surface immediately if
  either is ever used during a fight.

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
