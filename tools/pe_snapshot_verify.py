#!/usr/bin/env python3
"""Fail the ELF/PE link if rollback markers do not contain required game globals."""
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nm', required=True)
    parser.add_argument('image')
    parser.add_argument('--tracked', nargs='+', required=True)
    parser.add_argument('--excluded', nargs='+', required=True)
    args = parser.parse_args()
    text = subprocess.check_output([args.nm, '-n', '--defined-only', args.image], text=True)
    addresses = {}
    for line in text.splitlines():
        words = line.split()
        if len(words) == 3:
            try: addresses[words[2]] = int(words[0], 16)
            except ValueError: pass
    ranges = [(addresses[f'__melee_{kind}_start'], addresses[f'__melee_{kind}_end']) for kind in ('data','bss')]
    if any(end <= start for start, end in ranges):
        raise SystemExit('Rollback section is empty or reversed')
    if max(ranges[0][0], ranges[1][0]) < min(ranges[0][1], ranges[1][1]):
        raise SystemExit('Rollback sections overlap')
    for names, expected in ((args.tracked, True), (args.excluded, False)):
        for name in names:
            if name not in addresses:
                raise SystemExit(f'Rollback validation is missing symbol {name}')
            tracked = any(start <= addresses[name] < end for start, end in ranges)
            if tracked != expected:
                raise SystemExit(f'Rollback misplaced {name}: tracked={tracked}, expected={expected}')
    print('Rollback sections: game globals included, engine/audio globals excluded')


if __name__ == '__main__':
    main()
