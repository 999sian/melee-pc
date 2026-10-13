#!/usr/bin/env python3
"""Check low-address host buffers and GameCube ARAM offsets stay distinct."""
import json
from pathlib import Path
import subprocess
import tempfile
from net_test_support import build_dir, standalone_compile_args

root = Path(__file__).resolve().parent.parent
entries = json.loads((build_dir(root) / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/pc/file_cache.cpp"))
with tempfile.TemporaryDirectory(prefix="melee-file-cache-test-") as directory:
    exe = Path(directory) / "test"
    subprocess.run(standalone_compile_args(entry) + ["-UNDEBUG", "-ffunction-sections",
        "-fdata-sections", "-Wl,--gc-sections,--wrap=malloc,--wrap=free",
        str(root / "tools/test_file_cache.cpp"), "-o", str(exe)],
        cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
