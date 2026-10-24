#!/usr/bin/env python3
"""Exercise native field access without GameCube byte-offset overlays."""
import json
from pathlib import Path
import subprocess
import tempfile

from net_test_support import build_dir, standalone_compile_args

root = Path(__file__).resolve().parent.parent
entries = json.loads((build_dir(root) / "compile_commands.json").read_text())
with tempfile.TemporaryDirectory(prefix="melee-offsets-test-") as directory:
    for case, source in (("records", "gm/gm_181A.c"),
                         ("scope", "ft/kinds/ftCommon/ftCo_ItemScopeStart.c"),
                         ("block", "gr/grkinokoroute.c")):
        entry = next(e for e in entries if e["file"].endswith("/melee/" + source))
        exe = Path(directory) / case
        subprocess.run(standalone_compile_args(entry) + [
            "-UNDEBUG", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-no-pie", str(root / ("tools/test_gamecube_offsets_" + case + ".c")),
            "-o", str(exe)], cwd=entry["directory"], check=True)
        subprocess.run([str(exe)], check=True)
        print("PASS: GameCube offset regression: " + case, flush=True)
