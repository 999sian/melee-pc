#ifndef GALE01_0C97A8
#define GALE01_0C97A8

#include <melee/ft/forward.h>

/* 0C97A8 */ bool ftCo_800C97A8(Fighter_GObj* gobj);
/* 0C97DC */ bool ftCo_Turn_CheckInput(Fighter_GObj* gobj);
/* 0C9840 */ void ftCo_Turn_Enter(Fighter_GObj*, FtMotionId, MotionFlags,
                                  float, float, float);
/* 0C98AC */ void ftCo_Turn_Enter_Basic(Fighter_GObj* gobj);
/* 0C9924 */ void ftCo_Turn_Anim_Inner(Fighter_GObj* gobj);
/* 0C9970 */ void ftCo_Turn_Anim(Fighter_GObj* gobj);
/* 0C99F8 */ void ftCo_Turn_IASA(Fighter_GObj* gobj);
/* 0C9BEC */ void ftCo_Turn_Phys(Fighter_GObj* gobj);
/* 0C9C0C */ void ftCo_Turn_Coll(Fighter_GObj* gobj);
/* 0C9C2C */ bool fn_800C9C2C(Fighter_GObj* gobj);
/* 0C9C74 */ void ftCo_Turn_Enter_Smash(Fighter_GObj* gobj);

/// UCF: octagon-clamped raw stick X per port, [0] this frame .. [2] two ago.
extern s8 ftCo_ucf_raw_x[4][3];
bool ftCo_UcfDashback(Fighter* fp);

#endif
