#define __assert libc_assert
#include <assert.h>
#undef __assert
#include "../src/melee/ft/kinds/ftCommon/ftCo_ItemScopeStart.c"
#include <string.h>
static bool empty;
static FtMotionId last_motion;
bool it_8026B594(Item_GObj* gobj) {
    return empty;
}
void Fighter_ChangeMotionState(Fighter_GObj* gobj, FtMotionId msid, MotionFlags flags, f32 start,
    f32 speed, f32 blend, Fighter_GObj* arg) {
    last_motion = msid;
}
void ftAnim_8006EBA4(Fighter_GObj* gobj) {}
void ftCommon_ClampAirDrift(Fighter* fp) {}
void ftCommon_8007E79C(Fighter_GObj* gobj, s32 arg) {}
int it_80291DAC(Item_GObj* gobj, int charge) {
    return -1;
}
void it_80291F14(Item_GObj* gobj, int arg) {}
void ftCommon_8007E7E4(Fighter_GObj* gobj, bool arg) {}
int main(void) {
    Fighter fp;
    HSD_GObj gobj = {0};
    unsigned char cpu_before[sizeof(fp.cpu)];
    for (int air = 0; air < 2; ++air) {
        for (int depleted = 0; depleted < 2; ++depleted) {
            memset(&fp, 0xA5, sizeof(fp));
            fp.gobj = &gobj;
            gobj.user_data = &fp;
            fp.item_gobj = NULL;
            fp.mv.co.itemscope.timer = 77.0F;
            fp.mv.co.itemscope.flag = 1;
            memcpy(cpu_before, &fp.cpu, sizeof(cpu_before));
            empty = depleted;
            if (air)
                ft_800D7770(&gobj);
            else
                ft_800D76B8(&gobj);
            assert(fp.mv.co.itemscope.timer == 0.0F);
            assert(fp.mv.co.itemscope.flag == 0);
            assert(memcmp(cpu_before, &fp.cpu, sizeof(cpu_before)) == 0);
            assert(last_motion ==
                   (air ? (empty ? ftCo_MS_ItemScopeAirStartEmpty : ftCo_MS_ItemScopeAirStart) :
                          (empty ? ftCo_MS_ItemScopeStartEmpty : ftCo_MS_ItemScopeStart)));
            assert(fp.take_dmg_cb == (HSD_GObjEvent)fn_800D7938);
        }
    }
}
