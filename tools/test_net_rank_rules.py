#!/usr/bin/env python3
"""Compile the actual online ranked match-setup callbacks."""
import json
from pathlib import Path
from net_test_support import standalone_compile_args
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
entries = json.loads((root / "build/compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/gm/gmonlinemode.c"))
args = standalone_compile_args(entry)
with tempfile.TemporaryDirectory(prefix="melee-rank-rules-test-") as directory:
    exe = Path(directory) / "net_rank_rules_test"
    subprocess.run(args + ["-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                          "-Wl,--gc-sections", "-no-pie", str(root / "tools/test_net_rank_rules.c"),
                          "-o", str(exe)], cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
