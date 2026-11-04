/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded two-pass discovery of vendor MHI misc capabilities. Definitions are
 * from the pinned OnePlus MHI misc.h: BW scale3/ER2/DB126, host notify6/DB124.
 * Never interpret BHI serial/key registers or write unknown capability IDs.
 */
#ifndef OP13_WIFI_MHI_CAPS_H
#define OP13_WIFI_MHI_CAPS_H
#include <stdint.h>
struct op13_wifi_mhi_caps_io {
 void *context;
 uint32_t (*read)(void *,uint32_t);
 void (*write)(void *,uint32_t,uint32_t);
};
static int op13_wifi_mhi_cap_offset(uint32_t o)
{return !(o&3)&&((o>=0x1c8&&o<=0x2f8)||(o>=0x720&&o<=0xff8));}
static int op13_wifi_mhi_caps_setup(const struct op13_wifi_mhi_caps_io *io,uint32_t *bw,uint32_t *notify)
{
 uint32_t at=io->read(io->context,0x24),seen[32]={0};unsigned n=0;
 *bw=*notify=0;
 while(at){
  if(!op13_wifi_mhi_cap_offset(at)||n==32)return -1;
  for(unsigned i=0;i<n;i++)if(seen[i]==at)return -2;
  seen[n++]=at;
  uint32_t header=io->read(io->context,at),id=header>>24;
  if(header==~0u)return -3;
  if(id==3){if(*bw)return -4;*bw=at+4;}
  if(id==6){if(*notify)return -4;*notify=at+4;}
  at=(header>>12)&0xfff;
 }
 /* Only mutate after the entire chain has been validated. */
 /* Native mhi_init_bw_scale returns ENODEV without a host scaling callback.
  * This host cannot retrain PCIe: do not advertise dynamic BW support.
  * Fresh reset must leave this capability disabled. */
 if(*bw&&io->read(io->context,*bw))return -5;
 if(*notify)io->write(io->context,*notify,0x8000007c);
 return !!*bw+!!*notify;
}
#endif
