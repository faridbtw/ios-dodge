/* SPDX-License-Identifier: BSD-2-Clause
 * Native Peach v2 CE register survey. Register geometry from pinned Qualcomm
 * ISC headers msmhwiobase.h/soc_ce_seq_hwioreg.h and CNSS reg.h/pci.c.
 * Only BAR remap selection is written; CE registers and DMA rings are read-only.
 */
#ifndef OP13_WIFI_CE_H
#define OP13_WIFI_CE_H
#include <stdint.h>
#include <stddef.h>
#define OP13_CE_WINDOW 0x3278u
#define OP13_CE_BASE 0x01b80000u
struct op13_ce_io {void *cookie;int(*read)(void*,uint32_t,uint32_t*);int(*write)(void*,uint32_t,uint32_t);};
struct op13_ce_snapshot {uint32_t saved_window,selected_window;uint32_t values[4][2][7];unsigned count,restored;};
static int op13_ce_bar_allowed(uint32_t offset,int write)
{
 if(offset==OP13_CE_WINDOW)return 1;
 if(write||offset<0x80000||offset>=0x88000||(offset&3))return 0;
 unsigned local=(offset-0x80000)&0xfff;
 return local==0||local==4||local==8||local==12||local==16||local==0x400||local==0x404;
}
static int op13_ce_snapshot(const struct op13_ce_io *io,int ready,struct op13_ce_snapshot *s)
{
 if(!ready||!io||!io->read||!io->write||!s)return -1;
 *s=(struct op13_ce_snapshot){0};
 if(io->read(io->cookie,OP13_CE_WINDOW,&s->saved_window)||s->saved_window==~0u)return -2;
 s->selected_window=(s->saved_window&~0x3fu)|0x40000000u|((OP13_CE_BASE>>19)&0x3f);
 int rc=0;uint32_t value;
 /* A failed write may have landed: always try to restore the captured value. */
 if(io->write(io->cookie,OP13_CE_WINDOW,s->selected_window)||
    io->read(io->cookie,OP13_CE_WINDOW,&value)||value!=s->selected_window)rc=-3;
 static const uint32_t offsets[7]={0,4,8,12,16,0x400,0x404};
 for(unsigned ce=0;!rc&&ce<4;ce++)for(unsigned kind=0;!rc&&kind<2;kind++)for(unsigned i=0;!rc&&i<7;i++){
  uint32_t offset=0x80000+ce*0x2000+kind*0x1000+offsets[i];
  if(!op13_ce_bar_allowed(offset,0)||io->read(io->cookie,offset,&value)||value==~0u)rc=-4;
  else{s->values[ce][kind][i]=value;s->count++;}
 }
 if(io->write(io->cookie,OP13_CE_WINDOW,s->saved_window)||
    io->read(io->cookie,OP13_CE_WINDOW,&value)||value!=s->saved_window)return -5;
 s->restored=1;return rc;
}
#endif
