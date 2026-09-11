#ifndef GALE01_08A698
#define GALE01_08A698

#include <melee/ft/forward.h>

typedef struct DISC_STRUCT WaitStruct {
    union DISC_STRUCT {
        struct DISC_STRUCT {
            DISC_PTR(DiscS32) x;
            DISC_PTR(DiscS32) y;
        } p;
        struct DISC_STRUCT {
            int x;
            int y;
        } i;
    } u;
} WaitStruct;
DISC_ASSERT_SIZE(WaitStruct, 0x8);

/* 08A698 */ bool ftCo_8008A698(Fighter* fp);
/* 08A6D8 */ void ftCo_8008A6D8(Fighter_GObj* gobj, s32 anim_id);
/* 08A7A8 */ void ftCo_8008A7A8(Fighter_GObj* gobj, WaitStruct* arg1);
/* 3C54A8 */ extern char ftWaitAnim_803C54A8[];
/* 3C54C4 */ extern char ftWaitAnim_803C54C4[];

#endif
