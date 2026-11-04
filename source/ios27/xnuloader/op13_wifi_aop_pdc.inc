/* SPDX-License-Identifier: BSD-2-Clause
 * Required cnss_aop_pdc_reconfig() before radio power-on, actual Dodge DT.
 * Firmware channel is reused if open; pending messages are never overwritten.
 * Only our QMP handshake/box and AOP IPCC doorbell are written.
 */
#include "op13_wifi_qmp.h"
static uint32_t op13_wifi_aop_box;
static int op13_wifi_aop_fault;
static uint32_t op13_wifi_aop_read(void *c,uint32_t o)
{
 (void)c;
 if((o&3)||o>=0x400){op13_wifi_aop_fault=1;return ~0u;}
 return *(volatile uint32_t *)(uintptr_t)(0xc300000+o);
}
static void op13_wifi_aop_write(void *c,uint32_t o,uint32_t v)
{
 (void)c;
 int handshake=o==0x10||o==0x18||o==0x24||o==0x2c;
 int message=op13_wifi_aop_box>=0x3c&&op13_wifi_aop_box<=0x3bc&&o>=op13_wifi_aop_box&&o<=op13_wifi_aop_box+64;
 if(op13_wifi_aop_fault||(o&3)||o>=0x400||(!handshake&&!message)){
  op13_wifi_aop_fault=1;print("op13-wifi-aop: refused write offset=%x\n",(uint64_t)o);return;
 }
 *(volatile uint32_t *)(uintptr_t)(0xc300000+o)=v;__asm__ volatile("dsb sy":::"memory");
}
static void op13_wifi_aop_ring(void *c)
{
 (void)c;if(op13_wifi_aop_fault)return;
 *(volatile uint32_t *)(uintptr_t)0x40600c=0;__asm__ volatile("dsb sy":::"memory");
}
static void op13_wifi_aop_delay(void *c,unsigned us){(void)c;BS->stall(us);}
static int op13_wifi_aop_pdc(void)
{
 const efi_guid guid=EFI_CPU_ARCH_GUID;struct op13_wifi_cpu_arch *cpu=0;
 efi_status s=BS->locate_protocol(&guid,0,(void **)&cpu);
 if(EFI_ERROR(s)||!cpu||!cpu->set_memory_attributes)return -1;
 const uint64_t pages[]={0xc300000,0x406000};
 for(unsigned i=0;i<2;i++){
  uint64_t par=uefi_page_translation(pages[i]);
  if(!(par&1)&&((par&0x000ffffffffff000ULL)!=pages[i]))return -2;
  s=cpu->set_memory_attributes(cpu,pages[i],4096,0x4001);
  par=uefi_page_translation(pages[i]);
  if(EFI_ERROR(s)||(par&1)||((par&0x000ffffffffff000ULL)!=pages[i]))return -2;
 }
 struct op13_wifi_qmp_io io={0,op13_wifi_aop_read,op13_wifi_aop_write,op13_wifi_aop_ring,op13_wifi_aop_delay};
 s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -3;
 uint32_t box=0;int rc=op13_wifi_qmp_box(&io,&box);
 if(!rc){op13_wifi_aop_box=box;rc=op13_wifi_qmp_open(&io,&box);}
 print("op13-wifi-aop: QMP open=%d box=%x\n",(uint64_t)(int64_t)rc,(uint64_t)box);
 if(!rc)for(unsigned i=0;i<4;i++){
  rc=op13_wifi_qmp_send(&io,box,op13_wifi_pdc_messages[i]);
  print("op13-wifi-aop: WLAN PDC init message=%d result=%d\n",(uint64_t)i,(uint64_t)(int64_t)rc);
  if(rc||op13_wifi_aop_fault)break;
 }
 BS->set_watchdog_timer(0,0,0,0);
 if(!rc&&!op13_wifi_aop_fault)print("op13-wifi-aop: all4 DT PDC messages consumed; radio power-on can proceed\n");
 return rc||op13_wifi_aop_fault?-4:0;
}
