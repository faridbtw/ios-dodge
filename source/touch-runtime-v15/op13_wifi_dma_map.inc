/* SPDX-License-Identifier: BSD-2-Clause
 * Private SID1401 translation only; PCI bus mastering remains OFF.
 * Context register formats/order cross-checked with updated iphont hk_wifi.c.
 * Resource allocations and context retained until cold reset, including errors.
 */
static uint64_t op13_wifi_dma_pool,op13_wifi_dma_tables;
static int op13_wifi_dma_attempted,op13_wifi_dma_mapped;
static uint32_t op13_wifi_dma_cb_read(unsigned off)
{
 struct op13_wifi_smmu_plan *p=&op13_wifi_dma_plan;
 uint64_t cb=0x15000000ULL+(p->pages+p->context)*(uint64_t)p->page;
 return *(volatile uint32_t *)(uintptr_t)(cb+off);
}
static void op13_wifi_dma_cb_write(unsigned off,uint32_t value)
{
 /* This function can address only the verified unused context page. */
 const unsigned allowed[]={0,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c,0x58,0x610,0x7f0};
 int ok=0;for(unsigned i=0;i<sizeof(allowed)/sizeof(*allowed);i++)if(off==allowed[i])ok=1;
 if(!ok)return;
 struct op13_wifi_smmu_plan *p=&op13_wifi_dma_plan;
 uint64_t cb=0x15000000ULL+(p->pages+p->context)*(uint64_t)p->page;
 *(volatile uint32_t *)(uintptr_t)(cb+off)=value;
 __asm__ volatile("dsb sy":::"memory");
}
static int op13_wifi_dma_map(void)
{
 if(!op13_wifi_smmu_context_ready||op13_wifi_dma_attempted)return -1;
 op13_wifi_dma_attempted=1;
 struct op13_wifi_smmu_plan *p=&op13_wifi_dma_plan;
 if(p->context>=p->contexts||p->slot>=p->matches||p->page!=4096||p->pages!=128)return -2;
 if((op13_wifi_mhi_read(0x40100004)&7)!=2||(op13_wifi_mhi_read(0x40000004)&7)!=2)return -2;
 volatile uint32_t *g=(volatile uint32_t *)(uintptr_t)0x15000000;
 uint32_t smrs[256]={0},s2crs[256]={0};struct op13_wifi_smmu_plan check;
 for(unsigned i=0;i<p->matches;i++){smrs[i]=g[(0x800+4*i)/4];s2crs[i]=g[(0xc00+4*i)/4];}
 if(op13_wifi_smmu_choose(g[0],g[8],g[9],smrs,s2crs,&check)||check.slot!=p->slot||check.context!=p->context)return -3;
 if(op13_wifi_dma_cb_read(0)&1)return -3;
 uint64_t pool_alloc=(1ULL<<36)-1,tables=(1ULL<<36)-1;
 efi_status s=BS->allocate_pages(AllocateMaxAddress,EfiLoaderData,2*OP13_WIFI_DMA_BYTES/4096,&pool_alloc);
 if(EFI_ERROR(s))return -4;
 uint64_t pool=(pool_alloc+OP13_WIFI_DMA_BYTES-1)&~(OP13_WIFI_DMA_BYTES-1);
 if(pool<pool_alloc||pool+OP13_WIFI_DMA_BYTES>pool_alloc+2*OP13_WIFI_DMA_BYTES)return -4;
 s=BS->allocate_pages(AllocateMaxAddress,EfiLoaderData,OP13_WIFI_TABLES,&tables);
 if(EFI_ERROR(s))return -4;
 for(uint64_t at=tables;at<tables+OP13_WIFI_TABLES*4096ULL;at+=4096){
  uint64_t par=uefi_page_translation(at);
  if((par&1)||((par&0x000ffffffffff000ULL)!=at))return -5;
 }
 const efi_guid guid=EFI_CPU_ARCH_GUID;struct op13_wifi_cpu_arch *cpu=0;
 s=BS->locate_protocol(&guid,0,(void **)&cpu);
 if(EFI_ERROR(s)||!cpu||!cpu->set_memory_attributes)return -5;
 for(uint64_t at=pool;at<pool+OP13_WIFI_DMA_BYTES;at+=4096){
  uint64_t par=uefi_page_translation(at);
  if((par&1)||((par&0x000ffffffffff000ULL)!=at))return -5;
 }
 /* Noncacheable CPU alias matching the SMMU's normal-NC MAIR index0. */
 flush_dcache(pool,pool+OP13_WIFI_DMA_BYTES);
 s=cpu->set_memory_attributes(cpu,pool,OP13_WIFI_DMA_BYTES,0x4002);
 if(EFI_ERROR(s))return -6;
 if(op13_wifi_smmu_tables((uint64_t (*)[512])(uintptr_t)tables,tables,pool))return -6;
 flush_dcache(tables,tables+OP13_WIFI_TABLES*4096ULL);
 print("op13-wifi-dma: owned pool=%x bytes=%x tables=%x slot=%d context=%d\n",pool,OP13_WIFI_DMA_BYTES,tables,(uint64_t)p->slot,(uint64_t)p->context);
 fb_reply("INFO","Private Wi-Fi context programming next; PCI BME remains off");
 s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -7;
 volatile uint32_t *gr1=(volatile uint32_t *)(uintptr_t)(0x15000000ULL+p->page);
 op13_wifi_dma_cb_write(0,0);
 gr1[(0x800+4*p->context)/4]=1;__asm__ volatile("dsb sy":::"memory");
 gr1[p->context]=(1u<<16)|(3u<<8)|(15u<<12);__asm__ volatile("dsb sy":::"memory");
 op13_wifi_dma_cb_write(0x10,1u|(1u<<4)|(7u<<15));
 op13_wifi_dma_cb_write(0x30,25u|(1u<<8)|(1u<<10)|(3u<<12)|(1u<<23));
 op13_wifi_dma_cb_write(0x20,(uint32_t)tables);op13_wifi_dma_cb_write(0x24,(uint32_t)(tables>>32));
 op13_wifi_dma_cb_write(0x28,0);op13_wifi_dma_cb_write(0x2c,0);
 op13_wifi_dma_cb_write(0x38,0xf404ff44);op13_wifi_dma_cb_write(0x3c,0);
 op13_wifi_dma_cb_write(0x58,0xc00001fe);op13_wifi_dma_cb_write(0x610,p->context);
 op13_wifi_dma_cb_write(0x7f0,1);
 unsigned i;for(i=0;i<1000;i++){if(!(op13_wifi_dma_cb_read(0x7f4)&1))break;BS->stall(100);}
 if(i==1000){BS->set_watchdog_timer(0,0,0,0);return -8;}
 op13_wifi_dma_cb_write(0,(1u<<0)|(1u<<1)|(1u<<2)|(1u<<5)|(1u<<8)|(1u<<12));
 g[(0xc00+4*p->slot)/4]=p->context;__asm__ volatile("dsb sy":::"memory");
 g[(0x800+4*p->slot)/4]=0x80001401;__asm__ volatile("dsb sy":::"memory");
 uint32_t smr=g[(0x800+4*p->slot)/4],s2cr=g[(0xc00+4*p->slot)/4],ctl=op13_wifi_dma_cb_read(0);
 print("op13-wifi-dma: readback SMR=%x S2CR=%x SCTLR=%x TTBR0=%x:%x\n",(uint64_t)smr,(uint64_t)s2cr,(uint64_t)ctl,(uint64_t)op13_wifi_dma_cb_read(0x24),(uint64_t)op13_wifi_dma_cb_read(0x20));
 BS->set_watchdog_timer(0,0,0,0);
 if(smr!=0x80001401||(s2cr&0x300ff)!=p->context||!(ctl&1)||op13_wifi_dma_cb_read(0x20)!=(uint32_t)tables||op13_wifi_dma_cb_read(0x24)!=(uint32_t)(tables>>32))return -9;
 op13_wifi_dma_pool=pool;op13_wifi_dma_tables=tables;op13_wifi_dma_mapped=1;
 print("op13-wifi-dma: private mapping configured; BME OFF, no device DMA or firmware transfer performed\n");return 0;
}
