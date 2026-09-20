#!/usr/bin/env python3
"""Compile the actual magnifier damage predicate independent of rendering."""
import json
import sys
from pathlib import Path
from net_test_support import standalone_compile_args
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
entries = json.loads((root / "build/compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/if/ifmagnify.c"))
args = standalone_compile_args(entry)
if "--old" in sys.argv: args.append("-DTEST_OLD_PREDICATE")
with tempfile.TemporaryDirectory(prefix="melee-magnify-test-") as directory:
    exe = Path(directory) / "net_magnify_test"
    subprocess.run(args + ["-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                          "-Wl,--gc-sections", "-no-pie", str(root / "tools/test_net_magnify.c"),
                          "-o", str(exe)], cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
