/* SPDX-License-Identifier: BSD-2-Clause
 * Finite BHI bootstrap only, using the captured device firmware's first512KiB.
 * Protocol/order checked against updated iphont mhi_host.c and vendor CNSS.
 * No full firmware runtime, network traffic, or partition access.
 */
static int op13_wifi_bhi_attempted,op13_wifi_bhi_success;
static uint8_t op13_wifi_bdf_hash(uint16_t bdf)
{
 uint8_t crc=0,bytes[2]={(uint8_t)(bdf>>8),(uint8_t)bdf};
 for(unsigned b=0;b<2;b++){
  crc^=bytes[b];for(unsigned i=0;i<8;i++)crc=(uint8_t)((crc&128)?(crc<<1)^7:crc<<1);
 }return crc;
}
static uint32_t op13_wifi_bhi_read(unsigned off)
{
 const unsigned allowed[]={0x28,0x38,0x48,0x128,0x12c,0x130,0x134,0x138,0x13c};
 int ok=0;for(unsigned i=0;i<sizeof(allowed)/sizeof(*allowed);i++)if(off==allowed[i])ok=1;
 return ok?*(volatile uint32_t *)(uintptr_t)(0x40400000+off):~0u;
}
static void op13_wifi_bhi_write(unsigned off,uint32_t value)
{
 const unsigned allowed[]={0x108,0x10c,0x110,0x118,0x120,0x12c};
 int ok=0;for(unsigned i=0;i<sizeof(allowed)/sizeof(*allowed);i++)if(off==allowed[i])ok=1;
 if(!ok)return;
 *(volatile uint32_t *)(uintptr_t)(0x40400000+off)=value;__asm__ volatile("dsb sy":::"memory");
}
static int op13_wifi_mhi_prepare(void)
{
 if(!op13_wifi_dma_mapped||!op13_wifi_dma_clocks_ready||op13_wifi_bhi_attempted)return -1;
 op13_wifi_bhi_attempted=1;
 /* Whole local firmware checksum prevents booting an incomplete/wrong staged file. */
 const uint8_t *image=(const uint8_t *)(uintptr_t)PAYLOAD_BASE;
 if(ufs_fnv(0xcbf29ce484222325ULL,image,6840384)!=0x23266c0280baa80fULL)return -2;
 if(op13_wifi_bhi_read(0x28)!=0x100||op13_wifi_bhi_read(0x128)!=0||op13_wifi_bhi_read(0x130))return -3;
 if((op13_wifi_mhi_read(0x40100004)&7)!=2||(op13_wifi_mhi_read(0x40000004)&7)!=2)return -3;
 if(op13_wifi_dma_cb_read(0x58)&0xc00001fe)return -3;
 memcpy((void *)(uintptr_t)op13_wifi_dma_pool,image,0x80000);__asm__ volatile("dsb sy":::"memory");
 uint64_t original=ufs_fnv(0xcbf29ce484222325ULL,image,0x80000);
 uint64_t copied=ufs_fnv(0xcbf29ce484222325ULL,(const uint8_t *)(uintptr_t)op13_wifi_dma_pool,0x80000);
 print("op13-wifi-bhi: bootstrap buffer roundtrip=%d\n",(uint64_t)(original==copied));
 if(original!=copied)return -3;
 /* Program the dedicated RC0 BDF table only with DMA disabled. */
 volatile uint32_t *sid=(volatile uint32_t *)(uintptr_t)0x1c02000;
 volatile uint32_t *cfg=(volatile uint32_t *)(uintptr_t)0x1c02c00;
 uint8_t hash=op13_wifi_bdf_hash(0x100);
 if(!hash)return -4;
 print("op13-wifi-bhi: SID config=%x endpoint hash=%x entry=%x\n",(uint64_t)*cfg,(uint64_t)hash,(uint64_t)sid[hash]);
 if(*cfg&1){
  /* Measured cold root has BYPASS=1 and uninitialized SRAM, so none of
   * these entries routes traffic. Match vendor msm_pcie_config_sid():
   * initialize the dedicated RC0 table while both BME bits are off. */
  for(unsigned i=0;i<256;i++)sid[i]=0;
  __asm__ volatile("dsb sy":::"memory");
  sid[hash]=0x01000100;__asm__ volatile("dsb sy":::"memory");
  for(unsigned i=0;i<256;i++)if(sid[i]!=(i==hash?0x01000100u:0u))return -4;
 }else{
  /* If translation is already active preserve every entry and chain. */
  if((sid[hash]&0xffffff00)!=0x01000100)return -4;
 }
 *cfg&=~1u;__asm__ volatile("dsb sy":::"memory");
 if((sid[hash]&0xffffff00)!=0x01000100||(*cfg&1))return -4;
 /* The real Android PCI configuration uses the 32-bit MSI capability at50,
  * per-vector mask at5c. Match Android MME5; route all32vectors into an OWNED RAM sink, not the
  * GIC or firmware interrupts. This permits BHI completion writes safely. */
 uint64_t cfgpar=uefi_page_translation(0x40100050);
 if((cfgpar&1)||((cfgpar&0x000ffffffffff000ULL)!=0x40100000))return -4;
 volatile uint32_t *ep=(volatile uint32_t *)(uintptr_t)0x40100000;
 uint32_t cap=ep[0x50/4],ctl=cap>>16;
 if((cap&255)!=5||(ctl&0x80)||!(ctl&0x100)||(ctl&1)||((ctl>>1)&7)<5)return -4;
 volatile uint32_t *sink=(volatile uint32_t *)(uintptr_t)(op13_wifi_dma_pool+0x40);
 *sink=0xdeadbeef;__asm__ volatile("dsb sy":::"memory");
 ep[0x5c/4]=~0u;__asm__ volatile("dsb sy":::"memory");
 ep[0x54/4]=(uint32_t)(OP13_WIFI_IOVA+0x40);
 ep[0x58/4]=0;ep[0x5c/4]=~7u;
 ep[0x50/4]=(cap&~(0x71u<<16))|(0x51u<<16);
 __asm__ volatile("dsb sy":::"memory");
 print("op13-wifi-bhi: MSI vector0 owned RAM sink; control=%x address=%x mask=%x\n",(uint64_t)(ep[0x50/4]>>16),(uint64_t)ep[0x54/4],(uint64_t)ep[0x5c/4]);
 print("op13-wifi-bhi: BDF0100 maps SID1401 at hash=%x; bootstrap bytes=%x IOVA=%x\n",(uint64_t)hash,0x80000ULL,OP13_WIFI_IOVA);
 fb_reply("INFO","BHI firmware DMA test next; watchdog armed for20s");
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -5;
 /* Both PCI command registers: MEM|MASTER, no I/O decode. */
 op13_wifi_link_write(0x40000004,6);
 *(volatile uint32_t *)(uintptr_t)0x40100004=6;__asm__ volatile("dsb sy":::"memory");
 if((op13_wifi_mhi_read(0x40100004)&7)!=6||(op13_wifi_mhi_read(0x40000004)&7)!=6){
  BS->set_watchdog_timer(0,0,0,0);return -6;
 }
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-mhi: SID and MSI private-sink prepared; no BHI request yet\n");
 return 0;
}
