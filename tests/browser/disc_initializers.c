/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#ifdef MELEE_DISC_LOWERING
#define DISC __attribute__((annotate("melee_disc")))
#else
#define DISC __attribute__((scalar_storage_order("big-endian")))
#endif
struct DISC V {float x,y,z;};
struct DISC A {uint32_t a[2][2];struct V v;};
static struct A g={{{0x12345678,2},{3,4}},{1.25f,-3.5f,100}};
int main(void){float f=7.25f;struct V local={f,f+1,-f};printf("%x %u %.3f %.3f %.3f\n",g.a[0][0],g.a[1][1],g.v.y,local.x,local.z);for(unsigned i=0;i<sizeof(g);i++)printf("%02x",((unsigned char*)&g)[i]);puts("");}
