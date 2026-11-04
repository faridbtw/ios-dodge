/* SPDX-License-Identifier: BSD-2-Clause */
#include "fifo_snapshot.h"
static const uint32_t offsets[FIFO_SNAPSHOT_COUNT]={
 0x40,0x64,0x68,0x258,0x610,0x640,0x800,0x804,
 0xc40,0xd40,0xe18,0xe1c,0xe20,0xe40,0x614,0x644};
uint32_t fifo_snapshot_offset(unsigned i){return i<FIFO_SNAPSHOT_COUNT?offsets[i]:UINT32_MAX;}
int fifo_snapshot_read(int enabled,int(*read32)(void *,uint64_t,uint32_t *),
 void *cookie,struct fifo_snapshot *r)
{
 if(!r||!read32)return -5;
 *r=(struct fifo_snapshot){0};r->result=-1;
 if(!enabled)return r->result=-3;
 for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<FIFO_SNAPSHOT_COUNT;i++){
  uint32_t *v=pass?&r->second[i]:&r->first[i];
  if(read32(cookie,0xa90000u+offsets[i],v))return -1;
  r->completed++;
 }
 r->stable=1;
 for(unsigned i=0;i<FIFO_SNAPSHOT_COUNT;i++)if(r->first[i]!=r->second[i])r->stable=0;
 const uint32_t *s=r->second;
 r->protocol_spi=((s[2]>>8)&255u)==1;
 /* Conservative LOCAL idle only: active command, FIFO words, DMA FSM/activity,
  * or pending IRQ state prevents readiness. This is not a GPI ring lock. */
 r->local_idle=!(s[0]&0x1001u)&&!(s[6]&0x0fffffffu)&&
  !(s[7]&0x01ffffffu)&&!(s[13]&0xff3u)&&!s[4]&&!s[5]&&!s[8]&&!s[9];
 r->result=0;return 0;
}
