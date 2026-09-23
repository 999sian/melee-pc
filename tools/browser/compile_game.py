#!/usr/bin/env python3
"""Compile the game's C (src/melee, src/sysdolphin) to wasm objects.

Each unit is preprocessed by Clang, has its string literals converted to the
CP932 execution charset GCC would have used, has its DISC_STRUCT accesses
lowered to explicit big-endian loads and stores by disc_lower, and is then
compiled by emcc. Objects land in build/browser/game, where
platforms/browser/CMakeLists.txt picks them up.
"""
import argparse
import concurrent.futures
import json
import subprocess

from common import BUILD, DISC_LOWER, EMSCRIPTEN, LLVM, ROOT, SYSROOT, WASM_TARGET
from execution_charset import cp932_literals

OUT = BUILD / 'game'

PREPROCESS_FLAGS = [
    WASM_TARGET, f'--sysroot={SYSROOT}', '-pthread', '-fsigned-char',
    '-D__EMSCRIPTEN__', '-DTARGET_PC=1', '-DMELEE_PC=1', '-DMELEE_DISC_LOWERING=1',
    '-Iextern/aurora/include', '-Isrc', '-Isrc/sdk_include',
    '-include', 'dolphin/gx.h',
    '-include', 'src/pc/compat.h',
    '-include', 'tools/browser/disc_access.h',
    '-Wno-everything', '-ferror-limit=5',
]

# Mirrors melee_game's options in the top-level CMakeLists.txt: the simulation
# has to produce the same floats as every other platform. -fsigned-char is
# wasm32's default already; stated for the same reason the native build does.
COMPILE_FLAGS = [
    '-O2', '-pthread', '-fsigned-char', '-Wno-everything', '-ferror-limit=5',
    '-ffp-contract=off', '-fno-fast-math',
    '-fno-builtin-sinf', '-fno-builtin-cosf', '-fno-builtin-tanf', '-fno-builtin-atanf',
    '-ftrivial-auto-var-init=zero', '-fno-strict-aliasing', '-fwrapv',
]


def game_sources():
    sources = sorted([*(ROOT / 'src/melee').rglob('*.c'), *(ROOT / 'src/sysdolphin').rglob('*.c')])
    sources.append(ROOT / 'src/pc/vtxarray.c')
    # Same exclusion as cmake/GameSources.cmake: PowerPC debugger integration.
    return [s for s in sources if s.name != 'debugconsole_main.c']


def build(source):
    rel = source.relative_to(ROOT)
    base = OUT / rel
    base.parent.mkdir(parents=True, exist_ok=True)
    preprocessed, lowered = base.with_suffix('.i'), base.with_suffix('.lowered.c')
    obj, log = base.with_suffix('.o'), base.with_suffix('.log')

    def run(stage, cmd, stdout):
        if subprocess.run(list(map(str, cmd)), cwd=ROOT, stdout=stdout, stderr=err).returncode:
            return {'source': str(rel), 'stage': stage, 'status': 'failed', 'log': str(log.relative_to(ROOT))}
        return None

    with log.open('w') as err:
        failed = run('preprocess', [LLVM / 'bin/clang', *PREPROCESS_FLAGS, '-E', source, '-o', preprocessed], err)
        if failed:
            return failed
        preprocessed.write_text(cp932_literals(preprocessed.read_text()))
        with lowered.open('w') as out:
            failed = run('lower', [DISC_LOWER, preprocessed, WASM_TARGET], out)
        if failed:
            return failed
        failed = run('compile', [EMSCRIPTEN / 'emcc', *COMPILE_FLAGS, '-c', lowered, '-o', obj], err)
        return failed or {'source': str(rel), 'status': 'passed'}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--source', action='append', help='compile only this unit (repeatable)')
    args = parser.parse_args()

    sources = [ROOT / s for s in args.source] if args.source else game_sources()
    OUT.mkdir(parents=True, exist_ok=True)
    if not args.source:
        # An upstream sync can remove or rename a unit; never link its stale object.
        expected = {OUT / s.relative_to(ROOT).with_suffix('.o') for s in sources}
        for stale in OUT.rglob('*.o'):
            if stale not in expected:
                stale.unlink()

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(build, sources):
            results.append(result)
            if result['status'] == 'failed':
                print(f"{result['source']}: FAILED at {result['stage']} (see {result['log']})", flush=True)
            elif len(results) % 100 == 0:
                print(len(results), 'units', flush=True)

    report = OUT / ('report-partial.json' if args.source else 'report.json')
    report.write_text(json.dumps(results, indent=2) + '\n')
    passed = sum(r['status'] == 'passed' for r in results)
    print(f'{passed} / {len(results)} compiled')
    raise SystemExit(passed != len(results))


if __name__ == '__main__':
    main()
