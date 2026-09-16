#!/usr/bin/env python3
"""Compile decomp translation units with GCC instead of the NDK's Clang.

The decomp relies on __attribute__((scalar_storage_order)), which Clang does
not implement, so melee_game's sources go through an aarch64 GCC cross
compiler pointed at the NDK sysroot. Everything else keeps using Clang.
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

# Setup GCC paths
gcc_bin = os.environ.get('GCC_AARCH64_BIN') or shutil.which(
    'aarch64-linux-gnu-gcc')
if not gcc_bin or not os.path.exists(gcc_bin):
    sys.exit(
        'gcc_launcher: no aarch64 GCC found. Install gcc-aarch64-linux-gnu '
        'or set GCC_AARCH64_BIN. Clang cannot build the decomp because it '
        'lacks scalar_storage_order.')
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
    if arg in (
        '-fcolor-diagnostics',
        '-Wno-unknown-warning-option',
        '-Werror=format-security',
        '-Wno-unknown-attributes',
    ):
        continue
    filtered_args.append(arg)

gcc_cmd = [
    gcc_bin,
    '-isystem', f'{sysroot}/usr/include',
    '-isystem', f'{sysroot}/usr/include/aarch64-linux-android',
    '-D_Nonnull=', '-D_Nullable=', '-D_Null_unspecified=',
    '-D__BIONIC_VERSIONER',
    '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
    '-D__ANDROID_API__=26',
    '-fexec-charset=CP932',
    '-Wno-scalar-storage-order',
    '-march=armv8-a+crc+crypto',
    '-mtune=cortex-a73',
] + filtered_args

os.execv(gcc_bin, gcc_cmd)
