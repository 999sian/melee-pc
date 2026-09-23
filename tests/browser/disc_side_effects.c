/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef MELEE_DISC_LOWERING
#define DISC __attribute__((annotate("melee_disc")))
#else
#define DISC __attribute__((scalar_storage_order("big-endian")))
#endif
struct DISC V{uint32_t n;};
struct DISC D{unsigned a:3,b:5;int s:7;unsigned z:1;uint16_t data[3];union DISC {uint32_t value;struct DISC{uint16_t hi,lo;};};struct V child;};
static struct D d;static int calls;
static struct D* next(void){calls++;return &d;}
int main(void){memset(&d,0,sizeof(d));next()->value=0x1234abcd;next()->data[1]+=5;unsigned a=next()->a=10;unsigned b=next()->a++;d.s=-19;d.child.n=0x87654321;printf("%d %u %u %u %x %x %d %x\n",calls,a,b,d.a,d.hi,d.lo,d.s,d.child.n);for(unsigned i=0;i<sizeof(d);i++)printf("%02x",((unsigned char*)&d)[i]);puts("");}
