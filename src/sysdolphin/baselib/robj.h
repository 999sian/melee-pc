#ifndef _robj_h_
#define _robj_h_

#include <Runtime/platform.h>

#include <sysdolphin/baselib/forward.h> // IWYU pragma: export

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/objalloc.h>

#define ROBJ_TYPE_MASK 0x70000000
#define REFTYPE_EXP 0x00000000
#define REFTYPE_JOBJ 0x10000000
#define REFTYPE_LIMIT 0x20000000
#define REFTYPE_BYTECODE 0x30000000
#define REFTYPE_IKHINT 0x40000000

struct HSD_Rvalue {
    HSD_Rvalue* next;
    u32 flags;
    HSD_JObj* jobj;
};

struct DISC_STRUCT HSD_RvalueList {
    u32 flags;
    DISC_PTR(HSD_Joint) joint;
};
DISC_ASSERT_SIZE(HSD_RvalueList, 0x8);

struct HSD_IKHint {
    f32 bone_length;
    f32 rotate_x;
};

struct DISC_STRUCT HSD_IKHintDesc {
    f32 bone_length;
    f32 rotate_x;
};
DISC_ASSERT_SIZE(HSD_IKHintDesc, 0x8);

struct HSD_Exp {
    union {
        f32 (*func)(void*);
        u8* bytecode;
    } expr;
    HSD_Rvalue* rvalue;
    u32 nb_args;
    u8 is_bytecode;
};

struct DISC_STRUCT HSD_ExpDesc {
    DISC_PTR(void) func; /* f32 (*)(void*); never valid on disc */
    DISC_PTR(HSD_RvalueList) rvalue;
};
DISC_ASSERT_SIZE(HSD_ExpDesc, 0x8);

struct DISC_STRUCT HSD_ByteCodeExpDesc {
    DISC_PTR(u8) bytecode; /* big-endian bytecode stream */
    DISC_PTR(HSD_RvalueList) rvalue;
};
DISC_ASSERT_SIZE(HSD_ByteCodeExpDesc, 0x8);

struct HSD_RObj {
    HSD_RObj* next;
    u32 flags;
    union {
        HSD_JObj* jobj;
        HSD_Exp exp;
        f32 limit;
        HSD_IKHint ik_hint;
    } u;
    HSD_AObj* aobj;
};

struct DISC_STRUCT HSD_RObjDesc {
    DISC_PTR(HSD_RObjDesc) next;
    u32 flags; // 0x04
    union DISC_STRUCT {
        u32 i;
        DISC_PTR(HSD_ExpDesc) exp;
        DISC_PTR(HSD_ByteCodeExpDesc) bcexp;
        DISC_PTR(HSD_IKHintDesc) ik_hint;
        DISC_PTR(HSD_Joint) joint;
        f32 limit;
    } u;
};
DISC_ASSERT_SIZE(HSD_RObjDesc, 0xC);

struct DISC_STRUCT HSD_RObjAnimJoint {
    DISC_PTR(HSD_RObjAnimJoint) next;
    DISC_PTR(HSD_AObjDesc) aobjdesc;
};
DISC_ASSERT_SIZE(HSD_RObjAnimJoint, 0x8);

void _HSD_RObjForgetMemory(void* low, void* high);
void HSD_RObjInitAllocData(void);
HSD_ObjAllocData* HSD_RObjGetAllocData(void);
HSD_ObjAllocData* HSD_RvalueObjGetAllocData(void);
HSD_RObj* HSD_RObjAlloc(void);
void HSD_RObjFree(HSD_RObj* robj);

void HSD_RObjSetFlags(HSD_RObj* robj, u32 flags);
HSD_RObj* HSD_RObjGetByType(HSD_RObj* robj, u32 type, u32 subtype);
void HSD_RObjAnimAll(HSD_RObj* robj);
void HSD_RObjRemoveAnimAllByFlags(HSD_RObj* robj, u32 flags);
void HSD_RObjRemoveAnimAll(HSD_RObj* robj);
void HSD_RObjReqAnimAllByFlags(HSD_RObj* robj, f32 startframe, u32 flags);
void HSD_RObjReqAnimAll(HSD_RObj* robj, f32 startframe);
void HSD_RObjAddAnimAll(HSD_RObj* robj, HSD_RObjAnimJoint* anim);

void HSD_RObjRemove(HSD_RObj*);
void HSD_RObjRemoveAll(HSD_RObj*);
void HSD_RObjResolveRefs(HSD_RObj*, HSD_RObjDesc*);
void HSD_RObjResolveRefsAll(HSD_RObj*, HSD_RObjDesc*);
HSD_RObj* HSD_RObjLoadDesc(HSD_RObjDesc*);
void HSD_RObjSetConstraintObj(HSD_RObj* robj, void* obj);
void HSD_RObjUpdateAll(HSD_RObj* robj, void* obj, HSD_ObjUpdateFunc);
int HSD_RObjGetGlobalPosition(HSD_RObj* robj, int, Vec3* translate);

void HSD_RObjRemoveAnimByFlags(HSD_RObj* robj, u32 flags);
void HSD_RObjReqAnimByFlags(HSD_RObj* robj, f32 startframe, u32 flags);
void HSD_RObjAddAnim(HSD_RObj* robj, HSD_RObjAnimJoint* anim);
void HSD_RObjAnim(HSD_RObj* robj);

HSD_Rvalue* HSD_RvalueAlloc(void);
void HSD_RvalueRemove(HSD_Rvalue* rvalue);
void HSD_RvalueRemoveAll(HSD_Rvalue* rvalue);
void HSD_RvalueResolveRefs(HSD_Rvalue* rvalue, HSD_RvalueList* list);
void HSD_RvalueResolveRefsAll(HSD_Rvalue* rvalue, HSD_RvalueList* list);

static inline bool RObjHasFlags(HSD_RObj* robj)
{
    if ((robj->flags & ROBJ_TYPE_MASK) == 0) {
        return true;
    }
    return false;
}

static inline bool RObjHasFlags2(HSD_RObj* robj)
{
    if ((robj->flags & 0x80000000) != 0) {
        return true;
    }
    return false;
}

static inline bool RObjHasLimitReftype(HSD_RObj* robj)
{
    if ((robj->flags & ROBJ_TYPE_MASK) == REFTYPE_LIMIT) {
        return true;
    }
    return false;
}

#endif
