# Native presentation enhancements — design for review

## Goal

Give Melee-PC a coherent presentation experience: correct widescreen gameplay,
configurable HUD placement, and smooth high-refresh rendering while preserving
the original 60 Hz gameplay simulation. These are proposed features, not claims
about the current build. The user selected presentation as the priority.

The F1 menu now follows Dusklight's translucent toolbar, floating settings
window, compact controls and scrolling content, with a Melee-inspired navy,
silver-blue and orange palette. It remains
RmlUi-based and uses the existing saved preferences.

## Why a native implementation

Dolphin already provides resolution scaling, anti-aliasing, filtering and
widescreen patches. Our target is integration and correctness across the game:
camera framing, HUD anchors and interpolation can use named game objects and
scene transitions directly. No performance or latency advantage is assumed;
those require measurements against comparable Dolphin settings.

## 1. True widescreen match presentation

Offer Original 4:3, 16:9 and Auto/window aspect. Preserve vertical framing and
extend the horizontal view. Apply this to identified gameplay cameras, not all
perspective matrices: trophies, portraits, render-to-texture effects and menu
cameras must keep their own projection conventions. Original 4:3 is the default
and compatibility reference.

Aurora provides VILockAspectRatio, VIUnlockAspectRatio and framebuffer sizing.
Melee's HSD camera projection is constructed in baselib/cobj.c. Changing the
framebuffer alone is insufficient. Trace the match camera and culling consumers
before changing its effective presentation aspect. Keep gameplay camera bounds,
blast zones, physics and collision calculations at their original semantics.
Do not write presentation transforms back into simulation state.

Menus, movies and unsupported scenes retain their intended aspect with
pillarboxing. Effects that use screen coordinates need the same presentation
viewport as the scene they decorate. Validate offscreen indicators and stage
edges so widening the camera does not expose missing geometry unexpectedly.

First deliverable: validated VS match widescreen, with explicit scene fallbacks.
Broaden scene coverage only after that vertical slice is correct.

## 2. HUD layout

Provide Classic (original 4:3 safe area) and Wide (safe margins within the wider
viewport), plus HUD scale and safe-margin controls. Start with stocks, damage
percentages and timer; preserve their aspect and legibility. Anchor positions in
presentation coordinates without modifying underlying game values. Keep menu
and world-space text separate from the match HUD transformation.

Reset to Classic must restore the original layout immediately. Defaults retain
the original experience. Arbitrary drag-and-drop HUD editing is outside this
first implementation.

## 3. High-refresh interpolation

Provide Off and display-refresh-targeted presentation, initially validated at
120 and 144 Hz. Simulation, inputs, hitlag, timers and audio callbacks stay at
60 Hz. Capture previous/current render transforms and camera state at simulation
boundaries; render intermediate poses without executing another gameplay tick.

Aurora's public APIs inspected here provide frame submission and presentation
facilities, not a ready-made Melee object interpolation system. Dusklight also
has game-specific camera, line and replacement interpolation code, and uses
Borealis for its presentation preference. Its code is a reference, not a drop-in
switch for this port.

Identify render traversal side effects before rendering more than once per tick.
Use object-lifetime identities to prevent interpolation across reused objects.
Reset history on scene changes, teleports, respawns, camera cuts and object
creation/destruction. Hold discrete sprite/UI state where blending is incorrect.
Particles, trails and texture animation need separate validation. Maintain a
working Off path for visual artifacts and comparison.

## Acceptance checks

- Compare 4:3 against the current build using identical saved settings and inputs.
- Check 16:9, 21:9, resizing, fullscreen and portrait/narrow-window fallback.
- Test contrasting stages: Final Destination, Battlefield, Dream Land, Fountain
  of Dreams and a scrolling/moving stage. Check particles, shields, magnifiers,
  damage/stock UI, timer and results transitions.
- Check title, character/stage select, movies, trophies and training for correct
  scene fallback instead of blanket stretching.
- Compare simulation ticks, game timers and reproducible input sequences at
  60/120/144 Hz presentation; game outcomes must not depend on render frequency.
- Measure frame pacing, CPU/GPU time and input-to-display latency with equivalent
  resolution/AA and display mode. Report limits; do not claim a Dolphin advantage
  from an FPS counter alone.
- Verify settings persistence, reset-to-default, F1 navigation and input isolation.

## Delivery order

Implement and verify widescreen VS with scene fallback first. Add HUD anchors
second. Implement interpolation only after the simulation/render separation has
been demonstrated. Each step must remain usable with the later steps absent.
