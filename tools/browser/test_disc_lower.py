#!/usr/bin/env python3
"""Check disc_lower against GCC's scalar_storage_order, the behaviour it replaces.

Every tests/browser/disc_*.c prints values and raw bytes. GCC's output is the
oracle; the lowered program must print the same thing both natively (Clang)
and as wasm (emcc + node).
"""
import shutil
import subprocess

from common import BUILD, DISC_LOWER, EMSCRIPTEN, LLVM, ROOT, SYSROOT, WASM_TARGET, node
from execution_charset import cp932_literals

OUT = BUILD / 'compiler-tests'
GCC = next((g for g in map(shutil.which, ('gcc-16', 'gcc-15', 'gcc-14', 'gcc-13', 'gcc-12')) if g), None)


def run(cmd):
    return subprocess.run(list(map(str, cmd)), cwd=ROOT, check=True, capture_output=True).stdout


def main():
    if not GCC:
        raise SystemExit('A real GCC (gcc-12 or newer on PATH) is required as the scalar_storage_order oracle.')
    OUT.mkdir(parents=True, exist_ok=True)
    for source in sorted((ROOT / 'tests/browser').glob('disc_*.c')):
        binary = OUT / source.stem
        run([GCC, '-w', '-fexec-charset=CP932', source, '-o', binary])
        expected = run([binary])
        for target in ('host', 'wasm'):
            target_flags = [WASM_TARGET, f'--sysroot={SYSROOT}'] if target == 'wasm' else []
            preprocessed = OUT / f'{source.stem}-{target}.i'
            lowered = preprocessed.with_suffix('.c')
            preprocessed.write_bytes(run([LLVM / 'bin/clang', *target_flags, '-E',
                                          '-include', 'tools/browser/disc_access.h',
                                          '-DMELEE_DISC_LOWERING', source]))
            preprocessed.write_text(cp932_literals(preprocessed.read_text()))
            lowered.write_bytes(run([DISC_LOWER, preprocessed, *([WASM_TARGET] if target == 'wasm' else [])]))
            if target == 'host':
                run([LLVM / 'bin/clang', '-O2', '-w', lowered, '-o', binary])
                actual = run([binary])
            else:
                script = OUT / f'{source.stem}.js'
                run([EMSCRIPTEN / 'emcc', '-O2', '-w', lowered, '-o', script])
                actual = run([node(), script])
            assert actual == expected, (source.stem, target, expected, actual)
            print(source.stem, target, 'matches GCC values and bytes', flush=True)


if __name__ == '__main__':
    main()
