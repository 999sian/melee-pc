/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Keep even empty ranges resolvable. Game objects join these sections after
 * compilation; zerofill stays zerofill and relocation records stay intact. */
#ifndef __APPLE__
#error "Mach-O snapshot anchors require an Apple target"
#endif
__asm__(".section __DATA,__melee_data,regular\n"
        ".globl _melee_state_data_anchor\n"
        "_melee_state_data_anchor:\n.byte 0\n"
        ".no_dead_strip _melee_state_data_anchor\n"
        ".zerofill __DATA,__melee_bss,_melee_state_bss_anchor,1,0\n"
        ".globl _melee_state_bss_anchor\n"
        ".no_dead_strip _melee_state_bss_anchor\n"
        ".text\n");
