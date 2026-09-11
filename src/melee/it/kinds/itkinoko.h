#ifndef GALE01_293660
#define GALE01_293660

#include <Runtime/platform.h>

#include <melee/it/forward.h>

#include <melee/it/kinds/types.h>

typedef struct DISC_STRUCT KinokoAnim {
    DISC_PTR(HSD_AnimJoint) joint;
} KinokoAnim;
DISC_ASSERT_SIZE(KinokoAnim, 0x4);

typedef struct DISC_STRUCT KinokoAttrs {
    f32 x0;
    f32 x4;
    s32 x8;
} KinokoAttrs;
DISC_ASSERT_SIZE(KinokoAttrs, 0xC);

HSD_AnimJoint* it_80293660(int idx);
void itKinoko_Logic26_Spawned(Item_GObj*);
void it_802936E4(Item_GObj*);
bool itKinoko_UnkMotion0_Anim(Item_GObj*);
void itKinoko_UnkMotion0_Phys(Item_GObj*);
bool itKinoko_UnkMotion0_Coll(Item_GObj*);
void it_8029385C(Item_GObj*);
void it_80293884(Item_GObj*);
bool itKinoko_UnkMotion1_Anim(Item_GObj*);
void itKinoko_UnkMotion1_Phys(Item_GObj*);
bool itKinoko_UnkMotion1_Coll(Item_GObj*);
bool itKinoko_Logic26_DmgDealt(Item_GObj*);
void itKinoko_Logic26_EvtUnk(Item_GObj*, HSD_GObj*);

extern ItemStateTable it_803F6110[];

#endif
