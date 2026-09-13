#include "../src/pc/widescreen.c"
#include <stdlib.h>
#define assert(x) do { if (!(x)) abort(); } while (0)
#include <stdio.h>
HSD_VIInfo HSD_VIData = {.current.vi.rmode = {.viWidth=640,.viHeight=480}};
static float viewport[4];
static u32 scissor[4];
static HSD_RenderPass test_pass = HSD_RP_SCREEN;
HSD_RenderPass HSD_GetCurrentRenderPass(void) { return test_pass; }
void AuroraSetViewportPolicy(AuroraViewportPolicy policy) { (void)policy; }
void AuroraGetRenderSize(u32* w,u32* h) { *w=1920; *h=1080; }
void GXSetViewportRender(float x,float y,float w,float h,float n,float f) {
    (void)n; (void)f; viewport[0]=x; viewport[1]=y; viewport[2]=w; viewport[3]=h;
}
void GXSetScissorRender(u32 x,u32 y,u32 w,u32 h) {
    scissor[0]=x; scissor[1]=y; scissor[2]=w; scissor[3]=h;
}
static void near(float a, float b) { assert(fabsf(a-b) < 0.01f); }
int main(void) {
    PcWideRect r = pc_widescreen_fit(1920,1080,4.0f/3.0f);
    near(r.x,240); near(r.y,0); near(r.width,1440); near(r.height,1080);
    r = pc_widescreen_fit(2560,1080,16.0f/9.0f);
    near(r.x,320); near(r.width,1920);
    r = pc_widescreen_fit(1080,1920,16.0f/9.0f);
    near(r.width,1080); near(r.height,607.5f); near(r.y,656.25f);
    r = pc_widescreen_fit(0,1080,16.0f/9.0f); near(r.width,0);
    near(pc_widescreen_aspect(0,true,21.0f/9,4.0f/3),4.0f/3);
    near(pc_widescreen_aspect(1,true,21.0f/9,4.0f/3),16.0f/9);
    near(pc_widescreen_aspect(2,true,21.0f/9,4.0f/3),21.0f/9);
    near(pc_widescreen_aspect(2,true,0.5f,4.0f/3),4.0f/3);
    near(pc_widescreen_aspect(1,false,21.0f/9,4.0f/3),4.0f/3);
    near(pc_widescreen_aspect(99,true,21.0f/9,4.0f/3),4.0f/3);
    HSD_CObj camera = {0};
    camera.projection_type = PROJ_PERSPECTIVE;
    camera.projection_param.perspective.aspect = 4.0f/3;
    camera.viewport.xmax=640; camera.viewport.ymax=480;
    camera.scissor.right=640; camera.scissor.bottom=480;
    HSD_CObj original = camera;
    float projection[4][4] = {{2,0,0,0},{0,3,0,0},{0,0,4,5},{0,0,-1,0}};
    pc_widescreen_set_mode(1); pc_widescreen_set_scene(true);
    pc_widescreen_apply_camera(&camera,projection);
    near(projection[0][0],1.5f); near(projection[1][1],3);
    near(camera.projection_param.perspective.aspect,original.projection_param.perspective.aspect);
    near(viewport[0],0); near(viewport[2],1920); assert(scissor[2]==1920);
    /* Every on-screen camera of an eligible scene widens the same way. */
    HSD_CObj hud=original; hud.projection_type=PROJ_ORTHO;
    projection[0][0]=2;
    pc_widescreen_apply_camera(&hud,projection);
    near(projection[0][0],1.5f); near(viewport[0],0); near(viewport[2],1920);
    /* Cameraless HUD text keeps the original aspect, centred. */
    pc_widescreen_apply_screen();
    near(viewport[0],240); near(viewport[2],1440); assert(scissor[0]==240);
    /* Offscreen passes keep GX's own mapping and projection. */
    test_pass=HSD_RP_OFFSCREEN;
    viewport[0]=-1; projection[0][0]=2;
    pc_widescreen_apply_camera(&camera,projection);
    near(viewport[0],-1); near(projection[0][0],2); near(pc_widescreen_scale(),1);
    test_pass=HSD_RP_SCREEN;
    /* Unsupported scenes fall back to a centred original-aspect frame. */
    pc_widescreen_set_scene(false);
    pc_widescreen_apply_camera(&camera,projection);
    near(viewport[0],240); near(viewport[2],1440);
    near(projection[0][0],2); near(pc_widescreen_scale(),1);
    puts("PASS: widescreen fit, aspect modes and scene fallback");
}
