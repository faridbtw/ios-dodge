/* Add only the captured Dodge PCIe MMIO ranges through the public CPU protocol.
 * AT checks never read the target device. Refuse nonidentity existing mappings.
 * CPU SetMemoryAttributes at +0x38 is verified against Mu basecore Cpu.h;
 * CpuMmuCommon.c + ArmMmuLibCore.c create missing mappings for EFI_MEMORY_UC.
 */
struct op13_wifi_cpu_arch {
 void *prefix[7];
 efi_status (*set_memory_attributes)(struct op13_wifi_cpu_arch *,uint64_t,uint64_t,uint64_t);
};
_Static_assert(offsetof(struct op13_wifi_cpu_arch,set_memory_attributes)==0x38,"CPU attributes ABI");
static int
op13_wifi_map_registers(void)
{
 static const efi_guid guid=EFI_CPU_ARCH_GUID;
 static const struct {uint64_t base,size;} regions[]={
  {0x0016b000,0x2000},{0x01c00000,0x8000},{0x01d01000,0x5000},{0x01d07000,0x7000},
  {0x40000000,0x2000},{0x40100000,0x100000},{0x40300000,0x300000}
 };
 struct op13_wifi_cpu_arch *cpu=0;
 efi_status s=BS->locate_protocol(&guid,0,(void **)&cpu);
 print("op13-wifi-map: CPU protocol status=%x\n",s);
 if(EFI_ERROR(s)||!cpu||!cpu->set_memory_attributes) return -1;
 for(unsigned i=0;i<sizeof(regions)/sizeof(regions[0]);i++) {
  uint64_t base=regions[i].base,size=regions[i].size;
  uint64_t par=uefi_page_translation(base);
  print("op13-wifi-map: before base=%x size=%x PAR=%x\n",base,size,par);
  for(uint64_t at=base;at<base+size;at+=0x1000) {
   par=uefi_page_translation(at);
   if(!(par&1)&&((par&0x000ffffffffff000ULL)!=at)) {
    print("op13-wifi-map: nonidentity mapping refused at=%x\n",at);return -1;
   }
  }
  /* UC maps device memory, and XP prevents execution from the PCIe aperture. */
  s=cpu->set_memory_attributes(cpu,base,size,0x4001);
  print("op13-wifi-map: map status=%x\n",s);
  if(EFI_ERROR(s)) return -1;
  for(uint64_t at=base;at<base+size;at+=0x1000) {
   par=uefi_page_translation(at);
   if((par&1)||((par&0x000ffffffffff000ULL)!=at)) {
    print("op13-wifi-map: translation verification failed at=%x PAR=%x\n",at,par);return -1;
   }
  }
  print("op13-wifi-map: verified base=%x size=%x\n",base,size);
 }
 print("op13-wifi-map: complete; no target registers read or written\n");return 0;
}
