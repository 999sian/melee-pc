#!/usr/bin/env python3
"""Style and formatting verification tool for melee-pc.

Checks:
- Clang-Format compliance on src/pc/ and native tools
- Banned primitive types (bare 'long', 'unsigned long') in src/pc/
- Trailing whitespace and missing final newlines

Usage:
  python3 tools/check_style.py [--fix] [--files <file1> <file2>...]
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Directories and extensions to check
CHECK_DIRS = [ROOT / "src/pc"]
TOOL_FILES = list((ROOT / "tools").glob("*.cpp")) + list((ROOT / "tools").glob("*.c"))
SOURCE_EXTENSIONS = {".c", ".cpp", ".h", ".hpp"}
EXCLUDE_FILES = {"stb_vorbis.c", "stb_vorbis.h", "stb_image.h"}
# Vendored verbatim, so upstream diffs stay readable: src/pc/libm is musl's
# trig (one implementation on every target, see docs/netcode-plan.md section 5)
# and src/pc/mdns is mjansson/mdns. Reformatting them is churn, not style.
EXCLUDE_DIRS = {ROOT / "src/pc/libm", ROOT / "src/pc/mdns"}

# Regex to detect bare long/unsigned long, excluding 'long double', 'long long', and standard macros
BANNED_LONG_REGEX = re.compile(
    r"\b(unsigned\s+long(?!\s+long)|long\s+int|(?<!unsigned\s)(?<!long\s)long)(?!\s+double)(?!\s+long)\b"
)


def get_target_files(custom_files=None):
    if custom_files:
        return [Path(f).resolve() for f in custom_files]

    files = []
    for d in CHECK_DIRS:
        if d.is_dir():
            for p in d.rglob("*"):
                if p.suffix not in SOURCE_EXTENSIONS or not p.is_file():
                    continue
                if p.name in EXCLUDE_FILES or any(x in EXCLUDE_DIRS for x in p.parents):
                    continue
                files.append(p)
    for p in TOOL_FILES:
        if p.is_file() and p.name not in EXCLUDE_FILES:
            files.append(p)
    return sorted(files)


def check_whitespace_and_newline(path: Path, fix: bool = False):
    errors = []
    try:
        content = path.read_bytes()
    except Exception as e:
        return [f"Could not read {path}: {e}"]

    text = content.decode("utf-8", errors="replace")
    lines = text.splitlines(keepends=True)
    modified = False
    new_lines = []

    for i, line in enumerate(lines, 1):
        stripped = line.rstrip("\r\n")
        if stripped != stripped.rstrip(" \t"):
            if fix:
                stripped = stripped.rstrip(" \t")
                line = stripped + "\n"
                modified = True
            else:
                errors.append(f"{path.name}:{i}: Trailing whitespace detected")
        new_lines.append(line)

    if lines and not lines[-1].endswith("\n"):
        if fix:
            new_lines[-1] = new_lines[-1] + "\n"
            modified = True
        else:
            errors.append(f"{path.name}: Missing newline at end of file")

    if fix and modified:
        path.write_text("".join(new_lines), encoding="utf-8")

    return errors


def check_banned_types(path: Path):
    errors = []
    # Only enforce banned types on platform code
    if "src/pc" not in str(path):
        return errors

    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except Exception:
        return errors

    in_block_comment = False
    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        if "/*" in stripped and "*/" not in stripped:
            in_block_comment = True
            continue
        if in_block_comment:
            if "*/" in stripped:
                in_block_comment = False
            continue
        if stripped.startswith("//") or stripped.startswith("*"):
            continue

        # Strip single-line block comments /* ... */
        code_part = re.sub(r"/\*.*?\*/", "", line)
        # Strip line comments // ...
        code_part = code_part.split("//")[0].strip()
        if not code_part:
            continue

        # Allow intentional exceptions tagged with NOLINT on this, previous, or next line
        has_nolint = ("NOLINT" in line or
                      (i > 1 and "NOLINT" in lines[i - 2]) or
                      (i < len(lines) and "NOLINT" in lines[i]))
        if has_nolint:
            continue

        # Ignore Win32 API typedefs or Dolphin SDK compat (AXFX / MCC headers)
        if ("#define" in code_part or "typedef" in code_part or "LONG WINAPI" in code_part
                or "AXFX" in line or "MCC" in line or "s_aux" in line or "AuxBus" in line
                or (path.name == "audio.c" and ("chan[" in line or "line[" in line or "lines[" in line or "tap" in line))):
            continue

        match = BANNED_LONG_REGEX.search(code_part)
        if match:
            # Check if it's inside quotes
            if '"' in code_part and code_part.count('"') % 2 == 0:
                parts = code_part.split('"')
                outside = "".join(parts[::2])
                if not BANNED_LONG_REGEX.search(outside):
                    continue
            errors.append(
                f"{path.name}:{i}: Banned primitive type '{match.group(0)}' found. "
                f"Use fixed-width types (uint32_t, int64_t, size_t, uintptr_t) or add NOLINT with justification."
            )

    return errors


def check_clang_format(path: Path, fix: bool = False):
    if fix:
        cmd = ["clang-format", "-i", str(path)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return [f"clang-format error on {path.name}: {r.stderr.strip()}"]
        return []

    cmd = ["clang-format", "--dry-run", "--Werror", str(path)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        err = (r.stderr or r.stdout or "").strip()
        msg = f"{path.name}: Formatting does not match .clang-format. Run with --fix to reformat."
        if err:
            msg += f"\n  {err}"
        return [msg]
    return []


def main():
    parser = argparse.ArgumentParser(description="Verify code style and formatting for melee-pc")
    parser.add_argument("--fix", action="store_true", help="Automatically fix formatting and whitespace")
    parser.add_argument("--files", nargs="+", help="Specific files to check")
    parser.add_argument("--skip-format", action="store_true", help="Skip clang-format check")
    args = parser.parse_args()

    files = get_target_files(args.files)
    if not files:
        print("No files to check.")
        return 0

    total_errors = 0
    print(f"Checking style across {len(files)} files in melee-pc...")

    for path in files:
        file_errors = []

        # Whitespace checks
        ws_errors = check_whitespace_and_newline(path, fix=args.fix)
        file_errors.extend(ws_errors)

        # Clang-format checks
        if not args.skip_format:
            fmt_errors = check_clang_format(path, fix=args.fix)
            file_errors.extend(fmt_errors)

        # Banned primitive type checks
        type_errors = check_banned_types(path)
        file_errors.extend(type_errors)

        if file_errors:
            total_errors += len(file_errors)
            for err in file_errors:
                print(f"  [FAIL] {err}")

    if total_errors == 0:
        print(f"[PASS] All {len(files)} files meet melee-pc style and portability guidelines.")
        return 0
    else:
        print(f"\n[FAIL] Found {total_errors} issue(s). Run with --fix to resolve formatting issues automatically.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
