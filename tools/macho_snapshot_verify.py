#!/usr/bin/env python3
"""Fail the Apple link if simulation/audio boundaries are incomplete."""
import argparse
from macho_snapshot import verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('--tracked', nargs='+', required=True)
    parser.add_argument('--excluded', nargs='+', required=True)
    args = parser.parse_args()
    try:
        verify(args.image, args.tracked, args.excluded)
    except (OSError, ValueError) as error:
        raise SystemExit(f'Apple rollback validation failed: {error}')
    print('Apple rollback sections: game globals included, engine/audio globals excluded')


if __name__ == '__main__':
    main()
