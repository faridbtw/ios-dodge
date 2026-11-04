#include "op13_wifi_ce.h"
static int op13_wifi_ce_fault;
static int op13_wifi_ce_read(void *cookie,uint32_t offset,uint32_t *value)
{
 (void)cookie;
 uint64_t address=0x40400000ULL+offset,par=uefi_page_translation(address);
 if(!value||op13_wifi_ce_fault||!op13_ce_bar_allowed(offset,0)||
    (par&1)||(par&0x000ffffffffff000ULL)!=(address&~0xfffULL))return -1;
 __asm__ volatile("dsb sy":::"memory");
 *value=*(volatile uint32_t *)(uintptr_t)address;
 __asm__ volatile("dsb sy":::"memory");return 0;
}
static int op13_wifi_ce_write(void *cookie,uint32_t offset,uint32_t value)
{
 (void)cookie;
 uint64_t address=0x40400000ULL+offset,par;
 if(!op13_ce_bar_allowed(offset,1))return -1;
 __asm__ volatile("at s1e1w, %1\nisb\nmrs %0, par_el1":"=r"(par):"r"(address):"memory");
 if((par&1)||(par&0x000ffffffffff000ULL)!=(address&~0xfffULL))return -1;
 __asm__ volatile("dsb sy":::"memory");
 *(volatile uint32_t *)(uintptr_t)address=value;
 __asm__ volatile("dsb sy":::"memory");return 0;
}
static int op13_wifi_ce_survey(void)
{
 struct mhi_host *h=&op13_wifi_mhi_host;
 if(!op13_wifi_fw_init_done||op13_wifi_mhi_stopped||!h->powered||h->failed||
    h->ee!=MHI_HOST_EE_AMSS||h->state!=MHI_HOST_STATE_M0||op13_wifi_mhi_adapter_fault)return -1;
 struct op13_ce_snapshot s;struct op13_ce_io io={0,op13_wifi_ce_read,op13_wifi_ce_write};
 efi_status status=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(status))return -2;
 int rc=op13_ce_snapshot(&io,1,&s);
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-ce: native v2 base=%x selector=%x saved=%x selected=%x reads=%d restored=%d rc=%d\n",
  (uint64_t)OP13_CE_BASE,(uint64_t)OP13_CE_WINDOW,(uint64_t)s.saved_window,(uint64_t)s.selected_window,
  (uint64_t)s.count,(uint64_t)s.restored,(uint64_t)(int64_t)rc);
 if(rc){op13_wifi_ce_fault=1;return rc;}
 for(unsigned ce=0;ce<4;ce++)for(unsigned kind=0;kind<2;kind++){
  const uint32_t *v=s.values[ce][kind];
  print("op13-wifi-ce: CE%d %s base=%x:%x id=%x status=%x misc=%x hp=%x tp=%x\n",
   (uint64_t)ce,kind?"dst":"src",(uint64_t)v[1],(uint64_t)v[0],(uint64_t)v[2],
   (uint64_t)v[3],(uint64_t)v[4],(uint64_t)v[5],(uint64_t)v[6]);
 }
 return 0;
}
