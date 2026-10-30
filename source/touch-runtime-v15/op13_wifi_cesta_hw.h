/* SPDX-License-Identifier: BSD-2-Clause
 * Cold-only PCIe0 HW-DRV0 Gen1 power map, transcribed from pinned crm-v2.c.
 * Refuse previously owned channels/tables. Leave other resources and all BW
 * passthrough votes unchanged. Once committed, release only by cold reset.
 */
#ifndef OP13_WIFI_CESTA_HW_H
#define OP13_WIFI_CESTA_HW_H
#include "op13_wifi_cesta.h"
static inline int op13_cesta_hw_gen1(const struct op13_cesta_io *io)
{
 if(!io||!io->read||!io->write||!io->delay_us)return -1;
 void *c=io->context;
 if(io->read(c,OP13_CRM_COMMON_VERSION)!=0x20000||
    io->read(c,OP13_CRM_COMMON_CONFIG)!=0x80228225||
    !(io->read(c,OP13_CRM_COMMON_ENABLE)&1))return -1;
 if(io->read(c,0x370)||io->read(c,0x374))return -2;
 /* Both channels must be completely unclaimed. Check independent resource1
  * and all16 passthrough nodes, not only the resource we are about to claim. */
 for(unsigned resource=0;resource<2;resource++)
  for(unsigned ch=0;ch<2;ch++)for(unsigned st=0;st<5;st++)
   if(io->read(c,resource*0x2c+ch*0x14+st*4))return -2;
 for(unsigned resource=0;resource<16;resource++)
  for(unsigned ch=0;ch<2;ch++)for(unsigned st=0;st<5;st++)
   if(io->read(c,0xb0+resource*0x2c+ch*0x14+st*4))return -2;
 /* D0 map_save(Gen1): power states0/1 -> performance2, other states0.
  * HW-DRV0/resource0/channel0 only. No bandwidth register writes. */
 const uint32_t values[5]={2,2,0,0,0};
 for(unsigned st=0;st<5;st++)io->write(c,st*4,values[st]);
 io->write(c,0x374,1);
 for(unsigned i=0;i<100;i++){
  if((io->read(c,0x370)&3)==1)return 0;
  io->delay_us(c,100);
 }
 return -3;
}
#endif
