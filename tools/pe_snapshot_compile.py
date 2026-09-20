#!/usr/bin/env python3
"""CMake compiler launcher: bracket eligible MinGW game statics for rollback.

Runs the real compiler first, then renames writable COFF object sections with
objcopy. PE's dollar-suffix ordering places them between A/Z boundary markers.
Relocations and zero-fill characteristics are preserved. Attach this launcher
only to melee_game, never engine/platform libraries. The exclusion list is read
from the existing ELF script so audio/worker-owned state cannot silently drift.

Usage: pe_snapshot_compile.py --objcopy TOOL --objdump TOOL -- COMPILER ARGS...
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def exclusions(script: Path) -> set[str]:
    groups = re.findall(r"EXCLUDE_FILE\(([^)]*)\)", script.read_text())
    if not groups:
        raise ValueError("snapshot exclusion list is missing")
    result = set(groups[0].split())
    if any(set(group.split()) != result for group in groups):
        raise ValueError("ELF snapshot sections disagree about excluded objects")
    if any(not name.startswith("*") or not name.endswith(".c.o") for name in result):
        raise ValueError("unsupported snapshot exclusion pattern")
    return {name[1:-2] for name in result}


def rewrite(obj: Path, objcopy: str, objdump: str) -> None:
    env = dict(os.environ, LC_ALL="C")
    listing = subprocess.check_output([objdump, "-h", str(obj)], text=True, env=env)
    if "file format pe-x86-64" not in listing:
        raise ValueError("Windows snapshots currently require a verified x86-64 PE object")
    names = re.findall(r"^\s*\d+\s+(\S+)\s+[0-9a-fA-F]+\s", listing, re.MULTILINE)
    if any(name.startswith(".gnu.lto_") for name in names):
        raise ValueError("LTO objects cannot be safely sectioned for Windows snapshots")
    if any(name.startswith((".mld$", ".mlb$")) for name in names):
        raise ValueError("object already contains reserved snapshot sections")
    arguments = []
    for name in names:
        for original, renamed in ((".data", ".mld$M"), (".bss", ".mlb$M")):
            if name == original or name.startswith((original + "$", original + ".")):
                arguments += ["--rename-section", name + "=" + renamed + name[len(original):]]
    symbols = subprocess.check_output([objdump, "-t", str(obj)], text=True, env=env)
    # COFF commons have section zero and positive value (their allocation size).
    # Undefined external references have value zero and must remain untouched.
    for line in symbols.splitlines():
        if re.search(r"\(sec\s+0\)", line):
            value = re.search(r"0x([0-9a-fA-F]+)\s+\S+\s*$", line)
            if value and int(value.group(1), 16):
                raise ValueError("COMMON allocation escaped snapshot sections; compile with -fno-common")
    if not arguments:
        return
    descriptor, temporary = tempfile.mkstemp(prefix=obj.name + ".snapshot-", dir=obj.parent)
    os.close(descriptor)
    try:
        subprocess.run([objcopy, *arguments, str(obj), temporary], check=True, env=env)
        os.replace(temporary, obj)
    finally:
        Path(temporary).unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--exclude-script", type=Path,
                        default=Path(__file__).resolve().parents[1] / "src/pc/melee_state.ld")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a compiler command is required after --")
    output = None
    try:
        compiled = subprocess.run(command)
        if compiled.returncode:
            return compiled.returncode
        if "-c" not in command:
            return 0
        sources = [Path(arg) for arg in command if arg.endswith(".c") and Path(arg).is_file()]
        if len(sources) != 1 or "-o" not in command:
            raise ValueError("expected a single C source and explicit -o object")
        output = Path(command[command.index("-o") + 1])
        if sources[0].name in exclusions(args.exclude_script):
            return 0
        rewrite(output, args.objcopy, args.objdump)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        # Never leave an unprocessed successful-looking object after failure.
        if output is not None:
            output.unlink(missing_ok=True)
        print(f"Windows snapshot sectioning failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
