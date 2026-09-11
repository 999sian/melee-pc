#include "ftCo_Attack100.h"
#include <melee/ft/types.h>

bool ftCo_800D72A0(Fighter* fp)
{
    struct Fighter_x2D0_t* p = fp->x2D0;
    enum_t states[2];
    s32 i;

    states[0] = p->x2C;
    states[1] = p->x30;
    for (i = 0; i < 2; i++) {
        if (states[i] != -1) {
            if (states[i] <= fp->motion_id &&
                fp->motion_id < states[i] + p->x28)
            {
                return true;
            }
        }
    }
    return false;
}
