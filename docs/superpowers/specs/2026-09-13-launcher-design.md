# Melee-PC launcher and disc handling

## Agreed direction

Add a Dusklight-inspired RmlUi launcher to melee-pc, using its existing Aurora
runtime. The user approved the launcher, controller navigation, settings,
remembered disc selection, verification progress, and recovery from invalid
saved paths. Target Linux, matching the current port.

## User experience

Starting without arguments opens a Melee-styled launcher in the game window.
Show Play, Choose Disc, Verify Disc, Settings, and Quit. Display selected image
name, game region/revision, and verification status. Use a dark background,
clear contrasting text, visible focus, and restrained Melee-inspired accents.
Support mouse, keyboard, and controller focus/activation. File selection uses
the native SDL dialog; cancellation preserves the previous selection.

Settings initially expose window/fullscreen, vsync, and UI scale. Persist these
alongside the disc path in the existing SDL preference directory. Existing
explicit environment overrides retain precedence where applicable.

## Startup and integration

Enable AURORA_ENABLE_RMLUI before adding Aurora in CMake. Add a C++ launcher
module with a small C-compatible entry point used by src/pc/main.c. RML/RCSS
and a redistributable font are packaged with the executable and resolved
independently of the working directory. Reuse Aurora's UI context, event pump,
and frame lifecycle; do not add a second window or renderer.

Initialize Aurora, run the launcher, open the accepted disc, close launcher
documents, then initialize the game platform and enter melee_main. Keep
shutdown safe when quitting before game initialization. Do not forward
launcher input into the game or leave a launcher frame open at handoff.

Preserve positional disc paths, --dvd, --no-card, and existing automated
launches. A valid explicit disc path launches directly after metadata checks;
an invalid path opens the launcher with an actionable error. With no explicit
path, show the launcher with the remembered selection.

## Disc handling

Use the existing disc decoding backend for ISO/GCM/CISO/RVZ. Accept Melee
NTSC-U 1.02 (GALE01, revision 2); distinguish wrong game, unsupported revision,
unreadable file, and malformed image. Do not infer validity from the extension.
Read assets directly through Aurora rather than requiring extraction.

Separate quick metadata inspection from full verification. Hash the decoded
logical disc, not compressed container bytes. The implementation must establish
a sourced reference hash and compatible verification API before enabling a
verified label; an arbitrary local image is not a trusted reference. Dusklight's
Borealis-based verifier is a reference, not a reason to replace Aurora's backend.

Run full verification off the UI thread with progress and cancellation. A
failed or canceled attempt does not replace the active disc. Never persist a
successful verification as indefinitely valid based only on a path: invalidate
it when file identity/size/modification metadata changes and re-inspect at startup.
Metadata-compatible images may launch with an explicit unverified status;
wrong game/version or failed reads cannot launch. A hash mismatch remains
clearly visible and is never represented as success.

Missing saved images return to selection. Persist settings atomically and
report save errors without crashing. Join verification workers before teardown;
callbacks must not access destroyed UI objects.

## Validation

Build the existing melee target with RmlUi enabled. Check startup without a
disc, dialog cancellation, wrong game/revision, corrupt and missing images,
paths containing spaces, preference reload, and launch through both UI and CLI.
Exercise verification cancellation/mismatch and close-during-verification.
Verify keyboard/controller focus, resize/fullscreen, and visual legibility in
the running launcher. Smoke-test a game boot and gameplay handoff, preserving
the existing card flag and shutdown behavior. Test disc metadata and preference
error handling with small fixtures where practical. Record unavailable UI or
controller checks honestly rather than claiming acceptance from a build alone.
