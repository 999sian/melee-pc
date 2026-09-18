/* SPDX-License-Identifier: GPL-3.0-or-later */
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
/* Simulation frame period the boundary paces to (60.000 Hz), src/pc/vi.c. */
uint64_t pc_sim_period_ns(void);

/* Append a line to the diagnostic log (src/pc/main.c), so frame stalls
 * interleave with aurora's own records and can be attributed to whatever
 * the engine reported around them. */
void pc_log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Keyboard -> virtual controller (src/pc/keyboard.c). */
typedef union SDL_Event SDL_Event;
void pc_keyboard_event(const SDL_Event* e);
void pc_keyboard_apply(void);
void pc_touch_apply(void);

/* Set once the window is closed; the game loop is expected to exit. */
extern bool pc_exit_requested;

/* Vertex array byte sizes for aurora's GXSetArray (src/pc/vtxarray.c).
 * pc_vtx_array_scan walks a PObj's display list at load time; the size of
 * an indexed attribute array is then (max index + 1) * stride. */
struct HSD_PObjDesc;
void pc_vtx_array_scan(const struct HSD_PObjDesc* desc);
uint32_t pc_vtx_array_size(const void* data);

/* Decode one THP-JPEG frame into GX I8-tiled Y/U/V planes (src/pc/thp.c). */
void pc_thp_decode_frame(const void* jpeg, void* tile_y, void* tile_u, void* tile_v);

/* GX/VI entry points the game uses that aurora does not declare
 * (implemented in src/pc/gx.c and src/pc/vi.c). */
struct _GXFogAdjTable;
void GXInitFogAdjTable(struct _GXFogAdjTable* table, uint16_t width, float projmtx[4][4]);
uint16_t VIPadFrameBufferWidth(uint16_t width);

/* Feature queries */
bool pc_is_custom_textures_enabled(void);
bool pc_is_unlock_all_enabled(void);
bool pc_is_frozen_stadium_enabled(void);
bool pc_is_free_camera_enabled(void);
int pc_get_hud_mode(void);
float pc_get_music_volume(void);
float pc_get_sfx_volume(void);

/* The whole unlock surface the RNG-visible unlock predicates read, packed
 * into one scalar: chars:16 | stages:16 | features:8 | latch4:8 | latch5:8 |
 * latch6:8. Implemented in src/melee/gm/gmmain_lib.c, next to the save-data
 * layout and the NUM_UNLOCKABLE_* constants that define it; netplay
 * snapshots it, forces pc_unlock_state_all() for the session and puts it
 * back at disconnect (src/pc/net_handshake.c). */
uint64_t pc_unlock_state_get(void);
void pc_unlock_state_set(uint64_t state);
uint64_t pc_unlock_state_all(void);
/* Per-install random id, generated once and kept in launcher.cfg
 * (src/pc/launcher.cpp); the LAN lobby's host election key. */
uint64_t pc_install_id(void);
/* Build identity peers must share to play (the app version, src/pc/version.cpp). */
const char* pc_app_rev(void);

/* Audio volume control */
void pc_audio_set_volume(float volume);
void pc_audio_set_music_volume(float volume);
void pc_audio_set_sfx_volume(float volume);
float pc_audio_get_music_volume(void);
float pc_audio_get_sfx_volume(void);

/* Texture replacements (src/pc/textures.cpp) */
void pc_textures_init(void);
void pc_textures_reload(void);
void pc_textures_shutdown(void);
const char* pc_textures_get_path(void);

#ifdef __cplusplus
}
#endif

#endif
