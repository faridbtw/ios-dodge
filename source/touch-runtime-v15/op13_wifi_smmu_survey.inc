/* SPDX-License-Identifier: BSD-2-Clause
 * One firmware resource vote per host command, with watchdog and INFO receipt.
 * SMMU register survey is read-only and requires clock-on confirmation first.
 * No audio resources, DMA, stream programming or context changes.
 */
static unsigned op13_wifi_smmu_next=1;
static const char *const op13_wifi_smmu_names[]={
 "gcc_hlos1_vote_all_smmu_mmu_gds","gcc_hlos1_vote_mmu_tcu_gds",
 "gcc_hlos1_vote_aggre_noc_mmu_pcie_qtb_gds",
 "gcc_hlos1_vote_all_smmu_mmu_clk","gcc_hlos1_vote_mmu_tcu_clk",
 "gcc_hlos1_vote_aggre_noc_mmu_pcie_qtb_clk","gcc_mmu_tcu_vote_clk"
};
static int op13_wifi_smmu_survey(unsigned step)
{
 struct op13_wifi_clock_protocol *p=0;uint64_t ids[7]={0};
 efi_status s=BS->locate_protocol(&op13_wifi_clock_guid,0,(void **)&p);
 if(EFI_ERROR(s)||!p||p->revision!=0x1000b||!p->get_id||!p->enable||!p->is_on)return -1;
 for(unsigned i=0;i<7;i++){
  s=p->get_id(p,op13_wifi_smmu_names[i],&ids[i]);
  print("op13-wifi-smmu: resolve %s status=%x id=%x\n",op13_wifi_smmu_names[i],s,ids[i]);
  if(EFI_ERROR(s)||!ids[i])return -2;
  for(unsigned j=0;j<i;j++)if(ids[i]==ids[j])return -2;
 }
 if(step==0){
  for(unsigned i=3;i<7;i++){
   uint8_t on=0;s=p->is_on(p,ids[i],&on);
   print("op13-wifi-smmu: clock %s status=%x on=%d\n",op13_wifi_smmu_names[i],s,(uint64_t)on);
  }
  return 0;
 }
 if(step>=1&&step<=7){
  if(step!=op13_wifi_smmu_next)return -3;
  /* INFO is sent synchronously before every potentially blocking operation. */
  fb_reply("INFO",op13_wifi_smmu_names[step-1]);
  s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -4;
  s=p->enable(p,ids[step-1]);
  BS->set_watchdog_timer(0,0,0,0);
  print("op13-wifi-smmu: enable step=%d %s status=%x; retained until reset\n",(uint64_t)step,op13_wifi_smmu_names[step-1],s);
  if(EFI_ERROR(s))return -5;
  op13_wifi_smmu_next++;
  return 0;
 }
 if(step!=9||op13_wifi_smmu_next!=8)return -6;
 for(unsigned i=3;i<6;i++){
  uint8_t on=0;s=p->is_on(p,ids[i],&on);
  print("op13-wifi-smmu: pre-read %s status=%x on=%d\n",op13_wifi_smmu_names[i],s,(uint64_t)on);
  if(EFI_ERROR(s)||!on)return -7;
 }
 const efi_guid guid=EFI_CPU_ARCH_GUID;struct op13_wifi_cpu_arch *cpu=0;
 s=BS->locate_protocol(&guid,0,(void **)&cpu);
 if(EFI_ERROR(s)||!cpu||!cpu->set_memory_attributes)return -8;
 for(uint64_t at=0x15000000;at<0x15100000;at+=0x1000){
  uint64_t par=uefi_page_translation(at);
  if(!(par&1)&&((par&0x000ffffffffff000ULL)!=at))return -8;
 }
 s=cpu->set_memory_attributes(cpu,0x15000000,0x100000,0x4001);
 if(EFI_ERROR(s))return -8;
 for(uint64_t at=0x15000000;at<0x15100000;at+=0x1000){
  uint64_t par=uefi_page_translation(at);
  if((par&1)||((par&0x000ffffffffff000ULL)!=at))return -8;
 }
 fb_reply("INFO","SMMU IDR read next; watchdog armed for 20 seconds");
 s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -9;
 volatile const uint32_t *regs=(volatile const uint32_t *)(uintptr_t)0x15000000;
 uint32_t idr0=regs[0x20/4],idr1=regs[0x24/4],scr=regs[0];
 print("op13-wifi-smmu: IDR0=%x IDR1=%x SCR0=%x\n",(uint64_t)idr0,(uint64_t)idr1,(uint64_t)scr);
 fb_reply("INFO","SMMU IDR reads returned; checking stream ownership");
 uint32_t matches=idr0&255,page=(idr1>>31)?0x10000:0x1000,pages=2u<<((idr1>>28)&7),contexts=idr1&255;
 if(idr0==~0u||idr1==~0u||!matches||!contexts||contexts>pages||2u*pages*page!=0x100000){
  BS->set_watchdog_timer(0,0,0,0);return -10;
 }
 print("op13-wifi-smmu: matches=%d contexts=%d page=%x pages=%d\n",(uint64_t)matches,(uint64_t)contexts,(uint64_t)page,(uint64_t)pages);
 for(unsigned i=0;i<matches;i++){
  uint32_t smr=regs[(0x800+4*i)/4],s2cr=regs[(0xc00+4*i)/4];
  if(smr>>31)print("op13-wifi-smmu: SMR%d=%x S2CR=%x\n",(uint64_t)i,(uint64_t)smr,(uint64_t)s2cr);
 }
 for(unsigned i=0;i<contexts;i++){
  uint32_t sctlr=regs[((pages+i)*page)/4];
  print("op13-wifi-smmu: CB%d SCTLR=%x\n",(uint64_t)i,(uint64_t)sctlr);
 }
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-smmu: read-only survey complete; no SMMU writes or DMA\n");return 0;
}
