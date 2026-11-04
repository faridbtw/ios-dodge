/* SPDX-License-Identifier: BSD-2-Clause
 * Global SMMU configuration reads only. Actual Apps SMMU DT has no clocks or
 * supplies: vendor arm_smmu_device_probe reads these with empty power resources.
 * New OnePlus8 reference likewise reads globals without a ClockDxe GDS vote.
 * No context-bank reads (occupied display contexts can be protected), no votes,
 * no SMMU writes, and no DMA. One INFO receipt before each possibly blocking read.
 */
#include "op13_wifi_smmu_plan.h"
static int op13_wifi_smmu_context_ready;
static struct op13_wifi_smmu_plan op13_wifi_dma_plan;
static int op13_wifi_smmu_survey(unsigned step)
{
 if(step!=9)return -1;
 const efi_guid guid=EFI_CPU_ARCH_GUID;struct op13_wifi_cpu_arch *cpu=0;
 efi_status s=BS->locate_protocol(&guid,0,(void **)&cpu);
 if(EFI_ERROR(s)||!cpu||!cpu->set_memory_attributes)return -2;
 for(uint64_t at=0x15000000;at<0x15002000;at+=0x1000){
  uint64_t par=uefi_page_translation(at);
  if(!(par&1)&&((par&0x000ffffffffff000ULL)!=at))return -2;
 }
 s=cpu->set_memory_attributes(cpu,0x15000000,0x2000,0x4001);
 if(EFI_ERROR(s))return -2;
 for(uint64_t at=0x15000000;at<0x15002000;at+=0x1000){
  uint64_t par=uefi_page_translation(at);
  if((par&1)||((par&0x000ffffffffff000ULL)!=at))return -2;
 }
 s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -3;
 volatile const uint32_t *regs=(volatile const uint32_t *)(uintptr_t)0x15000000;
 fb_reply("INFO","SMMU IDR0 read next; no ClockDxe power vote");
 uint32_t idr0=regs[0x20/4];
 fb_reply("INFO","IDR0 returned; SMMU IDR1 read next");
 uint32_t idr1=regs[0x24/4];
 fb_reply("INFO","IDR1 returned; SMMU SCR0 read next");
 uint32_t scr=regs[0];
 print("op13-wifi-smmu: IDR0=%x IDR1=%x SCR0=%x\n",(uint64_t)idr0,(uint64_t)idr1,(uint64_t)scr);
 fb_reply("INFO","SMMU globals returned; stream map reads next");
 uint32_t matches=idr0&255,page=(idr1>>31)?0x10000:0x1000,pages=2u<<((idr1>>28)&7),contexts=idr1&255;
 if(idr0==~0u||idr1==~0u||!matches||!contexts||contexts>pages||2u*pages*page!=0x100000){
  BS->set_watchdog_timer(0,0,0,0);return -4;
 }
 print("op13-wifi-smmu: matches=%d contexts=%d page=%x pages=%d\n",(uint64_t)matches,(uint64_t)contexts,(uint64_t)page,(uint64_t)pages);
 uint32_t smrs[256]={0},s2crs[256]={0};
 for(unsigned i=0;i<matches;i++){
  uint32_t smr=regs[(0x800+4*i)/4],s2cr=regs[(0xc00+4*i)/4];
  smrs[i]=smr;s2crs[i]=s2cr;
  if((smr>>31)||(s2cr&(1u<<10)))print("op13-wifi-smmu: SMR%d=%x S2CR=%x\n",(uint64_t)i,(uint64_t)smr,(uint64_t)s2cr);
 }
 if(op13_wifi_smmu_choose(scr,idr0,idr1,smrs,s2crs,&op13_wifi_dma_plan)){
  BS->set_watchdog_timer(0,0,0,0);return -5;
 }
 struct op13_wifi_smmu_plan *p=&op13_wifi_dma_plan;
 uint64_t cb=0x15000000ULL+(p->pages+p->context)*(uint64_t)p->page;
 print("op13-wifi-smmu: candidate slot=%d context=%d address=%x; only unused context read\n",(uint64_t)p->slot,(uint64_t)p->context,cb);
 uint64_t par=uefi_page_translation(cb);
 if(!(par&1)&&((par&0x000ffffffffff000ULL)!=cb)){
  BS->set_watchdog_timer(0,0,0,0);return -6;
 }
 s=cpu->set_memory_attributes(cpu,cb,p->page,0x4001);
 par=uefi_page_translation(cb);
 if(EFI_ERROR(s)||(par&1)||((par&0x000ffffffffff000ULL)!=cb)){
  BS->set_watchdog_timer(0,0,0,0);return -6;
 }
 fb_reply("INFO","Unused SMMU context SCTLR read next; watchdog armed");
 volatile const uint32_t *context=(volatile const uint32_t *)(uintptr_t)cb;
 uint32_t ctl=context[0];
 print("op13-wifi-smmu: candidate SCTLR=%x\n",(uint64_t)ctl);
 if(ctl==~0u||(ctl&1)){
  BS->set_watchdog_timer(0,0,0,0);return -7;
 }
 uint32_t low=context[0x20/4],high=context[0x24/4],fsr=context[0x58/4];
 print("op13-wifi-smmu: candidate TTBR0=%x:%x FSR=%x\n",(uint64_t)high,(uint64_t)low,(uint64_t)fsr);
 BS->set_watchdog_timer(0,0,0,0);
 op13_wifi_smmu_context_ready=1;
 print("op13-wifi-smmu: free context verified; no SMMU writes or DMA\n");return 0;
}
