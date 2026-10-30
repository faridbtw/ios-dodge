#include <assert.h>
#include <stdio.h>
#include "op13_peach_window.h"
int main(void)
{
 struct op13_peach_window p;
 assert(!op13_peach_window_plan(0x1b80010,0x80400112,&p));
 assert(p.remap&&p.address==0x80010&&p.selected==0xc0400137);
 assert(!op13_peach_window_plan(0xa00000,0,&p)&&p.selected==0x40000014);
 assert(!op13_peach_window_plan(0x100100,0,&p)&&p.selected==0x40000002);
 assert(!op13_peach_window_plan(0x2000,0,&p)&&!p.remap&&p.address==0x2000);
 assert(op13_peach_window_plan(0x38,0,&p)&&op13_peach_window_plan(0x3278,0,&p));
 for(uint32_t x=0;x<0x2000000;x+=4){
  int rc=op13_peach_window_plan(x,0,&p);unsigned w=x>>19;
  assert(!rc==(x<0x80000?(x>=0x1000&&x!=0x3278):(w==2||w==0x14||w==0x37)));
  assert(op13_peach_window_plan(x+1,0,&p));
 }
 puts("Peach window: all32MiB addresses, audited regions, selector bits, MHI exclusion and alignment passed");
}
