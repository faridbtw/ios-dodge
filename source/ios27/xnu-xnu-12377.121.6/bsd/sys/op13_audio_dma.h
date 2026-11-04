/* SPDX-License-Identifier: BSD-2-Clause
 * Cold-test-only Apps SMMUv500 audio context. Retain all resources to reset.
 * Register ordering/layout matches vendor arm-smmu, verified on this phone's
 * private PCIe context. Never change SCR0, bypass, or an occupied context.
 */
#ifndef OP13_AUDIO_DMA_H
#define OP13_AUDIO_DMA_H
#include "op13_audio_smmu.h"
struct op13_audio_dma_io {
 void *context;
 uint32_t (*read)(void *,uint32_t);
 void (*write)(void *,uint32_t,uint32_t);
 void (*delay_us)(void *,unsigned);
};
static int op13_audio_dma_program(const struct op13_audio_dma_io *io,
 const struct op13_audio_smmu_plan *p,uint64_t table)
{
 if(!io||!io->read||!io->write||!io->delay_us||!p||!table||
    (table&4095)||(table>>36)||p->page!=4096||p->pages!=128||
    p->context<p->first_context||p->context>=p->contexts||
    p->slots[0]>=p->matches||p->slots[1]>=p->matches||p->slots[0]==p->slots[1])return -1;
 void *c=io->context;struct op13_audio_smmu_plan measured={0};
 uint32_t scr=io->read(c,0),idr0=io->read(c,0x20),idr1=io->read(c,0x24);
 if(op13_audio_smmu_geometry(scr,idr0,idr1,&measured)||
    measured.page!=p->page||measured.pages!=p->pages||
    measured.matches!=p->matches||measured.contexts!=p->contexts||
    measured.first_context!=p->first_context)return -2;
 for(unsigned i=0;i<p->matches;i++){
  uint32_t smr=io->read(c,0x800+4*i),s2cr=io->read(c,0xc00+4*i);
  if(!(smr>>31))continue;
  if(i==p->slots[0]||i==p->slots[1]||(s2cr&255)==p->context)return -3;
  uint32_t sid=smr&0x7fff,mask=(smr>>16)&0x7fff;
  for(unsigned j=0;j<2;j++)
   if(!((sid^op13_audio_sids[j])&~(mask|op13_audio_masks[j])&0x7fff))return -3;
 }
 uint32_t cb=(p->pages+p->context)*p->page;
 if(io->read(c,cb)&1)return -3;
 io->write(c,cb,0);
 io->write(c,0x1800+4*p->context,1);
 io->write(c,0x1000+4*p->context,(1u<<16)|(3u<<8)|(15u<<12));
 io->write(c,cb+0x10,1u|(1u<<4)|(7u<<15));
 io->write(c,cb+0x30,25u|(1u<<8)|(1u<<10)|(3u<<12)|(1u<<23));
 io->write(c,cb+0x20,(uint32_t)table);
 io->write(c,cb+0x24,(uint32_t)(table>>32)|(p->context<<16));
 io->write(c,cb+0x28,0);io->write(c,cb+0x2c,p->context<<16);
 io->write(c,cb+0x38,0xf404ff44);io->write(c,cb+0x3c,0);
 io->write(c,cb+0x58,0xc00001fe);
 io->write(c,cb+0x610,p->context);io->write(c,cb+0x7f0,1);
 unsigned n;for(n=0;n<1000;n++){
  if(!(io->read(c,cb+0x7f4)&1))break;
  io->delay_us(c,100);
 }
 if(n==1000)return -4;
 io->write(c,cb,0x1127);
 for(unsigned j=0;j<2;j++){
  io->write(c,0xc00+4*p->slots[j],p->context);
  io->write(c,0x800+4*p->slots[j],0x80000000u|
   (op13_audio_masks[j]<<16)|op13_audio_sids[j]);
 }
 if((io->read(c,cb)&0x127)!=0x127||io->read(c,cb+0x20)!=(uint32_t)table||
    io->read(c,cb+0x24)!=((uint32_t)(table>>32)|(p->context<<16)))return -5;
 for(unsigned j=0;j<2;j++){
  if(io->read(c,0x800+4*p->slots[j])!=(0x80000000u|
     (op13_audio_masks[j]<<16)|op13_audio_sids[j])||
     (io->read(c,0xc00+4*p->slots[j])&0x300ff)!=p->context)return -5;
 }
 return 0;
}
#endif
