/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
/* Name-entry tables depend on two-byte CP932 glyphs plus a terminator. */
static const char glyphs[][3] = {"あ", "Ｊ", "－", "。"};
int main(void) {
    printf("%zu\n", sizeof(glyphs));
    for (unsigned i=0;i<sizeof(glyphs);i++)
        printf("%02x", ((const unsigned char*)glyphs)[i]);
    puts("");
}
