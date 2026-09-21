#!/usr/bin/env python3
"""Compile the actual online stage selector; stub only shared seed and stage switches."""
import json
from pathlib import Path
from net_test_support import build_dir, standalone_compile_args
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
entries = json.loads((build_dir(root) / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/mn/mnstagesel.c"))
args = standalone_compile_args(entry)
with tempfile.TemporaryDirectory(prefix="melee-net-stage-test-") as directory:
    exe = Path(directory) / "net_stage_test"
    subprocess.run(args + ["-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                          "-Wl,--gc-sections", "-no-pie", str(root / "tools/test_net_stage.c"),
                          "-o", str(exe)], cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
