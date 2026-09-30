/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * High-refresh presentation. The simulation stays at 60 Hz; between two ticks
 * the scene is drawn N-1 extra times with every joint world matrix and every
 * camera blended between the last presented tick and the current one, then
 * the tick's own frame is drawn exactly as before (src/pc/interp.c).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct HSD_JObj;
struct HSD_CObj;
struct SDL_Window;

enum {
    PC_INTERP_OFF = 0,
    PC_INTERP_RECORD = 1, /* the tick's exact frame: remember what it drew */
    PC_INTERP_DRAW = 2,   /* an in-between frame: blended state is swapped in */
};
extern int pc_interp_mode;

void pc_interp_set_window(struct SDL_Window* window);
/* Frames presented per 60 Hz tick (1 = interpolation off). */
int pc_interp_subframes(void);
/* Once per tick: pick the presentation mode interpolation needs. */
void pc_interp_update_present(void);
/* Target presentation rate in Hz, 0 = follow the display. Persisted by the
 * settings overlay; MELEE_INTERP_HZ overrides it. */
void pc_interp_set_target_hz(int hz);
int pc_interp_target_hz(void);

/* Scene lifetime: every recorded pointer dies with the scene's heaps. */
void pc_interp_reset(void);

/* Bracket the tick's exact render. */
void pc_interp_record_begin(void);
void pc_interp_record_end(void);
/* True once a frame has been recorded that the next tick can blend from. */
bool pc_interp_ready(void);

/* Bracket one in-between render at blend factor t in (0, 1). */
void pc_interp_draw_begin(float t);
void pc_interp_draw_end(void);
/* After pc_interp_draw_begin: `size` bytes at `data` are put back as they are
 * now when the in-between frame ends. */
void pc_interp_keep(void* data, size_t size);

/* MELEE_FPS: per-second averages of drawing and presenting a subframe. */
void pc_interp_note_timing(uint64_t draw_ns, uint64_t present_ns);

/* Hooks from sysdolphin. */
void pc_interp_jobj_record(struct HSD_JObj* jobj);
void pc_interp_jobj_recomputed(struct HSD_JObj* jobj);
void pc_interp_jobj_released(struct HSD_JObj* jobj);
void pc_interp_cobj_pre(struct HSD_CObj* cobj);
void pc_interp_cobj_proj_done(struct HSD_CObj* cobj);
void pc_interp_cobj_post(struct HSD_CObj* cobj);
void pc_interp_cobj_released(struct HSD_CObj* cobj);

/* src/pc/vi.c: present the in-between frame just drawn and open the next one,
 * paced to `k` N-ths of the way through the current tick. */
void pc_vi_present_subframe(int k, int n);
void pc_vi_trace(const char* what);
/* MELEE_INTERP_AUDIT (src/pc/net_snapshot.c). */
void pc_interp_audit_begin(void);
void pc_interp_audit_end(void);

#ifdef __cplusplus
}
#endif
