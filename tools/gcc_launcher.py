#!/usr/bin/env python3
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
gcc_bin = os.environ.get('GCC_AARCH64_BIN', '/home/sian/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc')
gcc_dir = os.path.dirname(gcc_bin)
if gcc_dir:
    os.environ['PATH'] = gcc_dir + ':' + os.environ.get('PATH', '')

ndk_root = os.environ.get('ANDROID_NDK_HOME', '/home/sian/Android/ndk/26.3.11579264')
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
] + filtered_args

os.execv(gcc_bin, gcc_cmd)
