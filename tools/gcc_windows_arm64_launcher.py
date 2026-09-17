#!/usr/bin/env python3
"""Compile decomp translation units for Windows ARM64 with GCC.

The decomp relies on __attribute__((scalar_storage_order("big-endian"))),
which Clang does not implement. This launcher runs GCC to compile to AArch64
assembly with Windows definitions, adapts ELF assembly directives and sections
to Windows PE/COFF (.rdata, .data, .bss, .text), and assembles into native
ARM64 COFF objects via aarch64-w64-mingw32-clang.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

args = sys.argv[1:]
if not args:
    sys.exit(0)

compiler = args[0]
cmd_args = args[1:]

# Determine if this compilation is for decomp code
is_decomp = False
for arg in cmd_args:
    if 'melee_game' in arg or '/src/melee/' in arg or '/src/sysdolphin/' in arg:
        is_decomp = True
        break

if not is_decomp:
    os.execv(compiler, [compiler] + cmd_args)

gcc_bin = os.environ.get('GCC_AARCH64_BIN') or '/home/sian/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc'
if not os.path.exists(gcc_bin):
    gcc_which = shutil.which('aarch64-linux-gnu-gcc')
    if gcc_which:
        gcc_bin = gcc_which
    else:
        sys.exit(f'gcc_windows_arm64_launcher: no aarch64 GCC found at {gcc_bin}')

mingw_include = os.environ.get('MINGW_ARM64_INCLUDE')
if not mingw_include:
    compiler_dir = os.path.dirname(os.path.abspath(compiler))
    toolchain_root = os.path.dirname(compiler_dir)
    for cand in [
        os.path.join(toolchain_root, 'generic-w64-mingw32', 'include'),
        os.path.join(toolchain_root, 'aarch64-w64-mingw32', 'include'),
        '/home/sian/toolchains/llvm-mingw/generic-w64-mingw32/include',
        '/usr/aarch64-w64-mingw32/include',
    ]:
        if os.path.isdir(cand):
            mingw_include = cand
            break

output_obj = None
source_file = None
filtered_args = []
skip_next = False

for i, arg in enumerate(cmd_args):
    if skip_next:
        skip_next = False
        continue
    if arg == '-o' and i + 1 < len(cmd_args):
        output_obj = cmd_args[i + 1]
        skip_next = True
        continue
    if arg.startswith('-o'):
        output_obj = arg[2:]
        continue
    if arg.startswith('--target='):
        continue
    if arg.startswith('--sysroot='):
        continue
    if arg in (
        '-fcolor-diagnostics',
        '-Wno-unknown-warning-option',
        '-Werror=format-security',
        '-Wno-unknown-attributes',
        '-mno-ms-bitfields',
    ):
        continue
    if arg.endswith('.c') and not arg.startswith('-'):
        source_file = arg
        continue
    filtered_args.append(arg)

if not source_file or not output_obj:
    os.execv(compiler, [compiler] + cmd_args)

temp_s = tempfile.NamedTemporaryFile(suffix='.s', delete=False).name
temp_coff_s = tempfile.NamedTemporaryFile(suffix='_coff.s', delete=False).name

try:
    gcc_cmd = [
        gcc_bin,
        '-S',
        '-DTARGET_PC=1',
        '-DMELEE_PC=1',
        '-D_WIN32=1',
        '-D_WIN64=1',
        '-D__aarch64__=1',
        '-D__cdecl=',
        '-D_CRTIMP=',
        '-D__declspec(x)=',
        '-fno-pic',
        '-fno-pie',
        '-fno-asynchronous-unwind-tables',
        '-fno-unwind-tables',
        '-fexec-charset=CP932',
        '-Wno-scalar-storage-order',
        '-fno-strict-aliasing',
        '-fwrapv',
        '-ffp-contract=off',
    ]
    if mingw_include:
        gcc_cmd.extend(['-isystem', mingw_include])
    gcc_cmd.extend([
        source_file,
        '-o', temp_s
    ])
    gcc_cmd.extend([a for a in filtered_args if a != '-c'])

    res = subprocess.run(gcc_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(f"GCC compile error on {source_file}:\n{res.stderr}\n")
        sys.exit(res.returncode)

    # Convert ELF assembly to Windows PE/COFF assembly
    with open(temp_s, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    new_lines = []
    for line in lines:
        s = line.strip()
        if any(s.startswith(p) for p in ('.type', '.size', '.aeabi_', '.ident', '.local', '.cfi_')):
            continue
        if '.note.GNU-stack' in s:
            continue

        if s.startswith('.section'):
            if '.rodata' in s:
                line = '\t.section\t.rdata,"dr"\n'
            elif '.data' in s:
                line = '\t.section\t.data,"dw"\n'
            elif '.bss' in s:
                line = '\t.section\t.bss,"bw"\n'
            elif '.text' in s:
                line = '\t.section\t.text,"xr"\n'
            else:
                line = '\t.section\t.rdata,"dr"\n'

        if 'movi' in line:
            line = re.sub(
                r'movi\s+(v\d+)\.(16b|8b),\s*(0x[0-9a-fA-F]+|-?\d+)',
                lambda m: f"movi {m.group(1)}.{m.group(2)}, {hex(int(m.group(3), 0) & 0xff)}",
                line
            )

        new_lines.append(line)

    with open(temp_coff_s, 'w', encoding='utf-8') as f:
        f.writelines(new_lines)

    # Assemble with clang to ARM64 Windows COFF object
    clang_cmd = [
        compiler,
        '-c', temp_coff_s,
        '-o', output_obj
    ]
    res_clang = subprocess.run(clang_cmd, capture_output=True, text=True)
    if res_clang.returncode != 0:
        sys.stderr.write(f"Clang assemble error for {source_file}:\n{res_clang.stderr}\n")
        sys.exit(res_clang.returncode)

finally:
    if os.path.exists(temp_s):
        os.remove(temp_s)
    if os.path.exists(temp_coff_s):
        os.remove(temp_coff_s)
