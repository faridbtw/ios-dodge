/* PCIe0 PHY-only bringup: endpoint stays in reset, no DMA or radio firmware.
 * All setup data comes from the phone FDT and pinned OnePlus PCIe/GCC drivers.
 * Resource retention is intentional: leave this probe by cold reset only.
 */
#include "op13_wifi_phy_data.h"
static int op13_wifi_phy_ready,op13_wifi_phy_fault;
static int op13_wifi_phy_address(uint32_t a)
{
 if(a&3)return 0;
 if(a>=0x01c06000&&a<0x01c08000)return 1;
 if(a==0x0016b000||a==0x0016c01c||a==0x0f166000||a==0x0f166004)return 1;
 if(a==0x01c00000||a==0x01c00040||a==0x01c01000||a==0x01c00174)return 1;
 if(a>=0x01d07000&&a<0x01d07034)return 1;
 if(a>=0x01d08000&&a<0x01d0801c)return 1;
 return a==0x01d08040||a==0x01d08048||a==0x01d08090;
}
static int op13_wifi_phy_access(uint32_t a)
{
 uint64_t par=uefi_page_translation(a);
 if(!op13_wifi_phy_address(a)||(par&1)||((par&0x000ffffffffff000ULL)!=(a&~0xfffULL))) {
  op13_wifi_phy_fault=1;print("op13-wifi-phy: refused register=%x PAR=%x\n",(uint64_t)a,par);return 0;
 }
 return 1;
}
static uint32_t op13_wifi_phy_read(uint32_t a)
{
 return op13_wifi_phy_access(a)?*(volatile uint32_t *)(uintptr_t)a:0xffffffffu;
}
static void op13_wifi_phy_write(uint32_t a,uint32_t v)
{
 if(!op13_wifi_phy_access(a))return;
 *(volatile uint32_t *)(uintptr_t)a=v;__asm__ volatile("dsb sy":::"memory");
}
static void op13_wifi_phy_update(uint32_t a,uint32_t clear,uint32_t set)
{
 uint32_t v=op13_wifi_phy_read(a);
 if(!op13_wifi_phy_fault)op13_wifi_phy_write(a,(v&~clear)|set);
}
static void op13_wifi_phy_probe(void)
{
 op13_wifi_phy_ready=op13_wifi_phy_fault=0;
 if(op13_wifi_map_registers())return;
 struct op13_wifi_clock_protocol *p=0;
 efi_status s=BS->locate_protocol(&op13_wifi_clock_guid,0,(void **)&p);
 const char *names[]={"gcc_pcie_0_mstr_axi_clk","gcc_pcie_0_slv_axi_clk", "gcc_pcie_0_slv_q2a_axi_clk",
  "gcc_pcie_0_pipe_clk","gcc_pcie_0_cfg_ahb_clk","gcc_pcie_0_phy_rchng_clk"};
 const uint64_t expected[]={0x4010029,0x401002a,0x401002b,0x4010056,0x4010036,0x4010055};
 uint64_t ids[6]={0};
 if(EFI_ERROR(s)||!p||p->revision!=0x1000b||!p->get_id||!p->enable||!p->is_on)return;
 for(unsigned i=0;i<6;i++) {
  s=p->get_id(p,names[i],&ids[i]);
  if(EFI_ERROR(s)||ids[i]!=expected[i]){print("op13-wifi-phy: unresolved clock %s\n",names[i]);return;}
 }
 for(unsigned i=0;i<sizeof(op13_wifi_phy_sequence)/sizeof(op13_wifi_phy_sequence[0]);i++) {
  const struct op13_wifi_phy_entry *e=&op13_wifi_phy_sequence[i];
  if(e->offset>=0x2000||(e->offset&3)||e->delay_us>10000)return;
 }
 if(!op13_wifi_phy_access(0x0f166000)||!op13_wifi_phy_access(0x0f166004))return;
 /* No subsequent loader/kernel run may reuse retained controller state. */
 op13_wifi_votes_stuck=1;
 print("op13-wifi-phy: retaining all resources until cold reset; no endpoint/DMA activation\n");
 print("op13-wifi-phy: PERST102 original config=%x data=%x\n",
  (uint64_t)op13_wifi_phy_read(0x0f166000),(uint64_t)op13_wifi_phy_read(0x0f166004));
 op13_wifi_phy_update(0x0f166004,2,0);
 op13_wifi_phy_update(0x0f166000,0x3ff,0x200); /* GPIO, no pull,2mA,output low */
 BS->stall(1000);
 op13_wifi_phy_write(0x01d08040,0);op13_wifi_phy_write(0x01d08048,0);
 for(unsigned i=0;i<sizeof(op13_wifi_sm_sequence)/sizeof(uint32_t);i++)
  op13_wifi_phy_write(0x01d07000+4*i,op13_wifi_sm_sequence[i]);
 for(unsigned i=0;i<sizeof(op13_wifi_sm_branches)/sizeof(uint32_t);i++)
  op13_wifi_phy_write(0x01d08000+4*i,op13_wifi_sm_branches[i]);
 op13_wifi_phy_update(0x01d08090,0,1);
 if(op13_wifi_phy_fault)return;
 print("op13-wifi-phy: PCIe state-manager sequence loaded\n");
 struct op13_cesta_io io={0,op13_wifi_crm_read,op13_wifi_crm_write,op13_wifi_crm_delay};
 uint32_t saved=0;int ret=op13_cesta_gen1_vote(&io,&saved);
 print("op13-wifi-phy: Gen1 vote result=%x current-perf=%x\n",(uint64_t)(int64_t)ret,
  (uint64_t)op13_wifi_crm_read(0,OP13_CRM_CURRENT_PERF));
 if(ret)return;
 for(unsigned i=0;i<3;i++) {
  s=p->enable(p,ids[i]);print("op13-wifi-phy: enable %s status=%x\n",names[i],s);
  if(EFI_ERROR(s))return;
 }
 /* DT reset IDs6/9 map to GCC offsets6b000/6c01c; pulse both in vendor order. */
 op13_wifi_phy_update(0x0016b000,0,1);op13_wifi_phy_update(0x0016c01c,0,1);BS->stall(1000);
 op13_wifi_phy_update(0x0016c01c,1,0);op13_wifi_phy_update(0x0016b000,1,0);BS->stall(1000);
 if(op13_wifi_phy_fault)return;
 for(unsigned i=4;i<6;i++) {
  uint8_t on=0;s=p->is_on(p,ids[i],&on);
  print("op13-wifi-phy: after reset %s status=%x on=%d\n",names[i],s,(uint64_t)on);
  if(EFI_ERROR(s)||!on)return;
 }
 op13_wifi_phy_write(0x01c01000,4);op13_wifi_phy_update(0x01c00040,1,0);
 op13_wifi_phy_write(0x01c00000,0x365e);op13_wifi_phy_update(0x01c00174,0,1u<<4);
 for(unsigned i=0;i<sizeof(op13_wifi_phy_sequence)/sizeof(op13_wifi_phy_sequence[0]);i++) {
  const struct op13_wifi_phy_entry *e=&op13_wifi_phy_sequence[i];
  op13_wifi_phy_write(0x01c06000+e->offset,e->value);
  if(op13_wifi_phy_fault)return;
  if(e->delay_us)BS->stall(e->delay_us);
 }
 BS->stall(1000);
 print("op13-wifi-phy:128 PHY configuration writes completed\n");
 s=p->enable(p,ids[3]);print("op13-wifi-phy: PIPE enable status=%x\n",s);
 if(EFI_ERROR(s))return;
 uint32_t status=0xffffffffu;
 for(unsigned i=0;i<1000;i++) {
  status=op13_wifi_phy_read(0x01c06414);
  if(op13_wifi_phy_fault)return;
  if(!(status&(1u<<6))) {op13_wifi_phy_ready=1;break;}
  BS->stall(100);
 }
 print("op13-wifi-phy: PHY ready=%d status=%x; endpoint remains in reset\n",(uint64_t)op13_wifi_phy_ready,(uint64_t)status);
 for(unsigned i=0;i<4;i++) {
  uint8_t on=0;s=p->is_on(p,ids[i],&on);
  print("op13-wifi-phy: final %s status=%x on=%d\n",names[i],s,(uint64_t)on);
 }
}
