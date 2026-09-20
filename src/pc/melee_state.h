/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_PC_STATE_H
#define MELEE_PC_STATE_H

/* Mach-O's linker-defined section boundaries follow ASLR automatically. ELF
 * uses melee_state.ld; PE uses sorted input sections with explicit markers. */
#ifdef __APPLE__
extern char __melee_data_start[] __asm("section$start$__DATA$__melee_data");
extern char __melee_data_end[] __asm("section$end$__DATA$__melee_data");
extern char __melee_bss_start[] __asm("section$start$__DATA$__melee_bss");
extern char __melee_bss_end[] __asm("section$end$__DATA$__melee_bss");
#else
extern char __melee_data_start[], __melee_data_end[];
extern char __melee_bss_start[], __melee_bss_end[];
#endif

#endif
