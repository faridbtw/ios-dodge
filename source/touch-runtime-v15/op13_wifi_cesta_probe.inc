#include "op13_wifi_cesta.h"
static uint32_t op13_wifi_crm_read(void *context,uint32_t offset)
{
 (void)context;
 return *(volatile uint32_t *)(uintptr_t)(0x01d01000u+offset);
}
static void op13_wifi_crm_write(void *context,uint32_t offset,uint32_t value)
{
 (void)context;
 if(!((offset==OP13_CRM_SW_ACTIVE&&(value==0||value==2)) ||
      (offset==OP13_CRM_SW_IRQ_CLEAR&&value==1))) {
  op13_wifi_votes_stuck=1;return;
 }
 print("op13-wifi-cesta: write address=%x value=%x\n",(uint64_t)(0x01d01000u+offset),(uint64_t)value);
 *(volatile uint32_t *)(uintptr_t)(0x01d01000u+offset)=value;
 __asm__ volatile("dsb sy":::"memory");
}
static void op13_wifi_crm_delay(void *context,unsigned us)
{
 (void)context;BS->stall(us);
}
static void op13_wifi_cesta_probe(void)
{
 if(op13_wifi_map_registers()) {print("op13-wifi-cesta: no votes: mapping incomplete\n");return;}
 struct op13_wifi_clock_protocol *p=0;uint64_t id=0;
 efi_status s=BS->locate_protocol(&op13_wifi_clock_guid,0,(void **)&p);
 if(EFI_ERROR(s)||!p||p->revision!=0x1000b||!p->get_id||!p->enable||!p->disable||!p->is_on)return;
 s=p->get_id(p,"gcc_pcie_0_mstr_axi_clk",&id);
 if(EFI_ERROR(s)||id!=0x4010029) {print("op13-wifi-cesta: no votes: unknown AXI clock ID\n");return;}
 s=p->enable(p,id);
 print("op13-wifi-cesta: AXI clock enable status=%x\n",s);
 if(EFI_ERROR(s)){op13_wifi_votes_stuck=1;return;}
 struct op13_cesta_io io={0,op13_wifi_crm_read,op13_wifi_crm_write,op13_wifi_crm_delay};
 uint32_t saved=0;
 print("op13-wifi-cesta: initial software-vote=%x irq=%x current-perf=%x\n",
  (uint64_t)op13_wifi_crm_read(0,OP13_CRM_SW_ACTIVE),
  (uint64_t)op13_wifi_crm_read(0,OP13_CRM_SW_IRQ),
  (uint64_t)op13_wifi_crm_read(0,OP13_CRM_CURRENT_PERF));
 int ret=op13_cesta_gen1_vote(&io,&saved);
 print("op13-wifi-cesta: Gen1 vote result=%x current-perf=%x\n",(uint64_t)(int64_t)ret,
  (uint64_t)op13_wifi_crm_read(0,OP13_CRM_CURRENT_PERF));
 if(ret==-3){op13_wifi_votes_stuck=1;print("op13-wifi-cesta: vote timeout: retain resources, cold reset required\n");return;}
 if(!ret) {
  uint8_t on=0;s=p->is_on(p,id,&on);
  print("op13-wifi-cesta: Gen1 AXI clock state-status=%x on=%d\n",s,(uint64_t)on);
 }
 s=p->disable(p,id);
 print("op13-wifi-cesta: own AXI clock release status=%x\n",s);
 if(EFI_ERROR(s)){op13_wifi_votes_stuck=1;return;}
 if(ret) return; /* Ownership/profile guard did not change a CRM vote. */
 ret=op13_cesta_restore(&io,saved);
 print("op13-wifi-cesta: restoration result=%x current-perf=%x irq=%x\n",(uint64_t)(int64_t)ret,
  (uint64_t)op13_wifi_crm_read(0,OP13_CRM_CURRENT_PERF),(uint64_t)op13_wifi_crm_read(0,OP13_CRM_SW_IRQ));
 if(ret){op13_wifi_votes_stuck=1;return;}
 print("op13-wifi-cesta: Gen1 vote and restoration passed; no radio traffic or DMA\n");
}
