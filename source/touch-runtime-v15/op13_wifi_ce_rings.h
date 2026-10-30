/* SPDX-License-Identifier: BSD-2-Clause
 * Seven native Peach/Kiwi host rings. CE9..11 are reserved for MHI.
 * Fixed descriptors/pointers live in the unused owned DMA interval24..32MiB,
 * between RDDM[16,23.5MiB) and WLAN runtime allocations[32MiB,...).
 */
#ifndef OP13_WIFI_CE_RINGS_H
#define OP13_WIFI_CE_RINGS_H
#include "op13_wifi_ce.h"
#define OP13_CE_MEM_FIRST 0x1800000u
#define OP13_CE_MEM_END   0x1900000u
struct op13_ce_ring {unsigned ce,kind,entries,words;uint32_t ring_offset,pointer_offset,buffer_offset,stride,max_bytes;};
static const struct op13_ce_ring op13_ce_rings[7]={
 {0,0,16,4,0x1800000,0x1801000,0x1870000,2048,2048},
 {1,1,64,2,0x1802000,0x1801004,0x1810000,2048,2048},
 {1,2,64,4,0x1803000,0x1801008,0,0,0},
 {2,1,64,2,0x1804000,0x180100c,0x1830000,4096,3520},
 {2,2,64,4,0x1805000,0x1801010,0,0,0},
 {3,0,32,4,0x1806000,0x1801014,0x1880000,4096,3520},
 {4,0,256,4,0x1807000,0x1801018,0x18a0000,256,256},
};
static uint32_t op13_ce_r0(const struct op13_ce_ring *r)
{return 0x80000+r->ce*0x2000+(r->kind?0x1000:0)+(r->kind==2?0x58:0);}
static uint32_t op13_ce_r2(const struct op13_ce_ring *r)
{return 0x80000+r->ce*0x2000+(r->kind?0x1000:0)+0x400+(r->kind==2?8:0);}
static int op13_ce_init_allowed(uint32_t off,int write)
{
 if(off==OP13_CE_WINDOW)return 1;
 if(off&3)return 0;
 for(unsigned i=0;i<7;i++){
  const struct op13_ce_ring *r=&op13_ce_rings[i];uint32_t r0=op13_ce_r0(r),r2=op13_ce_r2(r);
  if(off==r2||off==r2+4)return 1;
  if(off==r0||off==r0+4||off==r0+8||off==r0+16||off==r0+(r->kind==2?0x14:0x1c)||off==r0+(r->kind==2?0x18:0x20)||
     off==r0+(r->kind==2?0x24:0x30)||off==r0+(r->kind==2?0x38:0x4c))return 1;
  if(r->kind!=2&&off==r0+0x34)return 1;
  if(r->kind==1&&off==0x80000+r->ce*0x2000+0x1000+0xb0)return 1;
  if(!write&&off==r0+12)return 1;
 }
 return 0;
}
static int op13_ce_owned(uint32_t offset,uint32_t size,uint64_t pool_bytes)
{return offset>=OP13_CE_MEM_FIRST&&offset<OP13_CE_MEM_END&&size<=OP13_CE_MEM_END-offset&&
        offset<=pool_bytes&&size<=pool_bytes-offset;}
static int op13_ce_plan_valid(uint64_t iova,uint64_t pool_bytes)
{
 if(iova!=0x18000000ULL||pool_bytes!=0x4000000ULL)return 0;
 for(unsigned i=0;i<7;i++){
  const struct op13_ce_ring *r=&op13_ce_rings[i];
  if(!r->entries||(r->entries&(r->entries-1))||(r->ring_offset&0xfff)||(r->pointer_offset&3)||
     !op13_ce_owned(r->ring_offset,r->entries*r->words*4,pool_bytes)||!op13_ce_owned(r->pointer_offset,4,pool_bytes))return 0;
  if(r->kind!=2&&(!op13_ce_owned(r->buffer_offset,r->entries*r->stride,pool_bytes)||r->max_bytes>r->stride))return 0;
 }
 struct{uint32_t offset,size;} areas[21];unsigned count=0;
 for(unsigned i=0;i<7;i++){
  const struct op13_ce_ring *r=&op13_ce_rings[i];
  areas[count++]=(typeof(*areas)){r->ring_offset,r->entries*r->words*4};
  areas[count++]=(typeof(*areas)){r->pointer_offset,4};
  if(r->kind!=2)areas[count++]=(typeof(*areas)){r->buffer_offset,r->entries*r->stride};
 }
 for(unsigned i=0;i<count;i++)for(unsigned j=0;j<i;j++)
  if(areas[i].offset<areas[j].offset+areas[j].size&&areas[j].offset<areas[i].offset+areas[i].size)return 0;
 return 1;
}
/* Runs only after owned descriptor/pointer RAM is initialized, and all ring
 * registers have been preflighted as disabled/unconfigured in one pass. */
static int op13_ce_program(const struct op13_ce_io *io,uint64_t iova,uint64_t pool_bytes)
{
 if(!io||!io->read||!io->write||!op13_ce_plan_valid(iova,pool_bytes))return -1;
 for(unsigned i=0;i<7;i++){
  const struct op13_ce_ring *r=&op13_ce_rings[i];uint32_t b=op13_ce_r0(r),p=op13_ce_r2(r);
  uint64_t address=iova+r->ring_offset,pointer=iova+r->pointer_offset;
  struct{uint32_t off,value;} writes[]={
   {b+16,0}, /* Disable while programming. */
   {b,(uint32_t)address},{b+4,(uint32_t)(address>>32)|(r->entries*r->words<<8)},
   {b+8,r->words|(r->kind==2?(100+r->ce)<<8:0)},
   {b+(r->kind==2?0x14:0x1c),(uint32_t)pointer},
   {b+(r->kind==2?0x18:0x20),(uint32_t)(pointer>>32)},
   {b+(r->kind==2?0x24:0x30),0}, /* Polling: IRQ thresholds off. */
   {b+(r->kind==2?0x38:0x4c),0}, /* MSI enable off. */
   {p,0},{p+4,0},
  };
  for(unsigned j=0;j<sizeof writes/sizeof *writes;j++){
   if(!op13_ce_init_allowed(writes[j].off,1)||io->write(io->cookie,writes[j].off,writes[j].value))return -2;
  }
  if(r->kind!=2&&io->write(io->cookie,b+0x34,0))return -2;
  if(r->kind==1){
   uint32_t ctrl=0x80000+r->ce*0x2000+0x1000+0xb0,value;
   if(io->read(io->cookie,ctrl,&value)||value==~0u||io->write(io->cookie,ctrl,(value&~0xffffu)|r->max_bytes))return -2;
  }
  uint32_t misc=r->kind==2?0x40:0x42;
  const struct{uint32_t off,mask,value;} verify[]={
   {b,~0u,(uint32_t)address},{b+4,0xffffffu,(uint32_t)(address>>32)|(r->entries*r->words<<8)},
   {b+8,0xffffu,r->words|(r->kind==2?(100+r->ce)<<8:0)},
   {b+(r->kind==2?0x14:0x1c),~0u,(uint32_t)pointer},
   {b+(r->kind==2?0x18:0x20),0xffu,(uint32_t)(pointer>>32)},
   {b+(r->kind==2?0x38:0x4c),0x100u,0},
  };
  for(unsigned j=0;j<sizeof verify/sizeof *verify;j++){
   uint32_t value;if(io->read(io->cookie,verify[j].off,&value)||(value&verify[j].mask)!=verify[j].value)return -3;
  }
  uint32_t value;
  if(io->write(io->cookie,b+16,misc)||io->read(io->cookie,b+16,&value)||(value&0x7f)!=misc)return -3;
 }
 return 0;
}
#endif
