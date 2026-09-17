#!/usr/bin/env python3
"""Compile decomp translation units with GCC instead of Apple Clang.

The decomp relies on __attribute__((scalar_storage_order("big-endian"))),
which Clang does not implement. This launcher runs GCC to compile to AArch64
assembly with Darwin symbol prefixes, adapts ELF relocations to Mach-O
relocations (@PAGE, @PAGEOFF, @GOTPAGE, @GOTPAGEOFF), and assembles into
native Mach-O 64-bit arm64 objects via Clang.
"""
import math
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
        sys.exit(f'gcc_ios_launcher: no aarch64 GCC found at {gcc_bin}')

ios_sdk = os.environ.get('IOS_SDK_PATH') or '/home/sian/toolchains/sdks/sdks/iPhoneOS16.5.sdk'

# Extract target output object and source file
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
    if arg == '-arch':
        skip_next = True
        continue
    if arg == '-target':
        skip_next = True
        continue
    if arg.startswith('--target='):
        continue
    if arg.startswith('--sysroot=') or arg == '-isysroot':
        if arg == '-isysroot':
            skip_next = True
        continue
    if arg.startswith('-stdlib='):
        continue
    if arg in (
        '-fcolor-diagnostics',
        '-Wno-unknown-warning-option',
        '-Werror=format-security',
        '-Wno-unknown-attributes',
        '-fno-pie',
    ):
        continue
    if arg.endswith('.c') and not arg.startswith('-'):
        source_file = arg
        continue
    filtered_args.append(arg)

if not source_file or not output_obj:
    # Fallback to compiler if arguments couldn't be parsed
    os.execv(compiler, [compiler] + cmd_args)

temp_s = tempfile.NamedTemporaryFile(suffix='.s', delete=False).name
temp_darwin_s = tempfile.NamedTemporaryFile(suffix='_darwin.s', delete=False).name

try:
    gcc_cmd = [
        gcc_bin,
        '-S',
        '-fleading-underscore',
        '-fno-section-anchors',
        '-fPIC',
        '-ffixed-x18',
        '-fno-asynchronous-unwind-tables',
        '-fno-unwind-tables',
        '-fno-jump-tables',
        '-D__APPLE__=1',
        '-D__arm64__=1',
        '-D__aarch64__=1',
        '-fexec-charset=CP932',
        '-Wno-scalar-storage-order',
        '-fno-strict-aliasing',
        '-fwrapv',
        '-ffp-contract=off',
        '-isystem', f'{ios_sdk}/usr/include',
        source_file,
        '-o', temp_s
    ] + [a for a in filtered_args if a != '-c']

    res = subprocess.run(gcc_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(f"GCC compile error on {source_file}:\n{res.stderr}\n")
        sys.exit(res.returncode)

    # Convert ELF assembly to Darwin Mach-O assembly
    with open(temp_s, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    new_lines = []
    defined_labels = {l.strip()[:-1] for l in lines if l.strip().endswith(':')}
    local_syms = set()
    for l in lines:
        s = l.strip()
        if s.startswith('.local'):
            m_local = re.match(r'\s*\.local\s+([a-zA-Z0-9_.]+)', s)
            if m_local:
                local_syms.add(m_local.group(1))

    for line in lines:
        s = line.strip()
        if any(s.startswith(p) for p in ('.type', '.size', '.aeabi_', '.ident', '.local', '.cfi_')):
            continue
        if '.note.GNU-stack' in s:
            continue

        if s.startswith('.hidden'):
            line = re.sub(r'^\s*\.hidden\s+', '\t.private_extern\t', line)

        m_weak = re.match(r'^\s*\.weak\s+([a-zA-Z0-9_.]+)', line)
        if m_weak:
            sym = m_weak.group(1)
            if sym in defined_labels:
                line = f'\t.weak_definition\t{sym}\n'
            else:
                line = f'\t.weak_reference\t{sym}\n'

        if s.startswith('.comm'):
            m = re.match(r'\s*\.comm\s+([a-zA-Z0-9_.]+)\s*,\s*(\d+)\s*,\s*(\d+)', line)
            if m:
                sym, sz, align = m.group(1), m.group(2), int(m.group(3))
                log2_align = int(math.log2(align)) if align > 0 and (align & (align - 1)) == 0 else align
                if sym in local_syms:
                    line = f'\t.lcomm\t{sym},{sz},{log2_align}\n'
                else:
                    line = f'\t.comm\t{sym},{sz},{log2_align}\n'

        if s.startswith('.section'):
            if '.rodata.str' in s:
                line = '\t.section\t__TEXT,__cstring,cstring_literals\n'
            elif '.rodata' in s:
                line = '\t.section\t__TEXT,__const\n'
            elif '.data.rel.ro' in s:
                line = '\t.section\t__DATA,__const\n'
            elif '.data' in s:
                line = '\t.section\t__DATA,__data\n'
            elif '.bss' in s:
                line = '\t.section\t__DATA,__bss\n'
            elif '.text' in s:
                line = '\t.section\t__TEXT,__text,regular,pure_instructions\n'
            elif '.init_array' in s:
                line = '\t.section\t__DATA,__mod_init_func,mod_init_funcs\n'
            elif '.fini_array' in s:
                line = '\t.section\t__DATA,__mod_term_func,mod_term_funcs\n'
            else:
                line = '\t.section\t__TEXT,__const\n'

        if ':got:' in line:
            line = re.sub(r'adrp\s+([wx]\d+),\s*:got:([a-zA-Z0-9_.]+)', r'adrp \1, \2@GOTPAGE', line)
        elif 'adrp' in line:
            line = re.sub(r'adrp\s+([wx]\d+),\s*([a-zA-Z0-9_.]+)', r'adrp \1, \2@PAGE', line)

        if ':got_lo12:' in line:
            line = re.sub(r'\[\s*([wx]\d+),\s*:got_lo12:([a-zA-Z0-9_.]+)\s*\]', r'[\1, \2@GOTPAGEOFF]', line)
        elif ':lo12:' in line:
            line = re.sub(r'\[\s*([wx]\d+),\s*#?:lo12:([a-zA-Z0-9_.]+)\s*\]', r'[\1, \2@PAGEOFF]', line)
            line = re.sub(r'#?:lo12:([a-zA-Z0-9_.]+)', r'\1@PAGEOFF', line)

        if 'movi' in line:
            line = re.sub(
                r'movi\s+(v\d+)\.(16b|8b),\s*(0x[0-9a-fA-F]+|-?\d+)',
                lambda m: f"movi {m.group(1)}.{m.group(2)}, {hex(int(m.group(3), 0) & 0xff)}",
                line
            )

        new_lines.append(line)

    with open(temp_darwin_s, 'w', encoding='utf-8') as f:
        f.writelines(new_lines)

    # Assemble with Clang into Mach-O object
    clang_cmd = [
        compiler,
        '--target=arm64-apple-ios14.0',
        '-isysroot', ios_sdk,
        '-c', temp_darwin_s,
        '-o', output_obj
    ]
    res_clang = subprocess.run(clang_cmd, capture_output=True, text=True)
    if res_clang.returncode != 0:
        sys.stderr.write(f"Clang assemble error for {source_file}:\n{res_clang.stderr}\n")
        sys.exit(res_clang.returncode)

finally:
    if os.path.exists(temp_s):
        os.remove(temp_s)
    if os.path.exists(temp_darwin_s):
        os.remove(temp_darwin_s)
