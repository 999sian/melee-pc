#ifndef _pobj_h_
#define _pobj_h_

#include <Runtime/platform.h>

#include <sysdolphin/baselib/forward.h> // IWYU pragma: export

#include <dolphin/gx/GXEnum.h>
#include <dolphin/mtx.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/class.h>
#include <sysdolphin/baselib/list.h>

#define HSD_MTX_RIGID 1
#define HSD_MTX_ENVELOPE 2

struct _unk_struct_pobj {
    u32 data[8];
    HSD_AObj* aobj;
};

struct HSD_PObj {
    HSD_Class parent;
    HSD_PObj* next;
    HSD_VtxDescList* verts;
    u16 flags;
    u16 n_display;
    /// #u8 primitive, #u16 vtxcnt, #u16* indices
    u8* display;
    union HSD_PObjUnion {
        HSD_JObj* jobj;
        HSD_ShapeSet* shape_set;
        HSD_SList* envelope_list;
        struct _unk_struct_pobj* unk;
    } u;
};

struct DISC_STRUCT HSD_PObjDesc {
    DISC_PTR(char) class_name;
    DISC_PTR(HSD_PObjDesc) next;
    DISC_PTR(HSD_VtxDescList) verts;
    u16 flags;
    u16 n_display;
    DISC_PTR(u8) display; /* raw GX display list */
    union DISC_STRUCT {
        DISC_PTR(HSD_Joint) joint;
        DISC_PTR(HSD_ShapeSetDesc) shape_set;
        DISC_PTR(DiscU32) envelope_p; /* NULL-terminated DISC_PTR(HSD_EnvelopeDesc)[] */
    } u;
};
DISC_ASSERT_SIZE(HSD_PObjDesc, 0x18);

struct DISC_STRUCT HSD_VtxDescList {
    GXAttr attr;
    GXAttrType attr_type;
    GXCompCnt comp_cnt;
    GXCompType comp_type;
    u8 frac;
    u16 stride;
    DISC_PTR(void) vertex; /* raw GX vertex array */
};
DISC_ASSERT_SIZE(HSD_VtxDescList, 0x18);

struct HSD_Envelope {
    HSD_Envelope* next;
    HSD_JObj* jobj;
    f32 weight;
};

struct DISC_STRUCT HSD_EnvelopeDesc {
    DISC_PTR(HSD_Joint) joint;
    f32 weight;
};
DISC_ASSERT_SIZE(HSD_EnvelopeDesc, 0x8);

struct HSD_ShapeSet {
    u16 flags;
    u16 nb_shape;
    int nb_vertex_index;
    HSD_VtxDescList* vertex_desc;
    DiscU32* vertex_idx_list; /* disc array of DISC_PTR(u8) index arrays */
    s32 nb_normal_index;
    HSD_VtxDescList* normal_desc;
    DiscU32* normal_idx_list;
    union {
        f32* bp;
        f32 bl;
    } blend;
    HSD_AObj* aobj;
};

struct DISC_STRUCT HSD_ShapeSetDesc {
    u16 flags;
    u16 nb_shape;
    s32 nb_vertex_index;
    DISC_PTR(HSD_VtxDescList) vertex_desc;
    DISC_PTR(DiscU32) vertex_idx_list;
    s32 nb_normal_index;
    DISC_PTR(HSD_VtxDescList) normal_desc;
    DISC_PTR(DiscU32) normal_idx_list;
};
DISC_ASSERT_SIZE(HSD_ShapeSetDesc, 0x1C);

struct DISC_STRUCT HSD_ShapeAnim {
    DISC_PTR(HSD_ShapeAnim) next;
    DISC_PTR(HSD_AObjDesc) aobjdesc;
};
DISC_ASSERT_SIZE(HSD_ShapeAnim, 0x8);

struct DISC_STRUCT HSD_ShapeAnimJoint {
    DISC_PTR(HSD_ShapeAnimJoint) child;
    DISC_PTR(HSD_ShapeAnimJoint) next;
    DISC_PTR(HSD_ShapeAnimDObj) shapeanimdobj;
};
DISC_ASSERT_SIZE(HSD_ShapeAnimJoint, 0xC);

struct HSD_PObjInfo {
    HSD_ClassInfo parent;
    void (*disp)(HSD_PObj* pobj, Mtx vmtx, Mtx pmtx, u32 rendermode);
    void (*setup_mtx)(HSD_PObj* pobj, Mtx vmtx, Mtx pmtx, u32 rendermode);
    s32 (*load)(HSD_PObj* pobj, HSD_PObjDesc* desc);
};

extern HSD_PObjInfo hsdPObj;

#define HSD_POBJ(o) ((HSD_PObj*) (o))
#define HSD_POBJ_INFO(i) ((HSD_PObjInfo*) (i))
#define HSD_POBJ_METHOD(o) HSD_POBJ_INFO(HSD_CLASS_METHOD(o))

HSD_PObjInfo* HSD_PObjGetDefaultClass(void);
void HSD_PObjSetDefaultClass(HSD_PObjInfo* info);
HSD_PObj* HSD_PObjAlloc(void);
void HSD_PObjFree(HSD_PObj*);

u32 HSD_PObjGetFlags(HSD_PObj* pobj);
void HSD_PObjRemoveAnimAllByFlags(HSD_PObj* pobj, u32 flags);
void HSD_PObjReqAnimByFlags(HSD_PObj* pobj, f32 startframe, u32 flags);
void HSD_PObjReqAnimAllByFlags(HSD_PObj* pobj, f32 startframe, u32 flags);
void HSD_ClearVtxDesc(void);
HSD_PObj* HSD_PObjLoadDesc(HSD_PObjDesc*);

void HSD_PObjClearMtxMark(void* obj, u32 mark);
void HSD_PObjSetMtxMark(int idx, void* obj, u32 mark);
void HSD_PObjGetMtxMark(int idx, void** obj, u32* mark);
void HSD_PObjAddAnim(HSD_PObj*, HSD_ShapeAnim*);
void HSD_PObjAddAnimAll(HSD_PObj*, HSD_ShapeAnim*);
void HSD_PObjAnim(HSD_PObj* pobj);
void HSD_PObjAnimAll(HSD_PObj*);
void HSD_PObjResolveRefs(HSD_PObj*, HSD_PObjDesc*);
void HSD_PObjResolveRefsAll(HSD_PObj*, HSD_PObjDesc*);
void HSD_PObjRemove(HSD_PObj*);
void HSD_PObjRemoveAll(HSD_PObj*);

void HSD_PObjRemoveAnimByFlags(HSD_PObj* pobj, u32 flags);

void HSD_PObjDisp(HSD_PObj* pobj, Mtx vmtx, Mtx pmtx, u32 rendermode);

#endif
