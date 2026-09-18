/* Minimal subset of musl's src/internal/libm.h for the vendored trig
 * functions (see LICENSE in this directory). */
#ifndef PC_LIBM_H
#define PC_LIBM_H

#include <stdint.h>
#include <float.h>
#include <math.h>

/* musl's code assumes float/double are evaluated at their own precision.
 * Every target we ship (x86-64 SSE, AArch64) satisfies this; a platform
 * that does not would silently compute different bits. */
#if FLT_EVAL_METHOD != 0
#error "pc libm requires FLT_EVAL_METHOD == 0"
#endif

#define predict_false(x) __builtin_expect(x, 0)

static inline void fp_force_evalf(float x)
{
	volatile float y;
	y = x;
}

static inline void fp_force_eval(double x)
{
	volatile double y;
	y = x;
}

#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		fp_force_evalf(x);                \
	} else {                                  \
		fp_force_eval(x);                 \
	}                                         \
} while(0)

#define asuint(f) ((union{float _f; uint32_t _i;}){f})._i

#define GET_FLOAT_WORD(w,d)                       \
do {                                              \
  (w) = asuint(d);                                \
} while (0)

float pc_sindf(double);
float pc_cosdf(double);
float pc_tandf(double, int);
int pc_rem_pio2f(float, double *);
int pc_rem_pio2_large(double *, double *, int, int, int);

#endif
