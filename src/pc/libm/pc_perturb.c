/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One-ULP perturbation of one routed trig function, for the determinism
 * harness only.
 *
 * M0 asks for "an injected 1-ULP libm change is caught". It cannot be done
 * from outside the process: pc_trig.h routes sinf/cosf/tanf/atanf to the
 * vendored musl code in this directory and lbtrigf.c defines atan2f/asinf/
 * acosf in the game itself, so every math symbol the simulation uses is
 * defined inside the executable and LD_PRELOAD has nothing to interpose.
 * The hook therefore has to be a build-time one: configure with
 *
 *     cmake -B <dir> -DMELEE_FP_PERTURB=sinf
 *
 * and sinf returns one ULP off everywhere the simulation calls it, which is
 * what a peer with a different platform libm does. Such a build desyncs
 * against every other build by design, so it is fenced in four ways: the
 * option is OFF by default, CMake warns when it is on, src/pc/net.c logs
 * "net: FP PERTURB ..." on every run, and tools/package_windows.sh refuses
 * to package an image containing that marker.
 *
 * ponytail: one ULP away from zero on finite non-zero results is enough to
 * prove the harness is sensitive to a libm difference; a real one is many
 * ULP across many functions. If a future test needs a specific wrong value,
 * give perturb() a table rather than widening the option. */
#include "pc_trig.h"

#include <math.h>
#include <stdint.h>

/* Read by src/pc/net.c so every run reports how often the hook fired: a
 * perturbed build whose function is never called would otherwise look like
 * a clean pass. Defined unconditionally so this stays a valid translation
 * unit when the option is off. */
unsigned pc_fp_perturb_calls;

#if defined(MELEE_FP_PERTURB_SINF) || defined(MELEE_FP_PERTURB_COSF) ||                            \
    defined(MELEE_FP_PERTURB_TANF) || defined(MELEE_FP_PERTURB_ATANF)

static float perturb(float r) {
    union {
        float f;
        uint32_t i;
    } u = { .f = r };
    if (!isfinite(r) || r == 0.0f) {
        return r; /* 0, inf and nan stay exact: +1 would change their class */
    }
    u.i += 1; /* one ULP away from zero, whatever the sign */
    pc_fp_perturb_calls++;
    return u.f;
}

#ifdef MELEE_FP_PERTURB_SINF
float pc_perturb_sinf(float x) {
    return perturb(pc_sinf(x));
}
#endif
#ifdef MELEE_FP_PERTURB_COSF
float pc_perturb_cosf(float x) {
    return perturb(pc_cosf(x));
}
#endif
#ifdef MELEE_FP_PERTURB_TANF
float pc_perturb_tanf(float x) {
    return perturb(pc_tanf(x));
}
#endif
#ifdef MELEE_FP_PERTURB_ATANF
float pc_perturb_atanf(float x) {
    return perturb(pc_atanf(x));
}
#endif

#endif
