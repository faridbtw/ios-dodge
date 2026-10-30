/* SPDX-License-Identifier: BSD-2-Clause
 * Dodge PCIe SID1401 only. Pure ownership/table planning, no MMIO or bypass.
 */
#ifndef OP13_WIFI_SMMU_PLAN_H
#define OP13_WIFI_SMMU_PLAN_H
#include <stdint.h>
/* Captured cnss_audio_iommu_group0 geometry starts at 0x18000000. */
#define OP13_WIFI_IOVA 0x18000000ULL
#define OP13_WIFI_DMA_BYTES 0x8000000ULL
#define OP13_WIFI_TABLES 66u
struct op13_wifi_smmu_plan {uint32_t page,pages,contexts,matches,slot,context;};
static int op13_wifi_smmu_choose(uint32_t scr,uint32_t id0,uint32_t id1,
 const uint32_t *smr,const uint32_t *s2cr,struct op13_wifi_smmu_plan *p)
{
 if(scr==~0u||id0==~0u||id1==~0u||(scr&9)||!(id0&(1u<<30))||!(id0&(1u<<27)))return -1;
 p->page=(id1>>31)?0x10000:0x1000;p->pages=2u<<((id1>>28)&7);
 p->contexts=id1&255;p->matches=id0&255;p->slot=~0u;p->context=~0u;
 if(2u*p->page*p->pages!=0x100000||!p->contexts||p->contexts>p->pages||!p->matches||p->matches>256)return -1;
 uint8_t used[256]={0};
 for(unsigned i=0;i<p->matches;i++){
  if(smr[i]>>31){
   used[s2cr[i]&255]=1;
   uint32_t sid=smr[i]&0x7fff,mask=(smr[i]>>16)&0x7fff;
   if(!((sid^0x1401)&~mask&0x7fff))return -2;
  }else if(p->slot==~0u)p->slot=i;
 }
 for(unsigned i=(id1>>16)&255;i<p->contexts;i++)if(!used[i]){p->context=i;break;}
 return p->slot==~0u||p->context==~0u?-3:0;
}
static int op13_wifi_smmu_tables(uint64_t tables[OP13_WIFI_TABLES][512],uint64_t table_pa,uint64_t pool_pa)
{
 if(!table_pa||!pool_pa||(table_pa>>36)||(pool_pa>>36)||(table_pa&4095)||(pool_pa&(OP13_WIFI_DMA_BYTES-1))||
  table_pa+OP13_WIFI_TABLES*4096ULL>(1ULL<<36)||pool_pa+OP13_WIFI_DMA_BYTES>(1ULL<<36)||
  (table_pa<pool_pa+OP13_WIFI_DMA_BYTES&&pool_pa<table_pa+OP13_WIFI_TABLES*4096ULL))return -1;
 for(unsigned i=0;i<OP13_WIFI_TABLES;i++)for(unsigned j=0;j<512;j++)tables[i][j]=0;
 tables[0][(OP13_WIFI_IOVA>>30)&511]=(table_pa+4096)|3;
 for(unsigned block=0;block<OP13_WIFI_DMA_BYTES/0x200000ULL;block++){
  tables[1][((OP13_WIFI_IOVA>>21)&511)+block]=(table_pa+(2+block)*4096)|3;
  for(unsigned i=0;i<512;i++){
   /* Normal NC (MAIR index0), unprivileged RW, outer shareable, AF/nG/XN. */
   tables[2+block][i]=(pool_pa+(block*512ULL+i)*4096)|0xe43ULL|(3ULL<<53);
  }
 }
 return 0;
}
#endif
