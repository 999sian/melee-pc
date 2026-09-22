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
/* Sleep (at most 1 ms) toward the next due OSAlarm instead of spinning for it,
 * and a 0.1 ms sleep for the game's disc wait loops, src/pc/os.c. */
void pc_os_wait_alarm(void);
void pc_os_yield(void);

/* Append a line to the diagnostic log (src/pc/main.c), so frame stalls
 * interleave with aurora's own records and can be attributed to whatever
 * the engine reported around them. */
void pc_log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Keyboard -> virtual controller (src/pc/keyboard.c). */
typedef union SDL_Event SDL_Event;
void pc_keyboard_event(const SDL_Event* e);
void pc_keyboard_apply(void);
void pc_touch_apply(void);

/* GameCube adapter (WUP-028) read directly for raw 8-bit values
 * (src/pc/gcadapter.c). init runs before SDL_Init so SDL's own driver leaves
 * the adapter alone; poll runs on the 1000 Hz thread; status hands the
 * origin-relative pad for a port to the virtual-pad merge (keyboard.c). */
void pc_gcadapter_init(void);
void pc_gcadapter_poll(void);
void pc_gcadapter_apply(void); /* main-thread publication to virtual pads */
struct PADStatus;
bool pc_gcadapter_status(int port, struct PADStatus* out);
/* HUD snapshot: raw[6] = stick x,y  c-stick x,y  L R as the adapter reports
 * them (0-255). Returns false when no controller is in that slot. Safe from
 * any thread. */
bool pc_gcadapter_raw(int port, uint8_t raw[6], bool* wireless);
uint64_t pc_gcadapter_report_count(void);

/* Input latency meter (src/pc/input_poll.c): the 1000 Hz thread stamps each
 * sample; pc_frame_boundary records how old the sample the sim is about to
 * consume is. Stats cover the last 600 frames; any out-param may be NULL. */
uint64_t pc_monotonic_ns(void);
void pc_input_latency_record(void);
bool pc_input_latency(float* mean_ms, float* max_ms, float* p99_ms);
/* MELEE_INPUT_HUD=1: in-match controller diagnostic text (if/ifinput.c) and
 * a log summary every 600 frames. */
bool pc_is_input_hud_enabled(void);

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
bool pc_is_ucf_enabled(void);
int pc_get_hud_mode(void);
float pc_get_music_volume(void);
float pc_get_sfx_volume(void);
/* Build version string ("v0.1.8-beta"), src/pc/version.cpp. */
const char* pc_app_version(void);

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

/* Pipeline prewarm (src/pc/vi.c): blocks up to max_wait_ms while background
 * shader-pipeline compiles drain, returns how many are still pending. Call
 * from a load screen; 0 only polls. */
uint32_t pc_gfx_prewarm(uint32_t max_wait_ms);

#ifdef __cplusplus
}
#endif

#endif
