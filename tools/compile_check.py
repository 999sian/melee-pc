#!/usr/bin/env python3
"""Syntax-check game sources with the exact flags the build uses.

usage: compile_check.py <file-or-dir>... [--full]

Compiles each .c file (recursively for directories) with -fsyntax-only in
parallel and prints diagnostics. Exit status is non-zero if any file fails.
"""
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLAGS = [
    "gcc", "-std=gnu11", "-fsyntax-only", "-fpermissive", "-Wno-all", "-Wno-extra",
    "-Werror=int-conversion", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
    "-fno-strict-aliasing",
    "-Wno-scalar-storage-order", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DAURORA", "-DAURORA_ENABLE_GX",
    "-include", str(ROOT / "src/pc/compat.h"),
    "-I", str(ROOT / "extern/aurora/include"), "-I", str(ROOT / "src"), "-I", str(ROOT / "src/sdk_include"),
]


def collect(args):
    files = []
    for a in args:
        p = Path(a)
        if not p.is_absolute():
            p = ROOT / p
        if p.is_dir():
            files += sorted(p.rglob("*.c"))
        else:
            files.append(p)
    return [f for f in files if "debugconsole_main" not in f.name]


def run(f):
    r = subprocess.run(FLAGS + [str(f)], capture_output=True, text=True)
    return f, r.returncode, r.stderr


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        args = ["src/melee", "src/sysdolphin"]
    files = collect(args)
    failed = 0
    with ThreadPoolExecutor() as ex:
        for f, rc, err in ex.map(run, files):
            if rc != 0:
                failed += 1
                sys.stdout.write(err)
    print(f"{len(files) - failed}/{len(files)} files OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
