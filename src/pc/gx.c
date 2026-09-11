/* GX entry points aurora declares but does not implement. */
#include <dolphin/gx.h>

#include <string.h>

void GXSetTevClampMode(GXTevStageID stage, GXTevClampMode mode)
{
    (void) stage;
    (void) mode;
}

void GXSetMisc(GXMiscToken token, u32 val)
{
    (void) token;
    (void) val;
}

void GXSetVerifyLevel(int level)
{
    (void) level;
}

void GXAbortFrame(void) {}

static u16 s_draw_sync_token;

void GXSetDrawSync(u16 token)
{
    s_draw_sync_token = token;
}

u16 GXReadDrawSync(void)
{
    return s_draw_sync_token;
}

/* GXSetDrawDone posts a token that fires the draw-done callback once the FIFO
 * worker reaches it. GXDrawDone drains synchronously, which implies it. */
void GXWaitDrawDone(void)
{
    GXDrawDone();
}

/* Fog adjustment for non-square projections; aurora's fog is analytic. */
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, f32 projmtx[4][4])
{
    (void) width;
    (void) projmtx;
    memset(table, 0, sizeof(*table));
}

