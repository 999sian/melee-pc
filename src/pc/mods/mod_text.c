/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Text helpers shared by the pack registries; no game dependencies, so the
 * unit tests (tools/test_mods.c) link it on its own. */
#include "mod_internal.h"

#include <stdlib.h>
#include <string.h>

char* pc_mod_ascii_to_sjis(const char* ascii) {
    size_t n = strlen(ascii);
    unsigned char* out = (unsigned char*)malloc(n * 2 + 1);
    if (out == NULL)
        return NULL;
    unsigned char* o = out;
    for (const unsigned char* s = (const unsigned char*)ascii; *s; ++s) {
        unsigned code;
        unsigned char ch = *s;
        if (ch == ' ') {
            *o++ = ' '; /* the disc's own names keep a half-width space */
            continue;
        }
        if (ch >= 'A' && ch <= 'Z')
            code = 0x8260 + (ch - 'A');
        else if (ch >= 'a' && ch <= 'z')
            code = 0x8281 + (ch - 'a');
        else if (ch >= '0' && ch <= '9')
            code = 0x824F + (ch - '0');
        else if (ch == '.')
            code = 0x8144;
        else if (ch == '&')
            code = 0x8195;
        else if (ch == '-')
            code = 0x817C;
        else if (ch == '\'')
            code = 0x8166;
        else if (ch == '!')
            code = 0x8149;
        else if (ch == '?')
            code = 0x8148;
        else
            code = 0x8140;
        *o++ = (unsigned char)(code >> 8);
        *o++ = (unsigned char)(code & 0xFF);
    }
    *o = '\0';
    return (char*)out;
}
