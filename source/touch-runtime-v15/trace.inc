#include "trace.h"
#include "trace_info.h"
struct trace_entry {uint64_t tick;uint32_t stage,detail;};
static volatile struct {uint64_t magic;uint32_t count,last;struct trace_entry entries[256];} touch_trace;
void op13_trace(uint32_t stage,uint32_t detail)
{
 touch_trace.magic=0x4f50313354524332ULL;
 unsigned n=touch_trace.count;
 if(n<256){touch_trace.entries[n].tick=READ_SYSREG(CNTVCT_EL0);touch_trace.entries[n].stage=stage;touch_trace.entries[n].detail=detail;touch_trace.count=n+1;}
 touch_trace.last=stage;
 clean_dcache((uintptr_t)&touch_trace,(uintptr_t)&touch_trace+sizeof(touch_trace));
 /* Stream only while a host is waiting for this OEM command's status.
  * INFO is standard fastboot, not an unsolicited USB packet. Fixed hex fields
  * avoid the log/print buffers and remain available after a device reset.
  * usb_tx uses the existing watchdog-fed controller event loop; this may
  * perturb timing, so the stream is observation, not proof of original timing. */
 if(touch_trace_stream && USB && usb_tx_buf) {
  char msg[48];unsigned len=op13_trace_info(msg,stage,detail);
  usb_tx((const uint8_t *)msg,len);
 }
 if(!GOP||!GOP->mode||!GOP->mode->info||stage>31)return;
 efi_gop_mode *m=GOP->mode;uint64_t stride=(uint64_t)m->info->pixels_per_scanline*4;
 uint64_t x=8+(stage%8)*24,y=8+(stage/8)*24,end=(y+15)*stride+(x+16)*4;
 if(m->info->pixel_format>1||x+16>m->info->width||y+16>m->info->height||
 (x+16)*4>stride||end>m->fb_size||m->fb_base>UINT64_MAX-end)return;
 for(unsigned row=0;row<16;row++){
 volatile uint32_t *v=(void *)(uintptr_t)(m->fb_base+(y+row)*stride+x*4);
 for(unsigned col=0;col<16;col++)v[col]=detail==1?0x00ffffff:0x00808080;
 clean_dcache((uintptr_t)v,(uintptr_t)v+64);
 }
 __asm__ volatile("dsb sy" ::: "memory");
}
