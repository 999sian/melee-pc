#define __assert libc_assert
#include <assert.h>
#undef __assert
#include "../src/melee/gr/grkinokoroute.c"
static HSD_JObj expected;
static Ground ground;
static HSD_GObj map;
static int map_case, hidden, positioned, destroyed;
void HSD_JObjSetFlagsAll(HSD_JObj* jobj, u32 flags) {
    assert(jobj == &expected);
    assert(flags == JOBJ_HIDDEN);
    ++hidden;
}
Ground_GObj* Ground_GetMapGObj(int id) {
    assert(id == 3);
    return map_case ? &map : NULL;
}
void lb_8000B1CC(HSD_JObj* jobj, Vec3* in, Vec3* out) {
    assert(jobj == &expected);
    assert(in == NULL);
    *out = (Vec3){1, 2, 3};
    ++positioned;
}
void* efSync_Spawn(s32 effect, HSD_GObj* gobj, ...) {
    assert(effect == 0x442);
    return NULL;
}
void Camera_RequestQuake(CmQuakeKind kind, Vec* pos) {
    assert(kind == QuakeKind_Small);
    assert(pos == NULL);
}
void Ground_801C5414(int a, int b) {
    assert(a == 0x136);
    assert(b == 0xBA);
}
void grMaterial_801C8CDC(Item_GObj* gobj) {
    ++destroyed;
}
int main(void) {
    Item item = {0};
    HSD_GObj gobj = {0};
    item.xDD4_itemVar.yaku.x4_u.x4 = &expected;
    gobj.user_data = &item;
    for (map_case = 0; map_case < 3; ++map_case) {
        ground.u.kinokoroute2.flags_0 = 0;
        map.user_data = map_case == 2 ? &ground : NULL;
        grKinokoRoute_802084B4(&gobj);
        assert(ground.u.kinokoroute2.flags_0 == (map_case == 2));
    }
    assert(hidden == 3);
    assert(positioned == 3);
    assert(destroyed == 3);
}
