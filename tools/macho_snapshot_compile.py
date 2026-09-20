#!/usr/bin/env python3
"""Wrap the game compiler/bridge, then label only its simulation data for rollback."""
import argparse
from pathlib import Path
import subprocess
import sys
from macho_snapshot import rewrite
from pe_snapshot_compile import exclusions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exclude-script', type=Path,
                        default=Path(__file__).resolve().parents[1]/'src/pc/melee_state.ld')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('compiler command required after --')
    output = None
    try:
        if '-c' not in command or '-o' not in command:
            raise ValueError('expected a single compilation with explicit -o')
        output = Path(command[command.index('-o')+1])
        sources = [Path(x) for x in command if x.endswith('.c') and Path(x).is_file()]
        if len(sources) != 1:
            raise ValueError('expected one C source')
        result = subprocess.run(command)
        if result.returncode:
            output.unlink(missing_ok=True)
            return result.returncode
        if sources[0].name not in exclusions(args.exclude_script):
            rewrite(output)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        if output is not None:
            output.unlink(missing_ok=True)
        print(f'Apple snapshot sectioning failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
