#ifndef GALE01_IT_3F14
#define GALE01_IT_3F14

#include <Runtime/platform.h>

#include <melee/ft/forward.h>
#include <melee/it/forward.h>

#include <melee/it/kinds/types.h>
#include <melee/it/types.h>

typedef struct DISC_STRUCT it_804D6D40_t {
    /* 0x00 */ s32 x0;
    /* 0x04 */ f32 x4;
    /* 0x08 */ f32 x8;
    /* 0x0C */ f32 xC;
    /* 0x10 */ f32 x10;
    /* 0x14 */ f32 x14;
    /* 0x18 */ f32 x18;
} it_804D6D40_t;
DISC_ASSERT_SIZE(it_804D6D40_t, 0x1C);

/* Root of ItCo.dat ("itPublicData"). x4/x8/xC point at arrays of Article
 * pointer slots (big-endian), so the globals below are DiscU32* and read via
 * DP(Article, it_804D6D24[kind].v). */
typedef struct DISC_STRUCT it_804D6D20_t {
    DISC_PTR(ItemCommonData) x0;
    DISC_PTR(DiscU32) x4;
    DISC_PTR(DiscU32) x8;
    DISC_PTR(DiscU32) xC;
    DISC_PTR(it_804D6D40_t) x10;
    DISC_PTR(Fighter_804D653C_t) x14;
} it_804D6D20_t;
DISC_ASSERT_SIZE(it_804D6D20_t, 0x18);

/* 3F1418 */ extern struct sdata_ItemGXLink it_803F1418[43];
/* 3F14C4 */ extern struct ItemLogicTable it_803F14C4[43];
/* 3F1ED8 */ extern char it_803F1ED8[];
/* 3F1EE4 */ extern char it_803F1EE4[];
/* 3F1EF0 */ extern char it_803F1EF0[];
/* 4A0E30 */ extern RandomItemSpawner it_804A0E30;
/* 4A0E50 */ extern ItemPickTable it_804A0E50;
/* 4A0E60 */ extern ItemPickTable it_804A0E60;
/* 4A0E70 */ extern DamageLogEntry it_804A0E70[15];
/* 4A0F60 */ extern Article* it_804A0F60[30];
/* 4D6D00 */ extern s8 it_804D6D00;
/* 4D6D04 */ extern Fighter_804D653C_t* it_804D6D04;
/* 4D6D08 */ extern s32 it_804D6D08;
/* 4D6D0C */ extern s32 it_804D6D0C;
/* 4D6D10 */ extern u32 it_804D6D10;
/* 4D6D14 */ extern u32 it_804D6D14;
/* 4D6D18 */ extern u32 it_804D6D18;
/* 4D6D1C */ extern u8 it_804D6D1C[4];
/* 4D6D20 */ extern it_804D6D20_t* it_804D6D20;
/* 4D6D24 */ extern DiscU32* it_804D6D24;
/* 4D6D28 */ extern ItemCommonData* it_804D6D28;
/* 4D6D30 */ extern DiscU32* it_804D6D30;
/* 4D6D38 */ extern DiscU32* it_804D6D38;
/* 4D6D40 */ extern it_804D6D40_t* it_804D6D40;

#endif
