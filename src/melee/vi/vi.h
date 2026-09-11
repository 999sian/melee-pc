#ifndef GALE01_31C99C
#define GALE01_31C99C

#include <Runtime/platform.h>

#include <melee/sc/types.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/displayfunc.h>
#include <sysdolphin/baselib/fog.h>
#include <sysdolphin/baselib/gobj.h>

/* 31C99C */ char* viGetCharAnimByIndex(s32);
/* 31C9B4 */ void vi_8031C9B4(s32, s32);
/* 31CA04 */ void vi_8031CA04(HSD_GObj*);
/* 31CAAC */ void vi_8031CAAC(void);

/* SceneDesc slot readers shared by the vi scenes. */
static inline DynamicModelDesc* vi_SceneModel(SceneDesc* s, int i)
{
    return DP(DynamicModelDesc, DP(DiscU32, s->models)[i].v);
}
static inline HSD_CameraDescPerspective* vi_SceneCamDesc(SceneDesc* s)
{
    return (HSD_CameraDescPerspective*) DP(
        HSD_CObjDesc, DP(struct SceneCameraDesc, s->cameras)->desc);
}
static inline HSD_CameraAnim* vi_SceneCamAnim(SceneDesc* s, int i)
{
    return DP(HSD_CameraAnim,
              DP(DiscU32, DP(struct SceneCameraDesc, s->cameras)->anims)[i].v);
}
static inline HSD_FogDesc* vi_SceneFogDesc(SceneDesc* s)
{
    return DP(HSD_FogDesc, DP(struct SceneFogDesc, s->fogs)->desc);
}
static inline HSD_CameraAnim* vi_SceneFogAnim(SceneDesc* s, int i)
{
    return DP(HSD_CameraAnim,
              DP(DiscU32, DP(struct SceneFogDesc, s->fogs)->anims)[i].v);
}

static inline void vi_RunCamera(HSD_GObj* gobj, u8 erase_colors[4], u64 prio)
{
    if (HSD_CObjSetCurrent(GET_COBJ(gobj))) {
        HSD_SetEraseColor(erase_colors[0], erase_colors[1], erase_colors[2],
                          erase_colors[3]);
        HSD_CObjEraseScreen(GET_COBJ(gobj), 1, 0, 1);
        vi_8031CA04(gobj);
        gobj->gxlink_prios = prio;
        HSD_GObj_80390ED0(gobj, 0x7);
        HSD_CObjEndCurrent();
    }
}

#endif
