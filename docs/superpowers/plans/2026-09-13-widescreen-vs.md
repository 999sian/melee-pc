# Widescreen VS Implementation Plan

**Goal:** Add opt-in 16:9 and automatic window-aspect VS rendering with original gameplay and centred classic HUD.
**Architecture:** Keep simulation camera objects untouched. Apply one horizontal factor to both the submitted projection and the render viewport of every on-screen camera in an eligible scene, so drawing stays undistorted and the HUD keeps its classic geometry; fit everything else into an original-aspect viewport. Use Aurora's implemented viewport policy/render viewport APIs rather than its unimplemented aspect-lock declarations.
**Tech stack:** C game/platform code, C++ RmlUi settings, Aurora GX, existing Python/GDB runtime harness.
**Spec:** ../specs/2026-09-13-presentation-enhancements-design.md (first delivery only).

## Constraints

- Original 4:3 is the default and compatibility reference.
- Physics, blast zones, simulation camera bounds and 60 Hz timing remain unchanged.
- Widen only ordinary VS and sudden-death VS world rendering; unsupported scenes use their original aspect.
- Offscreen passes retain their original projections and viewports.
- Preserve the existing workspace's extensive uncommitted port changes; do not commit unrelated work.

## Tasks

- [x] Add `src/pc/widescreen.h` and `widescreen.c`: mode/scene state, aspect-fit maths, render viewport/scissor adjustment, shared horizontal multiplier. Test Original, 16:9, Auto, 21:9, narrow windows, invalid dimensions, offscreen passes and the centred cameraless-text rectangle in `tools/test_widescreen.c` with a runner using actual build flags.
- [x] Set scene eligibility from `gm_801A4B88` using `GM_VS` and `GS_VS`/`GS_SUDDEN_DEATH`. Apply rendering adjustments inside `setupNormalCamera`, and widen its erase rectangle consistently. Use native render sizes from Aurora, leaving logical GC viewport and all source camera fields intact.
- [x] End the shadow offscreen pass for real on PC (`HSD_Init_803755A8`); retail leaves `current_render_pass` offscreen for the rest of the frame, which bypassed the screen-space path for every camera after the first fighter shadow.
- [x] Add a persisted `widescreen` enum (0 Original, 1 16:9, 2 Auto) to launcher preferences and an F1 aspect-ratio control. Keep its setting independent of scene eligibility. Validate malformed preferences and round-trip all modes using the existing preferences test.
- [x] Extend the GDB match harness for ordinary VS with selectable stages and widescreen configuration. Run real matches and inspect screenshots in 4:3, 16:9 and ultrawide across four stages; confirm camera fields stay original, HUD placement is pixel-identical and ineligible scenes fall back. Run build, focused maths/preferences checks, live F1 test and scene regression smoke tests.
- [x] Document implemented scope, known limitations found during runtime checks, commands and screenshots (`docs/widescreen-validation.md`). Keep later HUD editing and interpolation out of this implementation.

## Maths contract

Fit requested aspect A into render width W and height H:
`height = min(H, W / A); width = height * A; left = (W-width)/2; top = (H-height)/2`.
In an eligible scene every on-screen camera uses that rectangle and divides the
submitted `projection[0][0]` by `presented_aspect / original_aspect`; vertical
terms are unchanged, so pixels keep their position relative to the frame centre.
Cameras outside eligible scenes, and the cameraless text path, use the original
aspect fitted inside that rectangle. Logical sub-viewports/scissors map
proportionally. Automatic aspect never narrows below the original aspect.
Zero-sized frames and offscreen passes produce no override.
