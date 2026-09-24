/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_IDENTITY_H
#define PC_NET_IDENTITY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PcNetIdentity {
    uint8_t secret_key[64];
    uint8_t public_key[32];
    char code[18]; /* 1..8 uppercase letters/digits, #, eight base32 digits */
} PcNetIdentity;
bool pc_identity_random(void* bytes, size_t length);
bool pc_identity_load(PcNetIdentity* identity, const char* directory, const char* name);
bool pc_identity_code_valid(const char* code);
/* Finds a connect code in free text -- a paste, a chat line, a code typed
 * with the wrong case or with 0/1/8 for O/I/B -- and returns its eight
 * base32 key characters in `suffix` (plus the name before the '#', if there
 * is one, in `name`; "" otherwise). Only the suffix identifies a player: it
 * is derived from their key, and the name is a label they can change. */
bool pc_identity_parse_code(const char* text, char suffix[9], char name[9]);
/* The suffix of `code` (a valid NAME#SUFFIX), or "" */
const char* pc_identity_code_suffix(const char* code);
void pc_identity_sign(
    const PcNetIdentity* identity, uint8_t signature[64], const void* message, size_t length);
bool pc_identity_verify(
    const uint8_t public_key[32], const uint8_t signature[64], const void* message, size_t length);
void pc_identity_clear(PcNetIdentity* identity);
#ifdef __cplusplus
}
#endif
#endif
