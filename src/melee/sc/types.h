#ifndef MELEE_SC_TYPES_H
#define MELEE_SC_TYPES_H

#include <melee/sc/forward.h> // IWYU pragma: export
#include <sysdolphin/baselib/forward.h>

/* Scene descriptors are read in place from archives (map_head, *Scene
 * symbols, EfXxData). Runtime code that fills one from lbArchive_LoadSections
 * must load into void* locals and DP_SET the slots.
 * Pointer arrays: `DISC_PTR(DiscU32) anims` is `HSD_AnimJoint*[]` on disc,
 * read as `(HSD_AnimJoint*) (uintptr_t) DP(DiscU32, d->anims)[i].v`. */

/// Model with a single animation or no animation
struct DISC_STRUCT StaticModelDesc {
    DISC_PTR(HSD_Joint) joint;
    DISC_PTR(HSD_AnimJoint) animjoint;
    DISC_PTR(HSD_MatAnimJoint) matanim_joint;
    DISC_PTR(HSD_ShapeAnimJoint) shapeanim_joint;
};
DISC_ASSERT_SIZE(struct StaticModelDesc, 0x10);

/// Model with multiple animations
struct DISC_STRUCT DynamicModelDesc {
    DISC_PTR(HSD_Joint) joint;
    DISC_PTR(DiscU32) anims;      /* HSD_AnimJoint*[] */
    DISC_PTR(DiscU32) matanims;   /* HSD_MatAnimJoint*[] */
    DISC_PTR(DiscU32) shapeanims; /* HSD_ShapeAnimJoint*[] */
};
DISC_ASSERT_SIZE(struct DynamicModelDesc, 0x10);

struct DISC_STRUCT SceneCameraDesc {
    DISC_PTR(HSD_CObjDesc) desc;
    DISC_PTR(DiscU32) anims; /* HSD_CameraAnim*[] */
};
DISC_ASSERT_SIZE(struct SceneCameraDesc, 0x8);

struct DISC_STRUCT LightList {
    DISC_PTR(HSD_LightDesc) desc;
    DISC_PTR(DiscU32) anims; /* HSD_LightAnim*[] */
};
DISC_ASSERT_SIZE(struct LightList, 0x8);

struct DISC_STRUCT SceneFogDesc {
    DISC_PTR(HSD_FogDesc) desc;
    DISC_PTR(DiscU32) anims; /* HSD_CameraAnim*[] */
};
DISC_ASSERT_SIZE(struct SceneFogDesc, 0x8);

/// The basis of a rendered scene, like a stage, menu, or HUD overlay
struct DISC_STRUCT SceneDesc {
    DISC_PTR(DiscU32) models; /* DynamicModelDesc*[], NULL-terminated */
    DISC_PTR(struct SceneCameraDesc) cameras;
    DISC_PTR(DiscU32) lights; /* LightList*[], NULL-terminated */
    DISC_PTR(struct SceneFogDesc) fogs;
};
DISC_ASSERT_SIZE(struct SceneDesc, 0x10);

#endif
