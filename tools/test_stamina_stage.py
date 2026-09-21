#!/usr/bin/env python3
"""Compile the actual Stamina stage-exit path; stub only audio and scene routing."""
import json
from pathlib import Path
from net_test_support import build_dir
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
entries = json.loads((build_dir(root) / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/gm/gmstamina.c"))
args = shlex.split(entry["command"])
for option in ("-o", "-c"):
    index = args.index(option)
    del args[index:index + 2]
args = [a for a in args if a != "-DNDEBUG"]
with tempfile.TemporaryDirectory(prefix="melee-stamina-test-") as directory:
    exe = Path(directory) / "stamina_test"
    subprocess.run(args + ["-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                          "-Wl,--gc-sections", "-no-pie", str(root / "tools/test_stamina_stage.c"),
                          "-o", str(exe)], cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
