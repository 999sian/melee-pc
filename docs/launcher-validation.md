# Launcher validation — 2026-09-13

## Delivered

RmlUi launcher, native SDL disc chooser and drag/drop handler, remembered absolute
disc paths, region/revision inspection, cancelable decoded-disc SHA-1 verification,
window/fullscreen and VSync settings, UI scaling, keyboard/gamepad navigation,
and startup recovery. Command-line disc paths still boot directly.

## Evidence

- `cmake --build build --target melee -j 6`: passed with RmlUi enabled.
- `cmake --build build --target launcher_data_test -j 6` and
  `build/launcher_data_test`: passed metadata rejection, missing paths, early and
  active verification cancellation, full-size hash mismatch, preference round-trip,
  malformed preference values, and failed writes.
- Local USA revision 2 CISO inspected successfully and its full decoded SHA-1
  matched Redump (`d4e70c064cc714ba8400a849cf299dbd1aa326fc`).
- `tools/test_launcher.py --disc <USA-rev2-ciso>`: seven cases passed: first-run
  keyboard navigation/scale persistence/Quit, invalid CLI recovery, missing saved
  disc recovery, fullscreen/VSync preference changes, UI game handoff, relative
  CLI path persistence, and window close during verification. Private preference
  directories and `--no-card` were used.
- Visual inspection: default launcher, errors, settings, 125% scale, and a
  720×760 window. Added explicit RmlUi block defaults and scrollbar styles after
  screenshots exposed layout defects. The final small/scaled check passed.
- Game handoff reached Melee's no-memory-card boot screen and closed cleanly;
  this is a boot smoke test, not a full match regression test.
- Read-only peer review found relative-path persistence and Play remaining enabled
  after a verification read failure. Both fixes were confirmed by the reviewer.
- `git diff --check`: passed.

## Outstanding acceptance checks

- Native chooser selection/cancellation: the forced Zenity test backend appeared
  blank/unresponsive in this shell environment. The opt-in check is retained as
  `tools/test_launcher.py --native-picker` for a functioning GTK/X11 desktop.
  No success is claimed for this check. Drag/drop has not been manually exercised.
- Physical controller input has not been tested. Keyboard navigation passed.
- RVZ/GCM support reuses nod but was not exercised with real files in this run;
  the real-disc verification check used CISO.

## Workspace

Changes remain uncommitted on `feat/rmlui-launcher` because Git author identity
is unset. Existing unrelated port changes were preserved.
