/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One trig implementation for every peer (musl's, see LICENSE). glibc,
 * mingw-w64 and bionic all return different bits for sinf/cosf/tanf/atanf,
 * which would desync netplay. Force-included into the game library
 * (compat.h) and aurora_mtx (CMakeLists.txt) so no call reaches the
 * platform libm. */
#ifndef PC_TRIG_H
#define PC_TRIG_H

#include <math.h>

float pc_sinf(float x);
float pc_cosf(float x);
float pc_tanf(float x);
float pc_atanf(float x);

#define sinf pc_sinf
#define cosf pc_cosf
#define tanf pc_tanf
#define atanf pc_atanf

/* MELEE_FP_PERTURB=<name> (CMakeLists.txt) re-routes one of them through the
 * 1-ULP wrapper in pc_perturb.c, which is how tools/net_determinism.py
 * proves it catches a libm divergence. Never on in a shipped build; see the
 * header comment of pc_perturb.c for the four fences. */
#ifdef MELEE_FP_PERTURB_SINF
float pc_perturb_sinf(float x);
#undef sinf
#define sinf pc_perturb_sinf
#endif
#ifdef MELEE_FP_PERTURB_COSF
float pc_perturb_cosf(float x);
#undef cosf
#define cosf pc_perturb_cosf
#endif
#ifdef MELEE_FP_PERTURB_TANF
float pc_perturb_tanf(float x);
#undef tanf
#define tanf pc_perturb_tanf
#endif
#ifdef MELEE_FP_PERTURB_ATANF
float pc_perturb_atanf(float x);
#undef atanf
#define atanf pc_perturb_atanf
#endif

#endif
