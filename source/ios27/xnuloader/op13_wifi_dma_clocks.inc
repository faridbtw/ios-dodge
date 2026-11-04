/* SPDX-License-Identifier: BSD-2-Clause
 * Explicit PCIe DMA/TBU clock votes through the verified firmware ABI only.
 * Names taken from actual Dodge ClockDxe. No power-domain or global bypass.
 */
static int op13_wifi_dma_clocks_ready,op13_wifi_dma_clocks_attempted;
static int op13_wifi_dma_clocks(void)
{
 if(!op13_wifi_dma_mapped||op13_wifi_dma_clocks_attempted)return -1;
 op13_wifi_dma_clocks_attempted=1;
 const char *names[]={"gcc_mmu_tcu_vote_clk","gcc_hlos1_vote_all_smmu_mmu_clk",
  "gcc_hlos1_vote_aggre_noc_mmu_pcie_qtb_clk","gcc_cnoc_pcie_sf_axi_clk",
  "gcc_ddrss_pcie_sf_qtb_clk"};
 uint64_t ids[5]={0};struct op13_wifi_clock_protocol *p=0;
 efi_status s=BS->locate_protocol(&op13_wifi_clock_guid,0,(void **)&p);
 if(EFI_ERROR(s)||!p||p->revision!=0x1000b||!p->get_id||!p->enable||!p->is_on)return -2;
 for(unsigned i=0;i<5;i++){
  s=p->get_id(p,names[i],&ids[i]);uint8_t on=0;efi_status t=~0ULL;
  if(!EFI_ERROR(s))t=p->is_on(p,ids[i],&on);
  print("op13-wifi-dma: clock %s resolve=%x id=%x state=%x on=%d\n",names[i],s,ids[i],t,(uint64_t)on);
  if(EFI_ERROR(s)||!ids[i]||EFI_ERROR(t))return -3;
  for(unsigned j=0;j<i;j++)if(ids[i]==ids[j])return -3;
 }
 for(unsigned i=0;i<5;i++){
  fb_reply("INFO",names[i]);
  s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -4;
  s=p->enable(p,ids[i]);BS->set_watchdog_timer(0,0,0,0);
  uint8_t on=0;efi_status t=p->is_on(p,ids[i],&on);
  print("op13-wifi-dma: clock enable %s status=%x state=%x on=%d; retained\n",names[i],s,t,(uint64_t)on);
  if(EFI_ERROR(s)||EFI_ERROR(t))return -5;
 }
 op13_wifi_dma_clocks_ready=1;
 print("op13-wifi-dma: dedicated DMA clock votes accepted; no GDS votes or DMA yet\n");return 0;
}
