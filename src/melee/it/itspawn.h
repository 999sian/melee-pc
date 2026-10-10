#ifndef GALE01_ITSPAWN_H
#define GALE01_ITSPAWN_H

#include <Runtime/platform.h>

#include <melee/it/forward.h>

#include <dolphin/mtx.h>

struct it_8026C47C_arg0_t;

/* 26C47C */ void it_8026C47C(struct it_8026C47C_arg0_t*);
/* 26C65C */ ItemKind it_8026C65C(ItemPickTable*);
/* 26C704 */ bool it_8026C704(void);
/* 26C75C */ ItemKind it_8026C75C(ItemPickTable*);
/* 26C88C */ void fn_8026C88C(HSD_GObj*);
/* 26CA4C */ void it_8026CA4C(ItemPickTable*, s32*, u64, s32, f32);
/* 26CB3C */ bool it_8026CB3C(Vec3*);
/* 26CB9C */ void it_8026CB9C(s32*, u64, f32);
/* 26CD50 */ void it_8026CD50(s32*, u64, f32);
/* 26CF04 */ void it_8026CF04(void);
/* 26D018 */ void it_8026D018(void);
#ifdef TARGET_PC
/// Mod item packs (pc/mods/items.h): the pack a spawn of @p kind takes from
/// the random draw (index + 1, 0 = the item itself); consumes the pick.
u8 it_PcTakePack(ItemKind kind);
/// The Article of item pack @p pack (index + 1) loaded for this match, or NULL.
Article* it_PcPackArticle(u8 pack);
/// Makes the next spawn of @p kind item pack @p pack (index + 1, 0 = none).
void it_PcSetPick(ItemKind kind, u8 pack);
/// Draw weight of item pack @p pack (index) next to its base @p kind, whose
/// own weight is @p base_weight; 0 when the pack is not on @p kind or did not
/// load.
s32 it_PcPackWeight(int pack, ItemKind kind, s32 base_weight);
int it_PcPackCount(void);
/// Character, projectile and stage items (PC_ITEM_OWNED): the pack a spawn
/// of @p kind by @p parent (fighter, item or NULL) becomes, index + 1 or 0.
u8 it_PcOwnedPack(ItemKind kind, HSD_GObj* parent);
/// Logs the first spawn of item pack @p pack (index + 1) in a match.
void it_PcNoteSpawn(u8 pack);
#endif
/* 26D258 */ bool it_8026D258(Vec3*, ItemKind);
/* 26D324 */ bool it_8026D324(ItemKind);
/* 26D3CC */ bool it_8026D3CC(void);
#endif
