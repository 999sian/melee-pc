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

#ifdef __cplusplus
}
#endif

#endif
