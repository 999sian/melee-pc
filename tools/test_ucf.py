#!/usr/bin/env python3
"""Compile and run the UCF dashback / shield drop decision test."""
import json
from pathlib import Path
from net_test_support import build_dir
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
entries = json.loads((build_dir(root) / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/ftCommon/ftCo_Turn.c"))
args = shlex.split(entry["command"])
for option in ("-o", "-c"):
    index = args.index(option)
    del args[index:index + 2]
args = [a for a in args if a != "-DNDEBUG"]
with tempfile.TemporaryDirectory(prefix="melee-ucf-test-") as directory:
    exe = Path(directory) / "ucf_test"
    subprocess.run(args + ["-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                          "-Wl,--gc-sections", "-no-pie", str(root / "tools/test_ucf.c"),
                          "-o", str(exe)], cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
