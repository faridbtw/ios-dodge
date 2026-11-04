/* SPDX-License-Identifier: BSD-2-Clause
 * Native OnePlus13 PMIC observer reader. buttons=2 selects this driver only;
 * buttons_steps defaults to 1 (version/count read, no SPMI transaction).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/op13_buttons.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern kern_return_t thread_terminate(thread_t);
static vm_offset_t regions[3];
static const unsigned sizes[3]={OP13_CORE_SIZE,OP13_CNFG_SIZE,OP13_OBSERVER_SIZE};
static struct op13_buttons keys;

static uint32_t read32(void *cookie,enum op13_region region,uint32_t offset)
{
 (void)cookie;
 if((unsigned)region>=3||!regions[region]||offset>sizes[region]-4||(offset&3))
  return 0xffffffffu;
 uint32_t value=*(volatile uint32_t *)(regions[region]+offset);
 __asm__ volatile("dmb oshld" ::: "memory");
 return value;
}
static void issue_read(void *cookie,uint32_t offset,uint32_t command)
{
 (void)cookie;
 /* Defence in depth: only observer EXT_READL; no writes to data/config/IRQ. */
 if(!regions[OP13_OBSERVER]||offset>OP13_OBSERVER_SIZE-0x20||(offset&0x1f)||
    (command&~0xff7u)!=(1u<<27)) return;
 __asm__ volatile("dmb osh" ::: "memory");
 *(volatile uint32_t *)(regions[OP13_OBSERVER]+offset)=command;
 __asm__ volatile("dsb osh" ::: "memory");
}
static void lock(void *cookie) { (void)cookie; redwood_spmi_lock(); }
static void unlock(void *cookie) { (void)cookie; redwood_spmi_unlock(); }
static uint64_t time_us(void *cookie)
{
 uint64_t ns; (void)cookie;
 absolutetime_to_nanoseconds(mach_absolute_time(),&ns); return ns/1000;
}
static void busy_us(void *cookie,unsigned us)
{
 uint64_t interval,start=mach_absolute_time(); (void)cookie;
 nanoseconds_to_absolutetime((uint64_t)us*NSEC_PER_USEC,&interval);
 /* Called while the existing shared SPMI spinlock is held: never sleep. */
 while(mach_absolute_time()-start<interval) __asm__ volatile("yield");
}
static void event(void *cookie,unsigned key,unsigned pressed)
{
 static const unsigned mapping[3]={REDWOOD_BUTTON_LOCK,REDWOOD_BUTTON_VOLUME_UP,REDWOOD_BUTTON_VOLUME_DOWN};
 (void)cookie;
 if(key>=3) return;
 printf("op13-buttons: key=%u %s\n",key,pressed?"down":"up");
 redwood_touch_post(pressed?REDWOOD_BUTTON_DOWN:REDWOOD_BUTTON_UP,mapping[key],0,0);
}
static void diagnostic(void *cookie,const char *message,uint32_t a,uint32_t b)
{ (void)cookie; printf("op13-buttons: %s: %#x %#x\n",message,a,b); }

static void worker(void *parameter,wait_result_t result)
{
 unsigned steps=(unsigned)(uintptr_t)parameter;
 int e=OP13_IO; (void)result;
 struct op13_host host={NULL,read32,issue_read,lock,unlock,busy_us,time_us,event,diagnostic};
 printf("op13-buttons: stage1 map core %#x, version/count only\n",OP13_CORE_PA);
 regions[OP13_CORE]=ml_io_map(OP13_CORE_PA,OP13_CORE_SIZE);
 if(!regions[OP13_CORE]) goto done;
 e=op13_version(&keys,&host); if(e||steps==1) goto done;
 printf("op13-buttons: stage2 APID map and ownership, no SPMI commands\n");
 regions[OP13_CNFG]=ml_io_map(OP13_CNFG_PA,OP13_CNFG_SIZE);
 if(!regions[OP13_CNFG]) { e=OP13_IO; goto done; }
 e=op13_find_apids(&keys); if(e||steps==2) goto done;
 printf("op13-buttons: stage3 observer type reads only; SID0/1300 SID1/8d00\n");
 regions[OP13_OBSERVER]=ml_io_map(OP13_OBSERVER_PA,OP13_OBSERVER_SIZE);
 if(!regions[OP13_OBSERVER]) { e=OP13_IO; goto done; }
 e=op13_identify(&keys); if(e||steps==3) goto done;
 printf("op13-buttons: stage4 validate input config, no PMIC writes\n");
 e=op13_validate_inputs(&keys); if(e||steps==4) goto done;
 printf("op13-buttons: stage5 poll8ms/debounce32ms; startup-held keys await release\n");
 for(;;) {
  e=op13_poll(&keys); if(e) break;
  delay_for_interval(8,NSEC_PER_MSEC);
 }
done:
 op13_stop(&keys);
 printf("op13-buttons: stopped error=%d requested_stage=%u version=%#x reads=%u status=%#x\n",
  e,steps,keys.version,keys.transactions,keys.last_status);
 /* Mappings are static for this boot; no firmware/PMIC state was changed. */
 thread_terminate(current_thread());
}

void redwood_op13_buttons_init(void)
{
 int enabled=0,steps=1; thread_t thread;
 PE_parse_boot_argn("buttons",&enabled,sizeof(enabled));
 if(enabled!=2) return;
 PE_parse_boot_argn("buttons_steps",&steps,sizeof(steps));
 if(steps<1||steps>5) {
  printf("op13-buttons: buttons_steps must be 1..5; no MMIO\n"); return;
 }
 if(kernel_thread_start(worker,(void *)(uintptr_t)steps,&thread)==KERN_SUCCESS)
  thread_deallocate(thread);
 else printf("op13-buttons: cannot create worker\n");
}
