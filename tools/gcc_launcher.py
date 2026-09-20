#!/usr/bin/env python3
"""Compile decomp translation units with GCC instead of Clang.

The decomp relies on __attribute__((scalar_storage_order)), which Clang does
not implement, so melee_game's sources go through GCC. Everything else keeps
using Clang.

Android: an AArch64 or x86-64 GCC pointed at the matching NDK sysroot.
macOS:   Homebrew's GCC (gcc-NN), which emits Mach-O objects that link with
         Apple's clang/ld. Set GCC_BIN to pick a specific binary.
"""
import shutil
import sys
import os

args = sys.argv[1:]
if not args:
    sys.exit(0)

compiler = args[0]
cmd_args = args[1:]

# Determine if this compilation is for melee_game (or sysdolphin/melee decomp code)
is_decomp = False
for arg in cmd_args:
    if 'melee_game' in arg or '/src/melee/' in arg or '/src/sysdolphin/' in arg:
        is_decomp = True
        break

if not is_decomp:
    # Run original compiler (clang)
    os.execv(compiler, [compiler] + cmd_args)

CLANG_ONLY_FLAGS = (
    '-fcolor-diagnostics',
    '-Wno-unknown-warning-option',
    '-Werror=format-security',
    '-Wno-unknown-attributes',
)

if sys.platform == 'darwin':
    gcc_bin = os.environ.get('GCC_BIN')
    if not gcc_bin:
        for ver in range(20, 12, -1):
            gcc_bin = shutil.which(f'gcc-{ver}')
            if gcc_bin:
                break
    if not gcc_bin or not os.path.exists(gcc_bin):
        sys.exit('gcc_launcher: no GCC found. `brew install gcc` or set '
                 'GCC_BIN. Clang cannot build the decomp because it lacks '
                 'scalar_storage_order.')

    filtered_args = []
    for arg in cmd_args:
        if arg in CLANG_ONLY_FLAGS:
            continue
        filtered_args.append(arg)

    gcc_cmd = [
        gcc_bin,
        '-fdiagnostics-color=always',
        '-fexec-charset=CP932',
        '-Wno-scalar-storage-order',
    ] + filtered_args
    os.execv(gcc_bin, gcc_cmd)

# Select the target ABI, not the build host. The x86-64 Android preset must
# not accidentally place AArch64 objects in its otherwise x86-64 ELF image.
android_target = next((arg.split('=', 1)[1] for arg in cmd_args
                       if arg.startswith('--target=')), os.path.basename(compiler))
if android_target.startswith('aarch64'):
    android_triple = 'aarch64-linux-android'
    gcc_bin = os.environ.get('GCC_AARCH64_BIN') or shutil.which('aarch64-linux-gnu-gcc')
    architecture_flags = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a73']
elif android_target.startswith('x86_64'):
    android_triple = 'x86_64-linux-android'
    gcc_bin = os.environ.get('GCC_X86_64_BIN') or shutil.which('x86_64-linux-gnu-gcc')
    architecture_flags = ['-m64', '-march=x86-64', '-mtune=generic']
else:
    sys.exit(f'gcc_launcher: unsupported Android target {android_target}')
if not gcc_bin or not os.path.exists(gcc_bin):
    sys.exit(f'gcc_launcher: no GCC for {android_triple}; install the matching '
             'cross compiler or set GCC_AARCH64_BIN/GCC_X86_64_BIN')
gcc_dir = os.path.dirname(gcc_bin)
if gcc_dir:
    os.environ['PATH'] = gcc_dir + ':' + os.environ.get('PATH', '')

ndk_root = os.environ.get('ANDROID_NDK_HOME')
if not ndk_root:
    sys.exit('gcc_launcher: ANDROID_NDK_HOME is not set')
sysroot = os.path.join(ndk_root, 'toolchains/llvm/prebuilt/linux-x86_64/sysroot')

filtered_args = []
skip_next = False
for i, arg in enumerate(cmd_args):
    if skip_next:
        skip_next = False
        continue
    if arg.startswith('--target='):
        continue
    if arg.startswith('--sysroot='):
        continue
    if arg == '-D_FORTIFY_SOURCE' or arg.startswith('-D_FORTIFY_SOURCE='):
        continue
    if arg in CLANG_ONLY_FLAGS:
        continue
    filtered_args.append(arg)

gcc_cmd = [
    gcc_bin,
    '-isystem', f'{sysroot}/usr/include',
    '-isystem', f'{sysroot}/usr/include/{android_triple}',
    '-D_Nonnull=', '-D_Nullable=', '-D_Null_unspecified=',
    '-D__BIONIC_VERSIONER',
    '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
    '-D__ANDROID_API__=26',
    '-fexec-charset=CP932',
    '-Wno-scalar-storage-order',
] + architecture_flags + filtered_args

os.execv(gcc_bin, gcc_cmd)
