/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * High-refresh presentation by re-drawing the scene between ticks.
 *
 * Every tick still ends with its own exact frame (PC_INTERP_RECORD), during
 * which every joint whose world matrix the renderer asked for and every camera
 * it made current are remembered. On the next tick, before that tick's exact
 * frame, the scene is drawn N-1 more times (PC_INTERP_DRAW). For each of those
 * the remembered joints are brought up to date first, then their world matrix
 * is swapped for a blend of the remembered one and the current one; cameras get
 * the same treatment in HSD_CObjSetCurrent. Afterwards every swapped value is
 * put back, so the exact frame and the next tick see precisely what they would
 * have seen without this file. Nothing here writes simulation state that
 * survives an in-between frame: joint SRT, animation clocks and the RNG seed
 * are left (or put back) as they were.
 *
 * Blending is done on world matrices, not on joint SRT, so user-defined
 * matrices, IK, constraints and skinning all come along for free: the envelope
 * code reads the bone matrices it is handed. Rotation goes through quaternion
 * slerp so limbs do not shrink mid-swing; a jump that is too big to be motion
 * (a turnaround, a respawn, a camera cut, a reused pointer) shows the current
 * value instead.
 */
#include "compat.h"
#include "interp.h"
#include "pc.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_video.h>

#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mtx.h>
#include <sysdolphin/baselib/particle.h>
#include <sysdolphin/baselib/random.h>

int pc_interp_mode;

/* ---- configuration ------------------------------------------------------ */

static SDL_Window* s_window;
static int s_target_hz = 120; /* 60 = off, 0 = follow the display */
static int s_env_hz = -1;

void pc_interp_set_window(SDL_Window* window) {
    s_window = window;
}

void pc_interp_set_target_hz(int hz) {
    s_target_hz = hz < 0 ? 0 : hz;
}

int pc_interp_target_hz(void) {
    return s_target_hz;
}

static int display_hz(void) {
    if (s_window == NULL)
        return 60;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window));
    if (mode == NULL || mode->refresh_rate <= 0.0f)
        return 60;
    return (int)(mode->refresh_rate + 0.5f);
}

int pc_interp_subframes(void) {
    if (s_env_hz < 0) {
        /* MELEE_INTERP=0 turns it off; MELEE_INTERP_HZ=<hz> picks the rate. */
        const char* off = getenv("MELEE_INTERP");
        const char* hz = getenv("MELEE_INTERP_HZ");
        s_env_hz = off != NULL && atoi(off) == 0 ? 60 : hz != NULL ? atoi(hz) : 0;
    }
    /* 60 means off; 0 means as fast as the display. */
    int target = s_env_hz > 0 ? s_env_hz : s_target_hz;
    if (target == 60)
        return 1;
    /* With VSync a frame cannot be shown faster than the display refreshes,
     * and asking for more would stretch the tick itself. 119.88 and 143.9 Hz
     * modes round to the rate they are sold as. */
    int limit = aurora_vsync_enabled() ? display_hz() : 1000;
    if (target <= 0)
        target = display_hz();
    if (target > limit)
        target = limit;
    int sim_hz = (int)(1000000000ull / pc_sim_period_ns());
    int n = (target + 5) / sim_hz;
    if (n > 32)
        n = 32;
    return n < 1 ? 1 : n;
}

void pc_interp_update_present(void) {
    /* Several frames per tick are paced by the precise timer in src/pc/vi.c.
     * Blocking FIFO presentation on top of that loses a refresh whenever the
     * timer-driven pad alarm and the display drift apart, and at one frame per
     * refresh it cannot keep up at all, so VSync presents through MAILBOX:
     * still tear-free, never blocking. */
    aurora_set_vsync_mailbox(pc_interp_subframes() > 1);
}

/* ---- matrix blending ---------------------------------------------------- */

/* A world-space jump bigger than this in one tick is a teleport, not motion:
 * the fastest knockback moves a fighter well under half of it. */
#define SNAP_TRANSLATE 60.0f
/* Faster than ~115 degrees per tick is a turnaround or a snap, not a swing. */
#define SNAP_ROTATE 2.0f
#define CAM_SNAP_TRANSLATE 150.0f
#define CAM_SNAP_ROTATE 0.8f

typedef struct {
    float q[4]; /* x y z w */
    float s[3];
} RotScale;

static bool decompose(const float m[3][4], RotScale* out) {
    float c[3][3];
    for (int i = 0; i < 3; i++) {
        float x = m[0][i], y = m[1][i], z = m[2][i];
        float len = sqrtf(x * x + y * y + z * z);
        if (len < 1e-6f)
            return false;
        out->s[i] = len;
        c[i][0] = x / len;
        c[i][1] = y / len;
        c[i][2] = z / len;
    }
    /* Reject shear: the remaining 3x3 has to be a rotation (or a reflection). */
    float d01 = c[0][0] * c[1][0] + c[0][1] * c[1][1] + c[0][2] * c[1][2];
    float d02 = c[0][0] * c[2][0] + c[0][1] * c[2][1] + c[0][2] * c[2][2];
    float d12 = c[1][0] * c[2][0] + c[1][1] * c[2][1] + c[1][2] * c[2][2];
    if (fabsf(d01) > 0.02f || fabsf(d02) > 0.02f || fabsf(d12) > 0.02f)
        return false;
    float det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) -
                c[1][0] * (c[0][1] * c[2][2] - c[0][2] * c[2][1]) +
                c[2][0] * (c[0][1] * c[1][2] - c[0][2] * c[1][1]);
    if (det < 0.0f) {
        out->s[0] = -out->s[0];
        c[0][0] = -c[0][0];
        c[0][1] = -c[0][1];
        c[0][2] = -c[0][2];
    }
    /* Rotation matrix R[row][col] = c[col][row]. */
    float r00 = c[0][0], r01 = c[1][0], r02 = c[2][0];
    float r10 = c[0][1], r11 = c[1][1], r12 = c[2][1];
    float r20 = c[0][2], r21 = c[1][2], r22 = c[2][2];
    float tr = r00 + r11 + r22;
    float* q = out->q;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (r21 - r12) / s;
        q[1] = (r02 - r20) / s;
        q[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q[3] = (r21 - r12) / s;
        q[0] = 0.25f * s;
        q[1] = (r01 + r10) / s;
        q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q[3] = (r02 - r20) / s;
        q[0] = (r01 + r10) / s;
        q[1] = 0.25f * s;
        q[2] = (r12 + r21) / s;
    } else {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q[3] = (r10 - r01) / s;
        q[0] = (r02 + r20) / s;
        q[1] = (r12 + r21) / s;
        q[2] = 0.25f * s;
    }
    return true;
}

/* Slerp a -> b; false when the arc is longer than `limit` radians. */
static bool slerp(const float a[4], const float b_in[4], float t, float limit, float out[4]) {
    float b[4] = { b_in[0], b_in[1], b_in[2], b_in[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) {
        d = -d;
        for (int i = 0; i < 4; i++)
            b[i] = -b[i];
    }
    if (d > 1.0f)
        d = 1.0f;
    float theta = acosf(d);
    if (2.0f * theta > limit)
        return false;
    float wa, wb;
    if (theta < 1e-4f) {
        wa = 1.0f - t;
        wb = t;
    } else {
        float s = sinf(theta);
        wa = sinf((1.0f - t) * theta) / s;
        wb = sinf(t * theta) / s;
    }
    float len = 0.0f;
    for (int i = 0; i < 4; i++) {
        out[i] = wa * a[i] + wb * b[i];
        len += out[i] * out[i];
    }
    len = 1.0f / sqrtf(len);
    for (int i = 0; i < 4; i++)
        out[i] *= len;
    return true;
}

static void compose(const float q[4], const float s[3], const float t[3], float m[3][4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
        { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
        { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
    };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++)
            m[row][col] = r[row][col] * s[col];
        m[row][3] = t[row];
    }
}

/* out = blend(a, b, t) for world matrices. Returns false (and copies b) when
 * the change is a discontinuity rather than motion. */
static bool blend_mtx(float out[3][4], const float a[3][4], const float b[3][4], float t,
    float snap_translate, float snap_rotate) {
    float tr[3];
    float dist2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = b[i][3] - a[i][3];
        dist2 += d * d;
        tr[i] = a[i][3] + d * t;
    }
    if (dist2 > snap_translate * snap_translate)
        goto snap;
    RotScale ra, rb;
    if (decompose(a, &ra) && decompose(b, &rb)) {
        /* A mirror appearing or vanishing (facing flips done with scale) is a
         * cut. */
        if ((ra.s[0] < 0.0f) != (rb.s[0] < 0.0f))
            goto snap;
        float q[4], s[3];
        if (!slerp(ra.q, rb.q, t, snap_rotate, q))
            goto snap;
        for (int i = 0; i < 3; i++)
            s[i] = ra.s[i] + (rb.s[i] - ra.s[i]) * t;
        compose(q, s, tr, out);
        return true;
    }
    /* Degenerate or sheared (a joint scaled to zero to hide it, parent
     * non-uniform scale): a plain element blend is the honest answer. */
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 4; col++)
            out[row][col] = a[row][col] + (b[row][col] - a[row][col]) * t;
    return true;
snap:
    memcpy(out, b, sizeof(float) * 12);
    return false;
}

/* ---- joint table -------------------------------------------------------- */

/* Open-addressed pointer set, double-buffered: the table filled by one exact
 * frame is what the following in-between frames blend from, while the next
 * exact frame fills the other one. A slot belongs to the table's current
 * generation or is empty, so starting a frame costs nothing. */
#define JT_BITS 14
#define JT_CAP (1u << JT_BITS)
#define JT_MAX_LIVE (JT_CAP * 3 / 4)
#define JT_TOMBSTONE ((HSD_JObj*)(uintptr_t)1)

typedef struct {
    HSD_JObj* key;
    uint32_t gen;
    Mtx last;  /* world matrix when the exact frame finished */
    Mtx saved; /* the exact current matrix while a blend is swapped in */
} JSlot;

typedef struct {
    JSlot slots[JT_CAP];
    uint32_t list[JT_MAX_LIVE];
    uint32_t count;
    uint32_t gen;
} JTable;

static JTable s_jt[2];
static JTable* s_building = &s_jt[0];
static JTable* s_last = &s_jt[1];
static uint32_t s_gen_counter = 1;
static bool s_last_valid;

/* Every joint rebuilt while blended matrices are live -- in the pre-pass or
 * during the in-between frame itself -- is put back exactly as it was before:
 * flags (the dirty bit), SRT (constraints write it), world matrix and the
 * inherited-scale vector. Put back newest first, so a joint rebuilt twice ends
 * with its oldest record. Nothing the in-between frame computes survives it,
 * so the next tick reads the same joints it would have without it. */
#define DIRTY_MAX 8192
typedef struct {
    HSD_JObj* jobj;
    Vec3* scl;
    Vec3 scl_val;
    u8 fields[offsetof(HSD_JObj, scl) - offsetof(HSD_JObj, flags)];
} JSaved;
static JSaved s_dirty[DIRTY_MAX];
static uint32_t s_dirty_count;
static bool s_dirty_overflow;

static inline uint32_t jt_hash(const void* p) {
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 17;
    v *= 0xED5AD4BBu;
    v ^= v >> 11;
    return (uint32_t)v & (JT_CAP - 1);
}

static JSlot* jt_find(JTable* t, const HSD_JObj* key) {
    uint32_t i = jt_hash(key);
    for (uint32_t n = 0; n < JT_CAP; n++, i = (i + 1) & (JT_CAP - 1)) {
        JSlot* s = &t->slots[i];
        if (s->gen != t->gen)
            return NULL;
        if (s->key == key)
            return s;
    }
    return NULL;
}

static void jt_insert(JTable* t, HSD_JObj* key) {
    if (t->count >= JT_MAX_LIVE)
        return;
    uint32_t i = jt_hash(key);
    for (uint32_t n = 0; n < JT_CAP; n++, i = (i + 1) & (JT_CAP - 1)) {
        JSlot* s = &t->slots[i];
        if (s->gen != t->gen) {
            s->gen = t->gen;
            s->key = key;
            t->list[t->count++] = i;
            return;
        }
        if (s->key == key)
            return;
    }
}

static void jt_clear(JTable* t) {
    t->gen = ++s_gen_counter;
    t->count = 0;
}

void pc_interp_jobj_record(HSD_JObj* jobj) {
    jt_insert(s_building, jobj);
}

void pc_interp_jobj_recomputed(HSD_JObj* jobj) {
    if (s_dirty_count >= DIRTY_MAX) {
        s_dirty_overflow = true;
        return;
    }
    JSaved* d = &s_dirty[s_dirty_count++];
    d->jobj = jobj;
    d->scl = jobj->scl;
    if (jobj->scl != NULL)
        d->scl_val = *jobj->scl;
    memcpy(d->fields, &jobj->flags, sizeof d->fields);
}

static void dirty_restore(void) {
    for (uint32_t i = s_dirty_count; i-- > 0;) {
        JSaved* d = &s_dirty[i];
        HSD_JObj* j = d->jobj;
        if (j == NULL)
            continue;
        memcpy(&j->flags, d->fields, sizeof d->fields);
        if (d->scl == NULL) {
            if (j->scl != NULL) {
                HSD_VecFree(j->scl);
                j->scl = NULL;
            }
        } else {
            if (j->scl == NULL)
                j->scl = HSD_VecAlloc();
            *j->scl = d->scl_val;
        }
    }
    s_dirty_count = 0;
}

void pc_interp_jobj_released(HSD_JObj* jobj) {
    JSlot* s;
    if ((s = jt_find(s_building, jobj)) != NULL)
        s->key = JT_TOMBSTONE;
    if ((s = jt_find(s_last, jobj)) != NULL)
        s->key = JT_TOMBSTONE;
    for (uint32_t i = 0; i < s_dirty_count; i++)
        if (s_dirty[i].jobj == jobj)
            s_dirty[i].jobj = NULL;
}

/* ---- camera table ------------------------------------------------------- */

#define CT_MAX 64

typedef struct {
    HSD_CObj* key;
    /* Last exact frame. */
    Mtx view;
    float proj[4];
    float near_z, far_z;
    u8 proj_type;
    bool valid;
    /* Current in-between frame. */
    bool swapped;
    Mtx saved_view;
    u32 saved_flags;
    Mtx blend_view;
    float saved_proj[4];
    float saved_near, saved_far;
    bool proj_swapped;
} CSlot;

static CSlot s_cam_last[CT_MAX];
static CSlot s_cam_building[CT_MAX];
static int s_cam_last_n;
static int s_cam_building_n;

static CSlot* ct_find(CSlot* table, int n, const HSD_CObj* key) {
    for (int i = 0; i < n; i++)
        if (table[i].key == key && table[i].valid)
            return &table[i];
    return NULL;
}

void pc_interp_cobj_released(HSD_CObj* cobj) {
    CSlot* s;
    if ((s = ct_find(s_cam_last, s_cam_last_n, cobj)) != NULL)
        s->valid = false;
    if ((s = ct_find(s_cam_building, s_cam_building_n, cobj)) != NULL)
        s->valid = false;
}

static float s_t;

/* Before the projection is built: blend the projection parameters. */
void pc_interp_cobj_pre(HSD_CObj* cobj) {
    if (pc_interp_mode != PC_INTERP_DRAW)
        return;
    CSlot* s = ct_find(s_cam_last, s_cam_last_n, cobj);
    if (s == NULL || s->proj_type != cobj->projection_type)
        return;
    float* cur = &cobj->projection_param.perspective.fov;
    memcpy(s->saved_proj, cur, sizeof s->saved_proj);
    s->saved_near = cobj->near;
    s->saved_far = cobj->far;
    s->proj_swapped = true;
    for (int i = 0; i < 4; i++)
        cur[i] = s->proj[i] + (cur[i] - s->proj[i]) * s_t;
    cobj->near = s->near_z + (cobj->near - s->near_z) * s_t;
    cobj->far = s->far_z + (cobj->far - s->far_z) * s_t;
}

/* Right after GXSetProjection: the projection parameters go straight back so
 * nothing downstream can keep a blended one. */
void pc_interp_cobj_proj_done(HSD_CObj* cobj) {
    if (pc_interp_mode != PC_INTERP_DRAW)
        return;
    CSlot* s = ct_find(s_cam_last, s_cam_last_n, cobj);
    if (s == NULL || !s->proj_swapped)
        return;
    memcpy(&cobj->projection_param.perspective.fov, s->saved_proj, sizeof s->saved_proj);
    cobj->near = s->saved_near;
    cobj->far = s->saved_far;
    s->proj_swapped = false;
}

/* Invert a rigid view matrix into the camera's world matrix and back. */
static void rigid_inverse(const float v[3][4], float out[3][4]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r][c] = v[c][r];
    for (int r = 0; r < 3; r++)
        out[r][3] = -(out[r][0] * v[0][3] + out[r][1] * v[1][3] + out[r][2] * v[2][3]);
}

/* After the viewing matrix is current. */
void pc_interp_cobj_post(HSD_CObj* cobj) {
    if (pc_interp_mode == PC_INTERP_RECORD) {
        CSlot* s = ct_find(s_cam_building, s_cam_building_n, cobj);
        if (s == NULL) {
            if (s_cam_building_n >= CT_MAX)
                return;
            s = &s_cam_building[s_cam_building_n++];
            memset(s, 0, sizeof *s);
            s->key = cobj;
            s->valid = true;
        }
        memcpy(s->view, cobj->view_mtx, sizeof(Mtx));
        memcpy(s->proj, &cobj->projection_param.perspective.fov, sizeof s->proj);
        s->near_z = cobj->near;
        s->far_z = cobj->far;
        s->proj_type = cobj->projection_type;
        return;
    }
    if (pc_interp_mode != PC_INTERP_DRAW)
        return;
    CSlot* s = ct_find(s_cam_last, s_cam_last_n, cobj);
    if (s == NULL)
        return;
    if (!s->swapped) {
        memcpy(s->saved_view, cobj->view_mtx, sizeof(Mtx));
        s->saved_flags = cobj->flags;
        s->swapped = true;
        /* Blend the camera's world placement, not the view matrix: the eye
         * then travels in a straight line and turns about itself. */
        Mtx wa, wb, wo;
        rigid_inverse(s->view, wa);
        rigid_inverse(cobj->view_mtx, wb);
        blend_mtx(wo, wa, wb, s_t, CAM_SNAP_TRANSLATE, CAM_SNAP_ROTATE);
        rigid_inverse(wo, s->blend_view);
    }
    memcpy(cobj->view_mtx, s->blend_view, sizeof(Mtx));
}

/* ---- frame brackets ----------------------------------------------------- */

void pc_interp_reset(void) {
    jt_clear(s_building);
    jt_clear(s_last);
    s_last_valid = false;
    s_cam_last_n = 0;
    s_cam_building_n = 0;
    s_dirty_count = 0;
    pc_interp_mode = PC_INTERP_OFF;
}

bool pc_interp_ready(void) {
    return s_last_valid;
}

void pc_interp_record_begin(void) {
    jt_clear(s_building);
    s_cam_building_n = 0;
    pc_interp_mode = PC_INTERP_RECORD;
}

void pc_interp_record_end(void) {
    pc_interp_mode = PC_INTERP_OFF;
    /* Matrices are read once the whole frame is drawn: a joint recomputed
     * after it was first asked for ends up with its final value. */
    for (uint32_t i = 0; i < s_building->count; i++) {
        JSlot* s = &s_building->slots[s_building->list[i]];
        if (s->key != JT_TOMBSTONE)
            memcpy(s->last, s->key->mtx, sizeof(Mtx));
    }
    JTable* t = s_last;
    s_last = s_building;
    s_building = t;
    memcpy(s_cam_last, s_cam_building, sizeof(CSlot) * (size_t)s_cam_building_n);
    s_cam_last_n = s_cam_building_n;
    s_last_valid = true;
}

static u32 s_saved_seed;

/* Opaque regions the game side asked to have put back (pc_interp_keep). */
typedef struct {
    void* data;
    size_t size;
} Kept;
static Kept* s_kept;
static size_t s_kept_n, s_kept_cap;
static uint8_t* s_kept_buf;
static size_t s_kept_bytes, s_kept_buf_cap;

void pc_interp_keep(void* data, size_t size) {
    if (s_kept_n == s_kept_cap) {
        size_t cap = s_kept_cap ? s_kept_cap * 2 : 64;
        Kept* k = realloc(s_kept, cap * sizeof *k);
        if (k == NULL)
            return;
        s_kept = k;
        s_kept_cap = cap;
    }
    if (s_kept_bytes + size > s_kept_buf_cap) {
        size_t cap = (s_kept_bytes + size) * 2;
        uint8_t* b = realloc(s_kept_buf, cap);
        if (b == NULL)
            return;
        s_kept_buf = b;
        s_kept_buf_cap = cap;
    }
    s_kept[s_kept_n].data = data;
    s_kept[s_kept_n].size = size;
    memcpy(s_kept_buf + s_kept_bytes, data, size);
    s_kept_bytes += size;
    s_kept_n++;
}

static void kept_restore(void) {
    size_t at = 0;
    for (size_t i = 0; i < s_kept_n; i++) {
        memcpy(s_kept[i].data, s_kept_buf + at, s_kept[i].size);
        at += s_kept[i].size;
    }
    s_kept_n = 0;
    s_kept_bytes = 0;
}

/* Particles are not joints: each is a point integrated as pos += vel once per
 * tick, and the game's own trail code already takes pos - vel as where it was
 * a tick ago. An in-between frame draws each one that much of a step back,
 * shifted as psDispParticles reaches it -- only the links the game draws are
 * walked, because a link nothing draws may still hold a stale head. Tornado
 * particles orbit their generator with vel as polar parameters, so they are
 * left where they are. */
void pc_psdisp_interp(int begin); /* psdisp.c */
#define PTCL_MAX 4096
#define PTCL_SET 8192
static HSD_Particle* s_ptcl[PTCL_MAX];
static Vec3 s_ptcl_pos[PTCL_MAX];
static uint32_t s_ptcl_count;
static HSD_Particle* s_ptcl_set[PTCL_SET];

void pc_interp_particle(HSD_Particle* pp) {
    if (pp->kind & Tornado)
        return;
    uint32_t i = jt_hash(pp) & (PTCL_SET - 1);
    while (s_ptcl_set[i] != NULL) {
        if (s_ptcl_set[i] == pp)
            return; /* already shifted this frame (another camera) */
        i = (i + 1) & (PTCL_SET - 1);
    }
    if (s_ptcl_count >= PTCL_MAX)
        return;
    s_ptcl_set[i] = pp;
    s_ptcl[s_ptcl_count] = pp;
    s_ptcl_pos[s_ptcl_count] = pp->pos;
    s_ptcl_count++;
    float back = 1.0f - s_t;
    pp->pos.x -= pp->vel.x * back;
    pp->pos.y -= pp->vel.y * back;
    pp->pos.z -= pp->vel.z * back;
}

static void particles_restore(void) {
    for (uint32_t i = 0; i < s_ptcl_count; i++)
        s_ptcl[i]->pos = s_ptcl_pos[i];
    s_ptcl_count = 0;
    memset(s_ptcl_set, 0, sizeof s_ptcl_set);
}

void pc_interp_draw_begin(float t) {
    s_t = t;
    s_kept_n = 0;
    s_kept_bytes = 0;
    s_saved_seed = *HSD_RandSeedPtr;
    s_dirty_count = 0;
    /* Bring every remembered joint up to date with this tick first, while no
     * blended parent can leak into a child's recompute. The mode makes each
     * rebuild record what it overwrote, to be put back afterwards. */
    pc_interp_mode = PC_INTERP_DRAW;
    for (uint32_t i = 0; i < s_last->count; i++) {
        JSlot* s = &s_last->slots[s_last->list[i]];
        if (s->key != JT_TOMBSTONE)
            HSD_JObjSetupMatrix(s->key);
    }
    pc_interp_mode = PC_INTERP_OFF;
    for (uint32_t i = 0; i < s_last->count; i++) {
        JSlot* s = &s_last->slots[s_last->list[i]];
        if (s->key == JT_TOMBSTONE)
            continue;
        memcpy(s->saved, s->key->mtx, sizeof(Mtx));
        blend_mtx(s->key->mtx, s->last, s->saved, t, SNAP_TRANSLATE, SNAP_ROTATE);
    }
    for (int i = 0; i < s_cam_last_n; i++) {
        s_cam_last[i].swapped = false;
        s_cam_last[i].proj_swapped = false;
    }
    pc_psdisp_interp(1);
    pc_interp_mode = PC_INTERP_DRAW;
}

void pc_interp_draw_end(void) {
    pc_interp_mode = PC_INTERP_OFF;
    pc_psdisp_interp(0);
    particles_restore();
    for (uint32_t i = 0; i < s_last->count; i++) {
        JSlot* s = &s_last->slots[s_last->list[i]];
        if (s->key != JT_TOMBSTONE)
            memcpy(s->key->mtx, s->saved, sizeof(Mtx));
    }
    dirty_restore();
    for (int i = 0; i < s_cam_last_n; i++) {
        CSlot* s = &s_cam_last[i];
        if (s->valid && s->swapped) {
            memcpy(s->key->view_mtx, s->saved_view, sizeof(Mtx));
            s->key->flags = s->saved_flags;
        }
        s->swapped = false;
    }
    kept_restore();
    *HSD_RandSeedPtr = s_saved_seed;
}

void pc_interp_note_timing(uint64_t draw_ns, uint64_t present_ns) {
    static int enabled = -1;
    static uint64_t draw_sum, present_sum, draw_max, present_max, t0;
    static uint32_t count;
    if (enabled < 0)
        enabled = getenv("MELEE_FPS") != NULL;
    if (!enabled)
        return;
    draw_sum += draw_ns;
    present_sum += present_ns;
    if (draw_ns > draw_max)
        draw_max = draw_ns;
    if (present_ns > present_max)
        present_max = present_ns;
    count++;
    uint64_t now = pc_monotonic_ns();
    if (t0 == 0)
        t0 = now;
    if (now - t0 >= 1000000000ull) {
        pc_log_line("interp: %u subframes draw avg %.2f max %.2f ms, present avg %.2f max %.2f ms",
            count, draw_sum / 1e6 / count, draw_max / 1e6, present_sum / 1e6 / count,
            present_max / 1e6);
        draw_sum = present_sum = draw_max = present_max = 0;
        count = 0;
        t0 = now;
    }
}
