/* SPDX-License-Identifier: BSD-2-Clause
 * Pure validation/planning for Dodge's Apps SMMUv500 audio streams.
 * No register access, bypass mode, or allocation. Leaves occupied slots alone.
 */
#ifndef OP13_AUDIO_SMMU_H
#define OP13_AUDIO_SMMU_H
#include <stdint.h>
#define OP13_AUDIO_IOVA 0x10000000ULL
#define OP13_AUDIO_DMA_PAGES 8u
#define OP13_AUDIO_DMA_BYTES (OP13_AUDIO_DMA_PAGES*4096u)
struct op13_audio_smmu_plan {
 uint32_t page,pages,contexts,matches,first_context,context,slots[2];
};
static const uint32_t op13_audio_sids[2]={0x1001,0x1041};
static const uint32_t op13_audio_masks[2]={0x80,0x20};
static int op13_audio_smmu_geometry(uint32_t scr,uint32_t idr0,uint32_t idr1,struct op13_audio_smmu_plan *p)
{
 if(scr==0xffffffff||idr0==0xffffffff||idr1==0xffffffff||(scr&8))return -1;
 p->page=(idr1>>31)?0x10000u:0x1000u;p->pages=2u<<((idr1>>28)&7);
 p->contexts=idr1&255;p->matches=idr0&255;p->first_context=(idr1>>16)&255;
 if(2*p->pages*p->page!=0x100000||!p->contexts||p->contexts>p->pages||p->first_context>=p->contexts||!p->matches||p->matches>256)return -1;
 return 0;
}
static int op13_audio_smmu_choose(struct op13_audio_smmu_plan *p,const uint32_t *smr,const uint32_t *s2cr,const uint32_t *sctlr)
{
 uint8_t used[256]={0};unsigned free_count=0;
 for(unsigned i=0;i<p->matches;i++) {
  if(smr[i]>>31) {
   used[s2cr[i]&255]=1;
   uint32_t sid=smr[i]&0x7fff,mask=(smr[i]>>16)&0x7fff;
   for(unsigned j=0;j<2;j++)
    if(!((sid^op13_audio_sids[j])&~(mask|op13_audio_masks[j])&0x7fff))return -2;
  } else if(free_count<2)p->slots[free_count++]=i;
 }
 if(free_count!=2)return -3;
 for(unsigned i=p->first_context;i<p->contexts;i++)if(!used[i]&&sctlr[i]!=0xffffffff&&!(sctlr[i]&1)){p->context=i;return 0;}
 return -4;
}
static int op13_audio_smmu_tables(uint64_t tables[3][512],const uint64_t physical[3],const uint64_t pages[OP13_AUDIO_DMA_PAGES])
{
 for(unsigned i=0;i<3;i++)if(!physical[i]||(physical[i]&4095)||(physical[i]>>36))return -1;
 for(unsigned i=0;i<OP13_AUDIO_DMA_PAGES;i++)if(!pages[i]||(pages[i]&4095)||(pages[i]>>36))return -1;
 for(unsigned i=0;i<3;i++) {
  for(unsigned j=0;j<i;j++)if(physical[i]==physical[j])return -1;
  for(unsigned j=0;j<OP13_AUDIO_DMA_PAGES;j++)if(physical[i]==pages[j])return -1;
 }
 for(unsigned i=0;i<OP13_AUDIO_DMA_PAGES;i++)
  for(unsigned j=0;j<i;j++)if(pages[i]==pages[j])return -1;
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<512;j++)tables[i][j]=0;
 tables[0][(OP13_AUDIO_IOVA>>30)&511]=physical[1]|3;
 tables[1][(OP13_AUDIO_IOVA>>21)&511]=physical[2]|3;
 /* Normal noncacheable, outer-shareable, read-only, AF,nG. */
 for(unsigned i=0;i<OP13_AUDIO_DMA_PAGES;i++)tables[2][((OP13_AUDIO_IOVA>>12)&511)+i]=pages[i]|0xec3ULL;
 return 0;
}
#endif
