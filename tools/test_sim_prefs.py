#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every local preference the game code reads must be pinned for netplay.

A pc_is_*/pc_get_* call in src/melee or src/sysdolphin reads a setting of
this machine. If it steers the simulation and differs between two peers,
they desync on the first frame it matters (UCF did exactly this before it
was forced on in sessions). So each one is listed here as either pinned (its
getter returns an agreed value while pc_net_deterministic() is true) or
presentation (it changes what is drawn, never sim state). A new call fails
this test until someone decides which it is."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

PINNED = {
    "pc_is_ucf_enabled": "forced on in launcher.cpp",
    "pc_is_free_camera_enabled": "forced off in launcher.cpp",
    "pc_is_unlock_all_enabled": "RULES (pc_net_rules)",
    "pc_is_frozen_stadium_enabled": "RULES (pc_net_rules)",
}
PRESENTATION = {
    "pc_get_hud_mode": "widescreen HUD placement (ifall.c)",
    "pc_get_net_target": "prefills the connect-code entry (gmonlinemode.c)",
}

found = {}
for d in ("src/melee", "src/sysdolphin"):
    for path in (ROOT / d).rglob("*.c"):
        for m in re.finditer(r"\b(pc_(?:is|get)_[a-z0-9_]+)\s*\(", path.read_text(errors="replace")):
            found.setdefault(m.group(1), path.relative_to(ROOT))

unknown = sorted(n for n in found if n not in PINNED and n not in PRESENTATION)
for n in unknown:
    print(f"FAIL: {n} (first in {found[n]}) is neither pinned nor presentation-only")
if unknown:
    sys.exit(1)

# The pinned getters must actually consult the session.
launcher = (ROOT / "src/pc/launcher.cpp").read_text()
for name in ("pc_is_ucf_enabled", "pc_is_free_camera_enabled"):
    body = re.search(r'extern "C" bool ' + name + r"\(void\) \{(.*?)\n\}", launcher, re.S)
    if not body or "pc_net_deterministic()" not in body.group(1):
        print(f"FAIL: {name} no longer checks pc_net_deterministic()")
        sys.exit(1)
print(f"test_sim_prefs: ok ({len(found)} getters, all classified)")
