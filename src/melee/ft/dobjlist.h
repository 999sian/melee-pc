#ifndef MELEE_FT_DOBJLIST_H
#define MELEE_FT_DOBJLIST_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>
#include <sysdolphin/baselib/forward.h>

/**
 * @todo This was split out of ft/types.h to break a circular dependency with
 * fighter-specific types.h headers. Figure out a better place for it.
 */

struct DObjList {
    u32 count;
    HSD_DObj** data;
};

struct CostumeTObjList {
    /*  fp+5CC */ u32 n_costume_tobjs;
    /*  fp+5D0 */ DiscU16* x5D0; ///< points into Pl*.dat (ftData_x8_x8::xC)
    /*  fp+5D4 */ HSD_TObj* costume_tobjs[5];
};

/* In Pl*.dat: per-model lists of DObj index groups. */
struct DISC_STRUCT TempS {
    int x0;
    DISC_PTR(u8) x4;
};
DISC_ASSERT_SIZE(struct TempS, 8);

struct DISC_STRUCT FtPartsVisLookup {
    int x0;
    DISC_PTR(TempS) x4;
};
DISC_ASSERT_SIZE(struct FtPartsVisLookup, 8);

struct FtPartsVis {
    /* fp+5AC */ u32 model_num;
    /* fp+5B0 */ u8 cleared[5];
    /* fp+5B8 */ FtPartsVisLookup* xC[5];
};

#endif
