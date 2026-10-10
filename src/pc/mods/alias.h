/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Per-pack file aliases ("replace_files" in a fighter or stage of mod.json).
 *
 * A pack can swap other disc files while it is in play: a map pack the extra
 * files its base stage loads on its own (Pokemon Stadium's transformations),
 * a character pack its base's sound bank (audio/<name>.ssm) or effect file.
 * Unlike a mod's plain file replacement, an alias applies only to the match
 * the pack is in, so the base keeps its own files everywhere else.
 *
 * The match setup (synced start data, so both netplay peers agree) decides
 * which tables are on; pc_file_alias() is consulted where the game turns a
 * file name into a disc entry (lbFileGetFullName, the sound-bank loader). */
#ifndef PC_MODS_ALIAS_H
#define PC_MODS_ALIAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PcAliasOwner {
    PC_ALIAS_FIGHTER, /* a character pack (roster index) */
    PC_ALIAS_STAGE,   /* a map pack (stage-pack index) */
} PcAliasOwner;

/* Records "from" -> "to" for one pack. Paths are disc paths with or without
 * the leading '/'; both must be checked by the caller. Strings are copied. */
bool pc_alias_add(PcAliasOwner owner, int pack, const char* from, const char* to);
size_t pc_alias_count(void);

/* One player of the match: the CharacterKind the game runs (a character
 * pack's base) and its pack (-1 = the character itself). */
typedef struct PcAliasPlayer {
    int ckind;
    int pack;
} PcAliasPlayer;

/* Turns on the tables of the match's map pack (-1 = none) and of every
 * character pack whose base no other player in the match runs -- that
 * player would otherwise hear or load the pack's files. Replaces whatever
 * was on before. */
void pc_alias_activate(int stage_pack, const PcAliasPlayer* players, int count);
void pc_alias_clear(void);

/* The set that is on, to put back after a temporary activation (the stage
 * select's preload runs for the next match, not the current scene). */
typedef struct PcAliasState {
    int count;
    int index[64];
} PcAliasState;
void pc_alias_save(PcAliasState* out);
void pc_alias_restore(const PcAliasState* in);

/* The file to open instead of `path`, or `path` itself. The result keeps
 * the input's form (leading '/' or not) and stays valid for the process. */
const char* pc_file_alias(const char* path);

uint32_t pc_alias_hash(uint32_t h);

#ifdef __cplusplus
}
#endif

#endif
