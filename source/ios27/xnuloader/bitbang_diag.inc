/* Loader S3910 GPIO-SPI integration. Parent-authored transport adapted from
 * pinned Ace5/HackiOS references; keep panel/reset ownership here. */
#include "op13_bitbang.h"
#include "startup_packet.h"
static struct op13_bitbang bb_state;

static int bb_allowed(uint32_t off)
{
 unsigned pin=off/0x1000u, reg=off%0x1000u;
 return pin>=48&&pin<=51&&(reg==0||reg==4);
}
static int bb_mmio_read(void *cookie,uint32_t off,uint32_t *value)
{
 (void)cookie;
 if(!value||!bb_allowed(off))return -5;
 __asm__ volatile("dsb sy" ::: "memory");
 *value=*(volatile uint32_t *)(uintptr_t)(0xf100000ULL+off);
 __asm__ volatile("dsb sy" ::: "memory");
 return 0;
}
static int bb_mmio_write(void *cookie,uint32_t off,uint32_t value)
{
 (void)cookie;
 if(!bb_allowed(off)||off==48*0x1000u+4)return -5;
 __asm__ volatile("dsb sy" ::: "memory");
 *(volatile uint32_t *)(uintptr_t)(0xf100000ULL+off)=value;
 __asm__ volatile("dsb sy" ::: "memory");
 return 0;
}
static int bb_delay(void *cookie,unsigned us)
{ (void)cookie;return us>1000000||EFI_ERROR(BS->stall(us))?-1:0; }
static int bb_mapping_check(void)
{
 for(unsigned pin=48;pin<=51;pin++) {
  uint64_t page=0xf100000ULL+pin*0x1000ULL,par=uefi_page_translation(page),wpar;
  __asm__ volatile("at s1e1w, %1\nisb\nmrs %0, par_el1":"=r"(wpar):"r"(page):"memory");
  print("bitbang map pin %d page %x read-PAR %x write-PAR %x\n",(uint64_t)pin,page,par,wpar);
  if((par&1)||(wpar&1)||(par&0x0000fffffffff000ULL)!=page||
     (wpar&0x0000fffffffff000ULL)!=page||((par>>56)&0xf0))return -3;
 }
 return 0;
}
#include "bitbang_cleanup.inc"
static void bitbang_diag(void)
{
 uint8_t header[4]={0},body[31]={0};
 unsigned frame=0,valid=0;
 int rc=-1,cleanup=0;
 if(identify_session.blocked||identify_session.panel_owned||bb_state.owned){
  print("bitbang refused retained resources\n");return;
 }
 if(bb_mapping_check()){print("bitbang refused mapping\n");return;}
 geni_preflight_diag();
 if(geni_state.result||geni_state.cleanup_result||geni_state.votes||
    !geni_state.snapshot.stable||!geni_state.snapshot.local_idle||
    !geni_state.snapshot.protocol_spi){
  print("bitbang refused controller state\n");return;
 }
 op13_trace(64,0);
 identify_session.panel_owned=1;
 rc=identify_prepare(&identify_panel);
 if(rc)goto done;
 const struct op13_bitbang_io io={0,bb_mmio_read,bb_mmio_write,bb_delay};
 rc=op13_bitbang_open(&bb_state,&io,1);
 if(rc)goto done;
 op13_trace(64,1);
 if(identify_session.limit==2){rc=0;goto done;}
 rc=op13_bitbang_transfer(&bb_state,0,header,sizeof(header));
 op13_trace(64,2);
 if(rc)goto done;
 valid=header[0]==0xa5&&header[1]==0x10&&header[2]==24&&header[3]==0;
 if(!valid){rc=-2;goto done;}
 if(identify_session.limit==3){rc=0;goto done;}
 rc=op13_bitbang_transfer(&bb_state,0,body,sizeof(body));
 op13_trace(64,3);
 if(rc)goto done;
 rc=startup_packet_validate(header,sizeof(header),body,sizeof(body),&frame);
done:
 cleanup=bb_cleanup_all();
 if(cleanup){identify_session.blocked=1;rc=-1;}
 op13_trace(64,4);
 print("touch-identify rc %x image-status 0 bitbang 1\n",(uint64_t)(int64_t)rc);
 print("bitbang result %x cleanup %x owned %d panel-owned %d blocked %d header-valid %d frame-valid %d\n",
  (uint64_t)(int64_t)rc,(uint64_t)(int64_t)cleanup,(uint64_t)bb_state.owned,
  (uint64_t)identify_session.panel_owned,(uint64_t)identify_session.blocked,(uint64_t)valid,(uint64_t)frame);
 print("bitbang header %x %x %x %x\n",(uint64_t)header[0],(uint64_t)header[1],(uint64_t)header[2],(uint64_t)header[3]);
 for(unsigned i=0;i<sizeof(body);i++)print("bitbang body %d %x\n",(uint64_t)i,(uint64_t)body[i]);
}
