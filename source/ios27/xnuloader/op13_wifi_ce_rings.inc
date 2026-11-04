#include "op13_wifi_ce_rings.h"
_Static_assert(OP13_CE_MEM_FIRST>=OP13_WIFI_RDDM_OFFSET+OP13_WIFI_RDDM_BYTES,"CE overlaps RDDM");
_Static_assert(OP13_CE_MEM_END<=0x2000000,"CE overlaps WLAN requested memory");
static int op13_wifi_ce_rings_ready,op13_wifi_ce_init_fault;
static int op13_wifi_ce_init_read(void *cookie,uint32_t offset,uint32_t *value)
{
 (void)cookie;uint64_t address=0x40400000ULL+offset,par=uefi_page_translation(address);
 if(!value||!op13_ce_init_allowed(offset,0)||(par&1)||
    (par&0x000ffffffffff000ULL)!=(address&~0xfffULL))return -1;
 __asm__ volatile("dsb sy":::"memory");*value=*(volatile uint32_t *)(uintptr_t)address;
 __asm__ volatile("dsb sy":::"memory");return 0;
}
static int op13_wifi_ce_init_write(void *cookie,uint32_t offset,uint32_t value)
{
 (void)cookie;uint64_t address=0x40400000ULL+offset,par;
 if(!op13_ce_init_allowed(offset,1))return -1;
 __asm__ volatile("at s1e1w, %1\nisb\nmrs %0, par_el1":"=r"(par):"r"(address):"memory");
 if((par&1)||(par&0x000ffffffffff000ULL)!=(address&~0xfffULL))return -1;
 __asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)(uintptr_t)address=value;
 __asm__ volatile("dsb sy":::"memory");return 0;
}
static int op13_wifi_ce_init(void)
{
 struct mhi_host *h=&op13_wifi_mhi_host;
 if(!op13_wifi_fw_init_done||op13_wifi_mhi_stopped||op13_wifi_ce_init_fault||op13_wifi_ce_rings_ready||
    !h->powered||h->failed||h->ee!=MHI_HOST_EE_AMSS||h->state!=MHI_HOST_STATE_M0||
    !op13_ce_plan_valid(OP13_WIFI_IOVA,OP13_WIFI_DMA_BYTES))return -1;
 const struct op13_ce_io io={0,op13_wifi_ce_init_read,op13_wifi_ce_init_write};
 uint32_t saved=0,selected=0,value=0;int changed=0,rc=-2;
 efi_status status=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(status))return -2;
 if(io.read(0,OP13_CE_WINDOW,&saved)||saved==~0u)goto done;
 selected=(saved&~0x3fu)|0x40000037u;changed=1;
 if(io.write(0,OP13_CE_WINDOW,selected)||io.read(0,OP13_CE_WINDOW,&value)||value!=selected)goto restore;
 /* Every ring must be unconfigured BEFORE any ring or DMA-memory changes. */
 for(unsigned i=0;i<7;i++){
  uint32_t b=op13_ce_r0(&op13_ce_rings[i]);
  if(io.read(0,b+16,&value)||!(value&0x80)||(value&0x40))goto restore;
  for(unsigned j=0;j<3;j++)if(io.read(0,b+j*4,&value)||value)goto restore;
 }
 memset((void *)(uintptr_t)(op13_wifi_dma_pool+OP13_CE_MEM_FIRST),0,OP13_CE_MEM_END-OP13_CE_MEM_FIRST);
 for(unsigned i=0;i<7;i++){
  const struct op13_ce_ring *r=&op13_ce_rings[i];if(r->kind!=1)continue;
  uint32_t *desc=(uint32_t *)(uintptr_t)(op13_wifi_dma_pool+r->ring_offset);
  for(unsigned n=0;n<r->entries;n++){
   uint64_t address=OP13_WIFI_IOVA+r->buffer_offset+(uint64_t)n*r->stride;
   desc[n*2]=(uint32_t)address;desc[n*2+1]=(uint32_t)(address>>32);
  }
 }
 __asm__ volatile("dsb sy":::"memory");
 rc=op13_ce_program(&io,OP13_WIFI_IOVA,OP13_WIFI_DMA_BYTES);if(rc)goto restore;
 /* Post N-1 receive buffers only AFTER all seven ring addresses were verified. */
 for(unsigned i=0;i<7;i++){
  const struct op13_ce_ring *r=&op13_ce_rings[i];if(r->kind!=1)continue;
  uint32_t hp=(r->entries-1)*r->words;
  if(io.write(0,op13_ce_r2(r),hp)||io.read(0,op13_ce_r2(r),&value)||value!=hp){rc=-4;goto restore;}
 }
 op13_wifi_ce_rings_ready=1;
restore:
 if(changed&&(io.write(0,OP13_CE_WINDOW,saved)||io.read(0,OP13_CE_WINDOW,&value)||value!=saved))rc=-5;
done:
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-ce: seven owned rings configured=%d rc=%d memory-offset=%x..%x window-restored=%d; no WLAN mode sent\n",
  (uint64_t)op13_wifi_ce_rings_ready,(uint64_t)(int64_t)rc,(uint64_t)OP13_CE_MEM_FIRST,
  (uint64_t)OP13_CE_MEM_END,(uint64_t)(changed&&value==saved));
 if(rc){op13_wifi_ce_init_fault=1;op13_wifi_ce_rings_ready=0;op13_wifi_mhi_stop_failed();}
 return rc;
}
