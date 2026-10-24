/* Exercise the real magnifier camera and copy callback without a GPU. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/if/ifmagnify.c"
#include "../src/pc/widescreen.c"

static HSD_GObj camera_gobj, fighter;
static HSD_CObj camera;
static HSD_ImageDesc image;
static VsSceneController controller;
static u32 render_width;
static int copies, draws;
static GXColor background = {0, 128, 255, 255};
uintptr_t OSBaseAddress;
uint32_t pc_register_ext_ptr(const void* p) {
    return 1;
}
u8 HSD_GObj_CameraKind;
HSD_GObj* HSD_GObj_804D7814;

HSD_GObj* GObj_Create(u16 kind, u8 link, u8 priority) {
    return &camera_gobj;
}
HSD_CObj* lb_80013B14(HSD_CameraDescPerspective* desc) {
    return &camera;
}
void HSD_GObjObject_80390A70(HSD_GObj* obj, u8 kind, void* data) {
    obj->hsd_obj = data;
}
void GObj_SetupGXLinkMax(HSD_GObj* obj, GObj_RenderFunc cb, u32 priority) {}
void HSD_CObjSetFlags(HSD_CObj* obj, u32 flags) {
    obj->flags |= flags;
}
u32 HSD_CObjGetFlags(HSD_CObj* obj) {
    return obj->flags;
}
void HSD_CObjSetOrtho(HSD_CObj* obj, f32 top, f32 bottom, f32 left, f32 right) {
    obj->projection_param.ortho.top = top;
    obj->projection_param.ortho.bottom = bottom;
    obj->projection_param.ortho.left = left;
    obj->projection_param.ortho.right = right;
}
void HSD_CObjGetOrtho(HSD_CObj* obj, f32* top, f32* bottom, f32* left, f32* right) {
    *top = obj->projection_param.ortho.top;
    *bottom = obj->projection_param.ortho.bottom;
    *left = obj->projection_param.ortho.left;
    *right = obj->projection_param.ortho.right;
}
void HSD_CObjSetViewport(HSD_CObj* obj, HSD_RectS16* rect) {}
void HSD_CObjSetScissorx4(HSD_CObj* obj, u16 left, u16 right, u16 top, u16 bottom) {}
bool HSD_CObjSetCurrent(HSD_CObj* obj) {
    return true;
}
void HSD_CObjEndCurrent(void) {}
void HSD_CObjSetInterest(HSD_CObj* obj, Vec3* pos) {}
void HSD_CObjSetEyePosition(HSD_CObj* obj, Vec3* pos) {}
void HSD_GObj_80390ED0(HSD_GObj* obj, u32 mask) {}
void HSD_SetEraseColor(u8 r, u8 g, u8 b, u8 a) {}
void HSD_CObjEraseScreen(HSD_CObj* obj, s32 color, s32 alpha, s32 depth) {}
VsSceneController* gmVs_GetSceneController(void) {
    return &controller;
}
bool ifAll_IsHUDHidden(void) {
    return false;
}
bool Camera_80030130(void) {
    return false;
}
HSD_GObj* Player_GetEntity(s32 slot) {
    return slot == 0 ? &fighter : NULL;
}
bool ftLib_IsOffscreen(HSD_GObj* obj) {
    return true;
}
bool ftLib_CanBeMagnified(HSD_GObj* obj) {
    return true;
}
f32 ftLib_80086B80(HSD_GObj* obj) {
    return 8.0f;
}
void ftLib_GetCameraSubjectBonePos(HSD_GObj* obj, Vec3* pos) {
    *pos = (Vec3){0};
}
void Player_80036978(s32 slot, Vec3* pos) {
    *pos = (Vec3){0};
}
float Stage_GetCamBoundsLeftOffset(void) {
    return -100;
}
float Stage_GetCamBoundsRightOffset(void) {
    return 100;
}
float Stage_GetCamBoundsTopOffset(void) {
    return 100;
}
float Stage_GetCamBoundsBottomOffset(void) {
    return -100;
}
#define COLOR(name)                                                                                \
    GXColor* name(void) {                                                                          \
        return &background;                                                                        \
    }
COLOR(Ground_801C0604)
COLOR(Ground_801C0618)
COLOR(Ground_801C062C)
COLOR(Ground_801C0640)
COLOR(Ground_801C0654)
COLOR(Ground_801C0668)
COLOR(Ground_801C067C)
COLOR(Ground_801C0690)
COLOR(Ground_801C06A4)
void ftDrawCommon_80080C28(HSD_GObj* obj, intptr_t pass) {
    ++draws;
}
void HSD_ImageDescCopyFromEFB(HSD_ImageDesc* desc, u16 x, u16 y, GXBool clear, bool sync) {
    /* Copy/clear must cover the entire 64x64 scratch viewport, including its
     * margins; otherwise temporary depth occludes the stage drawn afterward. */
    assert(x == 0 && y == 0 && desc->width == 64 && desc->height == 64);
    assert(clear && sync);
    ++copies;
}
void lb_800122C8(HSD_ImageDesc* desc, u16 x, u16 y, bool clear) {
    HSD_ImageDescCopyFromEFB(desc, x, y, clear, true);
}
HSD_RenderPass HSD_GetCurrentRenderPass(void) {
    return HSD_RP_SCREEN;
}
void AuroraGetRenderSize(u32* w, u32* h) {
    *w = render_width;
    *h = 1080;
}
void AuroraGetWindowSize(u32* w, u32* h) {
    *w = render_width;
    *h = 1080;
}
void AuroraSetPresentationAspect(float aspect) {}
void AuroraSetViewportPolicy(AuroraViewportPolicy policy) {}

int main(void) {
    image.width = image.height = 64;
    ifMagnify_804A1DE0.player[0].idesc = &image;
    controller.state.hud_enabled = 1;
    ifMagnify_802FC618();
    const u32 widths[] = {1314, 1920, 2560};
    for (unsigned i = 0; i < 3; ++i) {
        render_width = widths[i];
        /* The captured image fills fixed bubble UVs: widening its camera as
         * well would squeeze the fighter when the texture is displayed. */
        assert(pc_widescreen_cobj_scale(&camera) == 1.0f);
        copies = draws = 0;
        ifMagnify_802FBBDC(&camera_gobj, 0);
        assert(copies == 1 && draws == 3);
        assert(ifMagnify_804A1DE0.player[0].state.is_offscreen);
        assert(image.width == 64 && image.height == 64);
    }
    controller.state.hud_enabled = 0;
    copies = draws = 0;
    ifMagnify_802FBBDC(&camera_gobj, 0);
    assert(copies == 0 && draws == 0);
    assert(!ifMagnify_804A1DE0.player[0].state.is_offscreen);
    puts("PASS: magnifier fills its texture and clears its full scratch viewport at every aspect");
}
