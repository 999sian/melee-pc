#ifndef PC_PC_H
#define PC_PC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Game-visible memory. Runtime structs carry 64-bit pointers, so give the
 * game's heaps more room than the GameCube's 24MB. */
#define PC_MEM1_SIZE (96u * 1024 * 1024)
#define PC_ARAM_SIZE (16u * 1024 * 1024)

void pc_platform_init(void);

/* Frame boundary: presents the current frame, pumps events, starts the next
 * frame and runs due OSAlarms. Called from VIWaitForRetrace. */
void pc_frame_boundary(void);

/* Set once the window is closed; the game loop is expected to exit. */
extern bool pc_exit_requested;

/* Vertex array byte sizes for aurora's GXSetArray (src/pc/vtxarray.c).
 * pc_vtx_array_scan walks a PObj's display list at load time; the size of
 * an indexed attribute array is then (max index + 1) * stride. */
struct HSD_PObjDesc;
void pc_vtx_array_scan(const struct HSD_PObjDesc* desc);
uint32_t pc_vtx_array_size(const void* data);

/* GX/VI entry points the game uses that aurora does not declare
 * (implemented in src/pc/gx.c and src/pc/vi.c). */
struct _GXFogAdjTable;
void GXWaitDrawDone(void);
void GXInitFogAdjTable(struct _GXFogAdjTable* table, uint16_t width, float projmtx[4][4]);
uint16_t VIPadFrameBufferWidth(uint16_t width);

#ifdef __cplusplus
}
#endif

#endif
