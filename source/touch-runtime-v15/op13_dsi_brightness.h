/* SPDX-License-Identifier: BSD-2-Clause
 * Independent AA569 DCS51 sender. Wire format and FIFO sequence checked
 * against native dsi_ctrl.c and dsi_ctrl_hw_cmn.c. No memory DMA.
 */
#ifndef OP13_DSI_BRIGHTNESS_H
#define OP13_DSI_BRIGHTNESS_H
#include <stdint.h>
struct op13_dsi_io {
 void *context;
 uint32_t (*read)(void *,unsigned);
 void (*write)(void *,unsigned,uint32_t);
 void (*delay)(void *,unsigned);
};
#define OP13_DSI_STATUS_BITS ((1u<<0)|(1u<<8)|(1u<<10)|(1u<<12)|(1u<<14)|(1u<<16)|(1u<<20)|(1u<<24)|(1u<<28)|(1u<<30))
static uint32_t op13_dsi_payload(unsigned dbv)
{return 0xff000051u|((dbv>>8)&255u)<<8|(dbv&255u)<<16;}
static int op13_dsi_set(const struct op13_dsi_io *io,unsigned dbv)
{
 if(!dbv||dbv>3515)return -2; /* DT normal range; never enter HBM. */
 uint32_t ctrl=io->read(io->context,4),tpg=io->read(io->context,0x15c);
 if(ctrl==~0u||!(ctrl&1)||!(ctrl&6)||tpg)return -3;
 unsigned i;
 for(i=0;i<200;i++){if(!(io->read(io->context,8)&7))break;io->delay(io->context,50);}
 if(i==200)return -1;
 uint32_t intr=io->read(io->context,0x110);
 if(intr&(1u<<24))return -4;
 uint32_t dma=io->read(io->context,0x3c),len=io->read(io->context,0x4c),trig=io->read(io->context,0x84);
#define OP13_DSI_W(off,v) io->write(io->context,off,v)
 /* Preserve enable bits and other pending interrupts; ACK only DMA done. */
 OP13_DSI_W(0x110,(intr&~OP13_DSI_STATUS_BITS)|1);
 OP13_DSI_W(4,ctrl|4);
 /* Native SW DMA trigger: preserve the independent MDP trigger and TE. */
 OP13_DSI_W(0x84,(trig&~((7u<<17)|(1u<<16)|15u))|4);
 OP13_DSI_W(0x15c,0x30006);
 OP13_DSI_W(0x17c,0xc0390003); /* last, long DCS write, three payload bytes */
 OP13_DSI_W(0x17c,op13_dsi_payload(dbv));
 OP13_DSI_W(0x3c,(dma&~(3u<<30))|(1u<<28));
 OP13_DSI_W(0x4c,8);
 OP13_DSI_W(0x90,1);
 int done=0;
 for(i=0;i<1000;i++){
  intr=io->read(io->context,0x110);
  if(intr&(1u<<24))break;
  if(intr&1){done=1;break;}
  io->delay(io->context,10);
 }
 /* A stalled engine is retained for diagnosis and requires cold reset. */
 if(!done)return -5;
 OP13_DSI_W(0x110,(intr&~OP13_DSI_STATUS_BITS)|1);
 OP13_DSI_W(0x15c,0);
 OP13_DSI_W(0x1ec,1);io->delay(io->context,1);OP13_DSI_W(0x1ec,0);
 OP13_DSI_W(0x3c,dma);OP13_DSI_W(0x4c,len);OP13_DSI_W(0x84,trig);OP13_DSI_W(4,ctrl);
#undef OP13_DSI_W
 if(io->read(io->context,4)!=ctrl||io->read(io->context,0x3c)!=dma||io->read(io->context,0x84)!=trig)return -6;
 return 0;
}
#endif
