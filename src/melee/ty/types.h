#ifndef MELEE_TY_TYPES_H
#define MELEE_TY_TYPES_H

#include <Runtime/platform.h>

#include <melee/ty/forward.h> // IWYU pragma: export
#include <sysdolphin/baselib/forward.h>

#include <placeholder.h>

#include <dolphin/gx/GXStruct.h>
#include <dolphin/mtx.h>

struct TySortElem {
    s32 key;
    f32 val;
};

struct TyModeState {
    s8 x0;
    u8 x1;
    u8 x2;
    u8 x3;
    s8 x4;
    u8 x5;
    u16 x6;
    u16 x8;
    u16 xA;
};

struct ToyAnimState {
    /* 0x00 */ struct HSD_GObj* gobj;
    /* 0x04 */ struct HSD_JObj* jobj[2];
    /* 0x0C */ s16 xC;
    /* 0x0E */ s8 x0E;
    /* 0x0F */ s8 x0F;
    /* 0x10 */ s8 x10;
    /* 0x11 */ s8 x11;
    /* 0x12 */ u8 pad_12[2];
};
ASSERT_SIZE(ToyAnimState, 0x14);

/* Used by _Toy_803109A0 for table lookup */
struct ToyEntry {
    s32 id;
    union {
        s8 value_byte;
        s32 value;
    };
};

/* One row of tyInitModelTbl / tyInitModelDTbl in TyDataI.dat, read in place,
 * so it keeps the disc byte order. Size 0x24. Declared native, `id` never
 * matched and the `id != -1` scan in Toy_803060BC walked off the table. */
struct DISC_STRUCT TrophyData {
    s32 id;
    s32 x04;
    f32 x08;
    f32 x0C;
    f32 x10;
    f32 x14;
    f32 x18;
    f32 x1C;
    s8 x20;
    s8 x21;
    s8 x22;
    s8 x23;
};
DISC_ASSERT_SIZE(struct TrophyData, 0x24);

/* Trophy list entry. Size: 0x34 bytes. */
struct TyListArg {
    /* 0x00 */ struct TyListArg* links[3];
    /* 0x0C */ struct HSD_JObj* jobjs[3];
    /* 0x18 */ struct HSD_Text* texts[3];
    /* 0x24 */ s8 x24;
    /* 0x25 */ u8 x25;
    /* 0x26 */ s16 idx;
    /* 0x28 */ int x28;
    /* 0x2C */ float x2C;
    /* 0x30 */ float x30;
};

struct Toy {
    /*   +0 */ char pad_0[0x4];
    /*   +4 */ int x4;
    /*   +8 */ int x8;
    /*   +C */ char pad_C[0x40 - 0xC];
    /*  +40 */ Vec3 translate;
    /*  +4C */ Vec3 offset;
    /*  +58 */ char pad_58[0x194 - 0x58];
    /* +194 */ s32 x194;
    /* +198 */ char pad_198[0x19A - 0x198];
    /* +19A */ u16 x19A;
    /* +19C */ u16 x19C;
    /* +19E */ u16 trophyTable[TY_TROPHY_COUNT];
    /* +3E8 */ char pad_3E8[0x3EC - 0x3E8];
    /* +3EC */ s16 trophyCount;
};
// TODO: This struct should only be 0x58
// STATIC_ASSERT(sizeof(struct Toy) == 0x58);

/* One row of tyDisplayModelTbl / tyDisplayModelUsTbl in TyDataI.dat, read in
 * place by tyDisplay_8031B9DC, so it keeps the disc byte order. Declared
 * native, x00 never matched the requested id, the scan ran to the -1
 * terminator row and callers then used its garbage x04 to index the
 * 44-entry archive arrays. */
struct DISC_STRUCT TyDspEntry {
    /* 0x00 */ s32 x00;
    /* 0x04 */ u8 x04;
    /* 0x05 */ u8 x05;
    /* 0x06 */ u8 pad_06[2];
    /* 0x08 */ f32 x08;
    /* 0x0C */ f32 x0C;
};
DISC_ASSERT_SIZE(struct TyDspEntry, 0x10);

/// View of #Toy_sbss_804D6ED4 (see #TyLightArray_).
struct TyFiguponED4 {
    /* 0x00 */ HSD_GObj* x0;
    /* 0x04 */ HSD_GObj* x4;
    /* 0x08 */ void* pad_08;
    /* 0x0C */ HSD_GObj* xC;
};

struct ToyListEntry {
    /* 0x00 */ struct ToyListEntry* prev;
    /* 0x04 */ struct ToyListEntry* next;
    /* 0x08 */ char* archive_name;
    /* 0x0C */ char* symbol_name;
    /* 0x10 */ s16 trophy_id;
    /* 0x12 */ u8 pad_12[2];
    /* 0x14 */ HSD_Archive* archive;
};

/* GC size 0x158. Neither byte-counted pad here was padding: the 0x138 bytes
 * at the front are the entry array the list code walks (toy.c reached them
 * through a second "ToyDisplayList" view of the same object), and
 * 0x144..0x154 hold four text handles that Toy_80310660 stores and frees.
 * With 8-byte pointers a pad sized in GameCube bytes no longer lands
 * first_entry at the end of the entry array, so name both runs. */
struct TyDisplayData {
    /* 0x000 */ ToyListEntry entries[13];
    /* 0x138 */ ToyListEntry* first_entry;
    /* 0x13C */ ToyListEntry* last_entry;
    /* 0x140 */ ToyListEntry* selected_entry;
    /* 0x144 */ HSD_Text* x144;
    /* 0x148 */ HSD_Text* x148;
    /* 0x14C */ HSD_Text* x14C;
    /* 0x150 */ HSD_Text* x150;
    /* 0x154 */ s16 selectedIdx;
    /* 0x156 */ u8 pad_156;
    /* 0x157 */ s8 visible_count;
};
ASSERT_SIZE(struct TyDisplayData, 0x158);

struct Toy26B8 {
    /* 0x000 */ Vec3 x0;
    /* 0x00C */ char devtext_buf_00C[0x8C];
    /* 0x098 */ char devtext_buf_098[0xFC];
    /* 0x194 */ u8 x194;
    /* 0x195 */ s8 x195;
    /* 0x196 */ s8 x196;
    /* 0x197 */ u8 x197;
    /* 0x198 */ u8 x198;
    /* 0x199 */ u8 pad_199;
    /* 0x19A */ u16 x19A;
    /* 0x19C */ u16 x19C;
    /* 0x19E */ u16 trophy_flags[TY_TROPHY_COUNT];
    /* 0x3E8 */ s16 selectedIdx;
    /* 0x3EA */ s16 selectedTrophyId;
    /* 0x3EC */ s16 trophy_count;
    /* 0x3EE */ u8 pad_3EE[0x3F0 - 0x3EE];
    /* 0x3F0 */ union {
        ToyAnimState anim;
        HSD_GObj* x3F0;
    };
};

/* GC size 0x8. pad_0 was the GObj slot toy_make_gobj creates and
 * GObj_SetupGXLink registers; it has to be a real pointer or x4 lands past
 * the 8-byte allocation on a 64-bit host. */
struct TyViewData {
    /* +0 */ HSD_GObj* x0;
    /* +4 */ s8 x4;
    /* +5 */ char pad_5[0x3];
};
ASSERT_SIZE(struct TyViewData, 0x8);

struct TyFiguponData {
    /* 0x00 */ HSD_GObj* x0;
    /* 0x04 */ HSD_GObj* x4;
    /* 0x08 */ HSD_GObj* x8;
    /* 0x0C */ u8 pad_0C[0x4];
    /* 0x10 */ HSD_GObjProc* x10; /* holds the HSD_GObj_SetupProc result;
                                   * declared s32 it truncated the pointer */
    /* 0x14 */ HSD_Text* x14;
    /* 0x18 */ HSD_Text* x18;
    /* 0x1C */ u8 pad_1C[0x4];
    /* 0x20 */ s32 x20;
    /* 0x24 */ s32 x24;
    /* 0x28 */ u8 x28;
    /* 0x29 */ u8 x29;
};

struct TyDspPos {
    /* 0x00 */ f32 x;
    /* 0x04 */ f32 z;
};

struct TyDspGrid {
    /* 0x000 */ s32 x00;
    /* 0x004 */ f32 x04_min_x;
    /* 0x008 */ f32 x08_min_z;
    /* 0x00C */ f32 x0C_max_x;
    /* 0x010 */ f32 x10_max_z;
    /* 0x014 */ TySortElem sort[301];
    /* 0x97C */ TyDspPos pos[301];
};

struct TyDspConfig {
    /* 0x00 */ HSD_GObj* x00;
    /* 0x04 */ u8 pad_04[4];
    /* 0x08 */ s32 x08;
    /* 0x0C */ f32 x0C;
    /* 0x10 */ f32 x10;
    /* 0x14 */ u8 pad_14[4];
    /* 0x18 */ f32 x18;
    /* 0x1C */ f32 x1C;
    /* 0x20 */ f32 x20;
    /* 0x24 */ f32 x24;
    /* 0x28 */ u8 pad_28[8];
    /* 0x30 */ f32 x30;
    /* 0x34 */ f32 x34;
    /* 0x38 */ u8 pad_38[8];
    /* 0x40 */ f32 x40;
    /* 0x44 */ f32 x44;
    /* 0x48 */ f32 x48;
    /* 0x4C */ f32 x4C;
    /* 0x50 */ f32 x50;
    /* 0x54 */ f32 x54;
    /* 0x58 */ f32 x58;
    /* 0x5C */ Vec3 x5C;
    /* 0x68 */ Vec3 x68;
    /* 0x74 */ s8 x74;
    /* 0x75 */ u8 x75;
    /* 0x76 */ u8 x76;
    /* 0x77 */ u8 pad_77[1];
    /* 0x78 */ HSD_GObj* x78;
    /* 0x7C */ s32 x7C;
};

struct TyDspArchNames {
    const char* entries[43];
};

struct TyDspNameTables {
    const char* jobj_names[43];
    const char* matanim_names[43];
    TyDspArchNames arch_names;
    s32 terminator;
};

struct TyDspBgData {
    /* 0x00 */ HSD_GObj* gobj0;
    /* 0x04 */ HSD_GObj* gobj4;
    /* 0x08 */ u8 pad_08[4];
    /* 0x0C */ HSD_JObj* jobj;
    /* 0x10 */ u8 pad_10[0x3C];
    /* 0x4C */ HSD_Archive* archive;
    /* 0x50 */ HSD_Archive* archives[43];
    /* 0xFC */ u8 pad_FC[8];
    /* 0x104 */ s16 x104;
    /* 0x106 */ u8 pad_106[2];
};

/// Same layout as #ToyListEntry (Toy_80308250 fills both).
struct TyDspArchiveHolder {
    /*  +0 */ UNK_T x0;
    /*  +4 */ void* pad_4;
    /*  +8 */ char* archive_name;
    /*  +C */ char* symbol_name;
    /* +10 */ s16 trophy_id;
    /* +12 */ u8 pad_12[2];
    /* +14 */ HSD_Archive* archive;
};

struct TyDspBaseData {
    /* 0x00 */ char x00[0x38];
};

struct TyListGobjEntry {
    /*  +0 */ HSD_GObj* x0;
    /*  +4 */ HSD_GObj* x4;
    /*  +8 */ u8 pad_8[0x0C - 0x08];
    /*  +C */ s8 x0C;
    /*  +D */ s8 x0D;
    /*  +E */ u8 pad_0E;
    /*  +F */ s8 x0F;
    /* +10 */ s8 x10;
    /* +11 */ s8 x11;
    /* +12 */ s8 x12;
    /* +13 */ s8 x13;
    /* +14 */ s8 x14;
    /* +15 */ u8 pad_15;
    /* +16 */ s8 x16;
};

struct DigitInit {
    s32 x0, x4, x8, xC;
};

/// View of #Toy_sbss_804D6ED4 (see #TyLightArray_).
struct TyLightData {
    /* 0x00 */ void* pad;
    /* 0x04 */ HSD_GObj* gobj;
    /* 0x08 */ void* pad8;
    /* 0x0C */ HSD_Archive* archive;
};

/* One row of tyModelSortTbl in TyDataI.dat, read in place by
 * _Toy_803064B8 (never written), so it keeps the disc byte order. */
struct DISC_STRUCT ToyNameData {
    s16 x0;
    s16 x2;
    s16 x4;
    s16 x6;
    s16 x8;
    s16 xA;
};
DISC_ASSERT_SIZE(struct ToyNameData, 0xC);

struct tyUnkStruct {
    /* 0x00 */ HSD_GObj* x0;
    /* 0x04 */ void* x4;
    /* 0x08 */ HSD_GObj* x8;
};

struct TyCleanupObj {
    /* 0x00 */ void* x0;
    /* 0x04 */ void* x4;
    /* 0x08 */ void* x8;
    /* 0x0C */ void* xC;
    /* 0x10 */ void* x10;
};

/// Allocated type of #Toy_sbss_804D6ED4 (0xE4 bytes on GC); #TyLightData,
/// #ToyCameraControl, #tyUnkStruct and #TyFiguponED4 are views of it.
struct TyLightArray_ {
    /* 0x00 */ HSD_GObj* x0;
    /* 0x04 */ HSD_GObj* x4;
    /* 0x08 */ HSD_GObj* x8;
    /* 0x0C */ HSD_Archive* archive;
    /* 0x10 */ s32 x10;
    /* 0x14 */ f32 x14;
    /* 0x18 */ f32 x18;
    /* 0x1C */ Vec3 pos[8];
    /* 0x7C */ Vec3 interest[8];
    /* 0xDC */ s8 xDC[8];
};

/// View of #Toy_sbss_804D6ED4 (see #TyLightArray_).
struct ToyCameraControl {
    /*  +0 */ HSD_GObj* x00;
    /*  +4 */ HSD_GObj* x04;
    /*  +8 */ HSD_GObj* x08;
    /*  +C */ void* pad_0C;
    /* +10 */ s32 x10;
    /* +14 */ f32 x14;
    /* +18 */ f32 x18;
};
ASSERT_SIZE(ToyCameraControl, 0x1C);

struct ToyTransitionObj {
    u8 pad[0x20];
    s32 x20;
    s32 x24;
};

struct Toy6E68 {
    ToyTransitionObj* x0;
    ToyTransitionObj* x4;
    HSD_GObj* x8;
    ToyTransitionObj* xC;
    /* 0x10..0x18 was pad: Toy_80310660 frees the slots at +0x10 and +0x14 as
     * GObjs while walking this object as an array of pointers, so they have
     * to be pointers here too or x18 sits at the wrong host offset. */
    HSD_GObj* x10;
    HSD_GObj* x14;
    f32 x18;
    f32 x1C;
    f32 x20;
    f32 x24;
    f32 x28;
    f32 x2C;
    f32 x30;
    f32 x34;
    f32 x38;
    f32 x3C;
    f32 x40;
    f32 x44;
    f32 x48;
    f32 x4C;
    f32 x50;
    f32 x54;
    s32 x58;
    s32 x5C;
    s8 x60;
    s8 x61;
};

/* Trophy list UI state. Size: 0x2D8 bytes. */
struct TyListState {
    /* 0x000 */ TyListArg entries[12]; /* 12 * 0x34 = 0x270 */
    /* 0x270 */ struct TyListArg* x270;
    /* 0x274 */ struct TyListArg* x274;
    /* 0x278 */ struct TyListArg* x278;
    /* 0x27C */ struct HSD_GObj* gobj;
    /* 0x280 */ u8 pad_280[0x8];
    /* 0x288 */ struct HSD_JObj* x288;
    /* 0x28C */ struct HSD_JObj* jobj;
    /* 0x290 */ struct HSD_Text* x290;
    /* 0x294 */ u8 pad_294[4];
    /* 0x298 */ s16 selectedIdx;
    /* 0x29A */ s8 entryCount;
    /* 0x29B */ s8 x29B;
    /* 0x29C */ s8 x29C;
    /* 0x29D */ s8 x29D;
    /* 0x29E */ s8 x29E;
    /* 0x29F */ s8 x29F;
    /* 0x2A0 */ s8 x2A0;
    /* 0x2A1 */ s8 x2A1;
    /* 0x2A4 */ float x2A4;
    /* 0x2A8 */ float x2A8;
};
ASSERT_SIZE(struct TyListState, 0x2AC);

struct SisFontData {
    u8 pad[0x4E8];
    u8* digits;
};

/* GC size 0x5C. The bytes from 0x10 to 0x34 are one nine-element joint array:
 * lb_8001204C fills it from &x10 with the nine indices in _Toy_803FE3F8, and
 * the slot at +0x30 is read back to drive the panel animation. Naming only
 * three of the nine slots left the rest inside pad runs, whose GameCube byte
 * counts stop agreeing with the pointer stride once pointers are 8 bytes.
 * This is also the one declaration for this object - it used to be viewed
 * through ToyGlobalsS_, TyArchiveData and tyLightData as well, which each
 * placed the archive pointer at a different host offset. */
struct ToyED8Data {
    /*  +0 */ HSD_GObj* x0;
    /*  +4 */ HSD_GObj* gobj;
    /*  +8 */ HSD_GObj* x8;
    /*  +C */ HSD_GObj* gobj2;
    /* +10 */ HSD_JObj* jobjs[9];
    /* +34 */ u8 pad_34[0x50 - 0x34];
    /* +50 */ HSD_Archive* archive;
    /* +54 */ HSD_Archive* x54;
    /* +58 */ HSD_Archive* x58;
};
ASSERT_OFFSET(struct ToyED8Data, x0, 0x0);
ASSERT_OFFSET(struct ToyED8Data, gobj, 0x4);
ASSERT_OFFSET(struct ToyED8Data, gobj2, 0xC);
ASSERT_OFFSET(struct ToyED8Data, jobjs, 0x10);
ASSERT_OFFSET(struct ToyED8Data, archive, 0x50);
ASSERT_OFFSET(struct ToyED8Data, x54, 0x54);
ASSERT_SIZE(struct ToyED8Data, 0x5C);

struct TyFiguponInner {
    u8 pad[0x4D];
    u8 x4D;
};

struct TyFiguponUD {
    /* 0x00 */ u32 x0;
    /* 0x04 */ u32 x4;
    /* 0x08 */ s32 x8;
    /* 0x0C */ u8 pad_0C[0x4];
    /* 0x10 */ s32 x10;
    /* 0x14 */ s32 x14;
    /* 0x18 */ s32 x18;
    /* 0x1C */ u8 pad_1C[0xC];
    /* 0x28 */ s32 x28;
    /* 0x2C */ s32 x2C;
    /* 0x30 */ s32 x30;
    /* 0x34 */ u8 pad_34[0x10];
    /* 0x44 */ f32 x44;
    /* 0x48 */ u8 pad_48[0x10];
};

struct ToyParamEditor {
    /* 0x00 */ HSD_GObj* gobj;
    /* 0x04 */ u8 selected_slot;
    /* 0x05 */ u8 repeat_delay;
    /* 0x06 */ s16 values[9];
};

struct ToyTable {
    ToyEntry entries[9];
};

struct PosArray {
    s32 xy[2];
};

struct PosArrayFull {
    PosArray a[7];
};

struct lbl_803FDDE4_t {
    struct {
        char* name;
        UNK_T empty;
    } symbols[6];
    struct {
        int index;
        GXColor color;
        bool flag;
    } values[6];
};
ASSERT_SIZE(struct lbl_803FDDE4_t, 0x78);

#endif
