#!/usr/bin/env python3
"""Compile every game TU with `-m32 -DLINT` and report failing size/offset asserts.

The decomp's ASSERT_SIZE / ASSERT_OFFSET are no-ops on TARGET_PC (they are
gated on MUST_MATCH || LINT). Under `-m32 -DLINT` they become real checks
against the GameCube ABI, so a failure means the struct *reconstruction* is
wrong, independently of anything the port changed. Expected failures are
listed in EXPECTED below; anything else is new.

usage: lint_sweep.py [build-dir]   (default: build)
"""
import json
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# Deliberate divergences, not reconstruction errors.
EXPECTED = {
    # Fighter carries an extra `throw_thrower` pointer: the throw victim has to
    # outlive the damage view and no position inside the motion-var union
    # survives on LP64. See src/melee/ft/kinds/ftCommon/types.h.
    "sizeof(struct Fighter) == 0x23EC",
}


def tu_command(entry):
    argv = shlex.split(entry["command"])
    out = []
    i = 0
    while i < len(argv):
        if argv[i] == "-o":
            i += 2
            continue
        if argv[i] in ("-c", "-Wno-all", "-Wno-extra"):
            i += 1
            continue
        out.append(argv[i])
        i += 1
    return out + ["-m32", "-DLINT", "-fsyntax-only", "-fdiagnostics-color=never"]


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
    db = json.loads((build / "compile_commands.json").read_text())
    game = [e for e in db if "/src/melee/" in e["file"] or "/src/sysdolphin/" in e["file"]]

    def run(entry):
        proc = subprocess.run(tu_command(entry), cwd=entry["directory"],
                              capture_output=True, text=True)
        return entry["file"], proc.stderr

    found = {}
    with ThreadPoolExecutor(16) as pool:
        for path, err in pool.map(run, game):
            for line in err.splitlines():
                m = re.search(r'static assertion failed: "\((.*?)\) failed"', line)
                if m:
                    found.setdefault(m.group(1), path)

    unexpected = {k: v for k, v in found.items() if k not in EXPECTED}
    print(f"{len(game)} TUs, {len(found)} failing asserts "
          f"({len(unexpected)} unexpected)")
    for assertion, path in sorted(unexpected.items()):
        print(f"  {assertion}\n    first seen in {path}")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
