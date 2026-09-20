/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PE sorts dollar-suffixed input sections into the same output section.
 * tools/pe_snapshot_compile.py assigns eligible game objects the M suffix;
 * A/Z markers bracket them without including ordinary engine/audio data.
 * BSS markers explicitly retain COFF uninitialized-data characteristics.
 * Built only for the independently verified MinGW x86-64 configuration. */
#if !defined(_WIN32) || !defined(__x86_64__)
#error "PE snapshot section markers are verified only for MinGW x86-64"
#endif
__asm__(".section .mld$A,\"dw\"\n"
        ".globl __melee_data_start\n"
        "__melee_data_start:\n.byte 0\n"
        ".section .mld$Z,\"dw\"\n"
        ".globl __melee_data_end\n"
        "__melee_data_end:\n.byte 0\n"
        ".section .mlb$A,\"bw\"\n"
        ".globl __melee_bss_start\n"
        "__melee_bss_start:\n.space 1\n"
        ".section .mlb$Z,\"bw\"\n"
        ".globl __melee_bss_end\n"
        "__melee_bss_end:\n.space 1\n"
        ".text\n");
