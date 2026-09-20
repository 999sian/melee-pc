/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef MELEE_DISC_LOWERING
#define DISC __attribute__((annotate("melee_disc")))
#else
#define DISC __attribute__((scalar_storage_order("big-endian")))
#endif
struct DISC Data { uint32_t u; float f; uint16_t a[3]; unsigned x:3,y:7,z:6; };
int main(void) { struct Data d; memset(&d,0,sizeof(d)); d.u=0x12345678; d.f=1.5f;d.a[1]=0xabcd;d.x=5;d.y=71;d.z=45; d.u+=3; unsigned old=d.a[1]++; printf("%x %.2f %x %x %u %u %u %zu\n",d.u,d.f,old,d.a[1],d.x,d.y,d.z,sizeof(d)); for(unsigned i=0;i<sizeof(d);i++)printf("%02x",((unsigned char*)&d)[i]);puts(""); }
