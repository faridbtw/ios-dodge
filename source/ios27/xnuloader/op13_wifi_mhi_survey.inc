/* SPDX-License-Identifier: BSD-2-Clause
 * CPU-only Peach BAR/MHI survey. No DMA, firmware transfer or interrupt setup.
 * MHI offsets: Linux drivers/bus/mhi/common.h, preserved in diagnostics.
 * iATU layout: pinned OnePlus pci-msm.c. BAR bases: captured Android resources.
 */
static int op13_wifi_mhi_ready,op13_wifi_mhi_fault;
static uint32_t op13_wifi_mhi_bhi;
static int op13_wifi_mhi_address(uint32_t a,int write)
{
 if(a&3)return 0;
 if(a==0x40100004||a==0x40100010||a==0x40100014||a==0x40100018||a==0x4010001c||a==0x40000020)return 1;
 if(a>=0x40001200&&a<=0x40001218)return 1; /* outbound region1 only */
 if(write)return 0;
 if(a==0x40100000||a==0x40000004)return 1;
 const uint32_t offsets[]={0,8,0x10,0x18,0x20,0x28,0x2c,0x38,0x48};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++)if(a==0x40400000+offsets[i])return 1;
 if(op13_wifi_mhi_bhi>=0x80&&op13_wifi_mhi_bhi<=0x1fff00&&!(op13_wifi_mhi_bhi&3)){
  const uint32_t bhi_offsets[]={0,4,0x28,0x2c,0x30};
  for(unsigned i=0;i<sizeof(bhi_offsets)/sizeof(*bhi_offsets);i++)
   if(a==0x40400000+op13_wifi_mhi_bhi+bhi_offsets[i])return 1;
 }
 return 0;
}
static uint32_t op13_wifi_mhi_read(uint32_t a)
{
 uint64_t par=uefi_page_translation(a);
 if(op13_wifi_mhi_fault||!op13_wifi_mhi_address(a,0)||(par&1)||((par&0x000ffffffffff000ULL)!=(a&~0xfffULL))){
  op13_wifi_mhi_fault=1;print("op13-wifi-mhi: refused read=%x\n",(uint64_t)a);return ~0u;
 }
 uint32_t value=*(volatile uint32_t *)(uintptr_t)a;
 __asm__ volatile("dsb sy":::"memory");return value;
}
static void op13_wifi_mhi_write(uint32_t a,uint32_t v)
{
 uint64_t par=uefi_page_translation(a);
 if(op13_wifi_mhi_fault||!op13_wifi_mhi_address(a,1)||(par&1)||((par&0x000ffffffffff000ULL)!=(a&~0xfffULL))||
    (a==0x40100004&&v!=0&&v!=2)){
  op13_wifi_mhi_fault=1;print("op13-wifi-mhi: refused write=%x\n",(uint64_t)a);return;
 }
 *(volatile uint32_t *)(uintptr_t)a=v;__asm__ volatile("dsb sy":::"memory");
}
static uint64_t op13_wifi_mhi_bar_size(uint32_t low,uint32_t high)
{
 if((low&0xf)!=4)return 0;
 uint64_t mask=((uint64_t)high<<32)|(low&~15u),size=~mask+1;
 if(!size||(size&(size-1)))return 0;
 return size;
}
static int op13_wifi_mhi_assign_bar(uint32_t cfg,uint32_t base,uint64_t expected)
{
 uint32_t lo=op13_wifi_mhi_read(cfg),hi=op13_wifi_mhi_read(cfg+4);
 if(op13_wifi_mhi_fault||(lo&0xf)!=4)return -1;
 /* Decode and bus mastering are both disabled for sizing/restoration. */
 if(op13_wifi_mhi_read(0x40100004)&7)return -1;
 op13_wifi_mhi_write(cfg,~0u);op13_wifi_mhi_write(cfg+4,~0u);
 uint32_t ml=op13_wifi_mhi_read(cfg),mh=op13_wifi_mhi_read(cfg+4);
 op13_wifi_mhi_write(cfg+4,hi);op13_wifi_mhi_write(cfg,lo);
 uint64_t size=op13_wifi_mhi_bar_size(ml,mh);
 print("op13-wifi-mhi: BAR config=%x mask=%x:%x size=%x expected=%x\n",(uint64_t)cfg,(uint64_t)mh,(uint64_t)ml,size,expected);
 if(op13_wifi_mhi_fault||size!=expected||(base&(size-1)))return -2;
 op13_wifi_mhi_write(cfg+4,0);op13_wifi_mhi_write(cfg,base|4);
 return op13_wifi_mhi_read(cfg)==(base|4)&&op13_wifi_mhi_read(cfg+4)==0&&!op13_wifi_mhi_fault?0:-3;
}
static void op13_wifi_mhi_survey(void)
{
 op13_wifi_mhi_ready=op13_wifi_mhi_fault=0;op13_wifi_mhi_bhi=0;
 op13_wifi_link_probe();if(!op13_wifi_link_ready)return;
 if(op13_wifi_mhi_read(0x40100000)!=0x110e17cb||(op13_wifi_mhi_read(0x40100004)&4))return;
 op13_wifi_mhi_write(0x40100004,0);
 if(op13_wifi_mhi_assign_bar(0x40100010,0x40400000,0x200000)||
    op13_wifi_mhi_assign_bar(0x40100018,0x40300000,0x8000))return;
 /* PCI bridge memory base/limit, 1MiB granularity: 40300000..405fffff. */
 op13_wifi_mhi_write(0x40000020,0x40504030);
 op13_wifi_mhi_write(0x40001204,0);op13_wifi_mhi_write(0x40001200,0);
 op13_wifi_mhi_write(0x40001208,0x40300000);op13_wifi_mhi_write(0x4000120c,0);
 op13_wifi_mhi_write(0x40001210,0x405fffff);op13_wifi_mhi_write(0x40001214,0x40300000);
 op13_wifi_mhi_write(0x40001218,0);op13_wifi_mhi_write(0x40001204,0x80000000);
 BS->stall(1000);
 if(op13_wifi_mhi_fault||!(op13_wifi_mhi_read(0x40001204)&0x80000000)||
    op13_wifi_mhi_read(0x40000020)!=0x40504030)return;
 op13_wifi_mhi_write(0x40100004,2);
 if((op13_wifi_mhi_read(0x40100004)&7)!=2||(op13_wifi_mhi_read(0x40000004)&7)!=2)return;
 print("op13-wifi-mhi: BAR windows enabled; CPU access only, bus mastering OFF\n");
 const uint32_t offsets[]={0,8,0x10,0x18,0x20,0x28,0x2c,0x38,0x48};
 uint32_t reglen=op13_wifi_mhi_read(0x40400000);
 print("op13-wifi-mhi: MHIREGLEN=%x\n",(uint64_t)reglen);
 if(op13_wifi_mhi_fault||reglen<0xa8||reglen>0x200000)return;
 for(unsigned i=1;i<sizeof(offsets)/sizeof(*offsets);i++)
  print("op13-wifi-mhi: register offset=%x value=%x\n",(uint64_t)offsets[i],(uint64_t)op13_wifi_mhi_read(0x40400000+offsets[i]));
 uint32_t bhi=op13_wifi_mhi_read(0x40400028);
 if(op13_wifi_mhi_fault||bhi<0x80||bhi>0x1fff00||(bhi&3))return;
 op13_wifi_mhi_bhi=bhi;
 const uint32_t bhi_offsets[]={0,4,0x28,0x2c,0x30};
 for(unsigned i=0;i<sizeof(bhi_offsets)/sizeof(*bhi_offsets);i++)
  print("op13-wifi-mhi: BHI offset=%x value=%x\n",(uint64_t)bhi_offsets[i],(uint64_t)op13_wifi_mhi_read(0x40400000+bhi+bhi_offsets[i]));
 if(!op13_wifi_mhi_fault)op13_wifi_mhi_ready=1;
 print("op13-wifi-mhi: survey-ready=%d; no firmware, DMA, scan or connection\n",(uint64_t)op13_wifi_mhi_ready);
}
