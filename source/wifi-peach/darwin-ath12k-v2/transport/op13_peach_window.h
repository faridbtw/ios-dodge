/* SPDX-License-Identifier: BSD-2-Clause
 * Peach dynamic PCIe window; only audited UMAC, CE and CMEM regions.
 * Callers must serialize selection, access, and restoration together. */
#ifndef OP13_PEACH_WINDOW_H
#define OP13_PEACH_WINDOW_H
#include <stdint.h>
struct op13_peach_window {uint32_t address,selected;unsigned remap;};
static int op13_peach_window_plan(uint32_t offset,uint32_t saved,struct op13_peach_window *out)
{
 if(!out||(offset&3))return -1;
 struct op13_peach_window p={0};
 if(offset<0x80000){
  /* MHI is owned by its transport, never by the HAL bus callback. */
  if(offset<0x1000||offset==0x3278)return -1;
  p.address=offset;
 }else{
  uint32_t win=offset>>19;
  if(win!=2&&win!=0x14&&win!=0x37)return -1;
  if(saved==~0u)return -1;
  p.address=0x80000+(offset&0x7ffff);p.selected=(saved&~0x3fu)|0x40000000u|win;p.remap=1;
 }
 *out=p;return 0;
}
#endif
