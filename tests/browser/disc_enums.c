/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#ifdef MELEE_DISC_LOWERING
#define DISC __attribute__((annotate("melee_disc")))
#else
#define DISC __attribute__((scalar_storage_order("big-endian")))
#endif
enum Kind{A=-3,B=0x12345678};
struct DISC Data{enum Kind kind,values[2];};
static struct Data d={B,{A,B}};
int main(void){printf("%x %d %x ",d.kind,d.values[0],d.values[1]);d.kind=A;d.values[1]=B;printf("%d ",d.kind);for(unsigned i=0;i<sizeof(d);i++)printf("%02x",((unsigned char*)&d)[i]);puts("");}
