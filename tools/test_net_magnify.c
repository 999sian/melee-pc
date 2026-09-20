/* The render callback is deliberately omitted between simulation ticks.
 * Query the shipping gameplay predicate with stale/contradictory render bits. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/if/ifmagnify.c"
static bool online = true, hidden, eligible = true, onscreen = true, camera_updated;
static HSD_GObj fighter, camera;
static VsSceneController controller;
bool pc_net_active(void) {
    return online;
}
VsSceneController* gmVs_GetSceneController(void) {
    return &controller;
}
bool ifAll_IsHUDHidden(void) {
    return hidden;
}
bool Camera_80030130(void) {
    return false;
}
HSD_GObj* Player_GetEntity(s32 slot) {
    return slot == 0 ? &fighter : NULL;
}
bool ftLib_80086ED0(HSD_GObj* gobj) {
    assert(gobj == &fighter);
    return eligible;
}
CmSubject* ftLib_80086B74(HSD_GObj* gobj) {
    assert(gobj == &fighter);
    return (CmSubject*)&fighter;
}
HSD_GObj* Camera_80030A50(void) {
    return &camera;
}
void Camera_8002A4AC(HSD_GObj* gobj) {
    assert(gobj == &camera);
    camera_updated = true;
}
bool Camera_80030CD8(CmSubject* subject, S32Vec2* out) {
    assert(subject == (CmSubject*)&fighter && out == NULL && camera_updated);
    camera_updated = false;
    return onscreen;
}
#ifdef TEST_OLD_PREDICATE
#define ifMagnify_IsOffscreenForDamage ifMagnify_802FC998
#endif
int main(void) {
    controller.state.hud_enabled = 1;
    /* A presented; B restored a prior offscreen render. The fighter is now
     * onscreen in both simulations. Neither may accumulate offscreen damage. */
    for (unsigned stale = 0; stale < 2; stale++) {
        ifMagnify_804A1DE0.player[0].state.is_offscreen = stale;
        for (unsigned tick = 0; tick < 120; tick++)
            assert(!ifMagnify_IsOffscreenForDamage(0));
    }
    onscreen = false;
    for (unsigned stale = 0; stale < 2; stale++) {
        ifMagnify_804A1DE0.player[0].state.is_offscreen = stale;
        for (unsigned tick = 0; tick < 120; tick++)
            assert(ifMagnify_IsOffscreenForDamage(0));
    }
    eligible = false;
    assert(!ifMagnify_IsOffscreenForDamage(0));
    eligible = true;
    ifMagnify_804A1DE0.player[0].state.ignore_offscreen = 1;
    assert(!ifMagnify_IsOffscreenForDamage(0));
    ifMagnify_804A1DE0.player[0].state.ignore_offscreen = 0;
    hidden = true;
    assert(!ifMagnify_IsOffscreenForDamage(0));
    hidden = false;
    assert(!ifMagnify_IsOffscreenForDamage(1));
    online = false;
    for (unsigned drawn = 0; drawn < 2; drawn++) {
        ifMagnify_804A1DE0.player[0].state.is_offscreen = drawn;
        assert(ifMagnify_IsOffscreenForDamage(0) == drawn);
    }
    puts("PASS: online magnifier damage independent of render history; camera refresh, visibility "
         "gates and offline semantics preserved");
}
