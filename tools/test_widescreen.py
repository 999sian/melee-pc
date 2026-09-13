#!/usr/bin/env python3
"""Compile the actual widescreen aspect and viewport maths."""
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
entries = json.loads((root / "build/compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/pc/vi.c"))
args = shlex.split(entry["command"])
for option in ("-o", "-c"):
    index = args.index(option)
    del args[index:index + 2]
args = [a for a in args if a != "-DNDEBUG"]
with tempfile.TemporaryDirectory(prefix="melee-widescreen-test-") as directory:
    exe = Path(directory) / "widescreen_test"
    subprocess.run(args + ["-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                          "-Wl,--gc-sections", "-no-pie", str(root / "tools/test_widescreen.c"),
                          "-lm", "-o", str(exe)], cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
