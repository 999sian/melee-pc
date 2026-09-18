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

#endif
