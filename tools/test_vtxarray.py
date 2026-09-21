#!/usr/bin/env python3
"""Compile the real display-list scanner regression with UBSan."""
import json
import pathlib
import shlex
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
entries = json.loads((build_dir(root) / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/pc/vtxarray.c"))
args = shlex.split(entry["command"])
for option in ("-o", "-c"):
    index = args.index(option)
    del args[index:index + 2]
args = [a for a in args if a != "-DNDEBUG"]
with tempfile.TemporaryDirectory(prefix="melee-vtx-test-") as directory:
    exe = pathlib.Path(directory) / "vtx_test"
    subprocess.run(args + ["-UNDEBUG", "-fsanitize=undefined",
                          "-fno-sanitize-recover=undefined", "-no-pie",
                          str(root / "tools/test_vtxarray.c"), "-o", str(exe)],
                   cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
