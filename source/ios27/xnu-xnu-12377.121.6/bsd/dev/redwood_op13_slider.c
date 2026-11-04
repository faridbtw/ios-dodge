/* SPDX-License-Identifier: BSD-2-Clause
 * Dodge AK09970 on GPIO80/81. Sensor protocol from OnePlus's vendor driver;
 * event path from haxi0's instantnoodle slider. Supply is voted by RAM UEFI.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/op13_slider_i2c.h>
#include <sys/op13_slider_position.h>
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_putc(char);
static void slider_log(const char *format, ...) __printflike(1,2);
static void slider_log(const char *format, ...)
{
 char line[200];va_list ap;va_start(ap,format);vsnprintf(line,sizeof(line),format,ap);va_end(ap);
 for(const char *c=line;*c;c++)loader_log_putc(*c);
 printf("%s",line);
}
static vm_offset_t pins;
static uint32_t position;
static int calibration[9];
SYSCTL_DECL(_kern_redwood);
SYSCTL_UINT(_kern_redwood,OID_AUTO,slider,CTLFLAG_RD|CTLFLAG_LOCKED,&position,0,"alert slider: 0 unknown,1 bottom/ring,2 middle/silent,3 top/DND");
static uint32_t read_pin(void *cookie,unsigned pin,unsigned offset)
{(void)cookie;return *(volatile uint32_t *)(pins+pin*0x1000+offset);}
static void write_pin(void *cookie,unsigned pin,unsigned offset,uint32_t value)
{(void)cookie;*(volatile uint32_t *)(pins+pin*0x1000+offset)=value;__asm__ volatile("dsb osh":::"memory");}
static void delay_pin(void *cookie,unsigned us){(void)cookie;delay(us);}
static void sleep_ms(unsigned ms)
{
 static int event;uint64_t deadline;
 clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
 assert_wait_deadline((event_t)&event,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
}
static void slider_worker(void *argument,wait_result_t result)
{
 (void)argument;(void)result;
 pins=ml_io_map(0x0f150000,0x4000);
 if(!pins)goto done;
 struct op13_slider_bus bus={NULL,read_pin,write_pin,delay_pin,{0},{0},0};
 uint8_t id[2]={0},mode=0x2a;
 int error=op13_slider_transfer(&bus,0,id,2,0);
 if(error||id[0]!=0x48||id[1]!=0xc1) {
  slider_log("op13-slider: sensor ID failed error=%d id=%02x%02x; stopped\n",error,id[0],id[1]);goto done;
 }
 if(op13_slider_transfer(&bus,0x21,&mode,1,1))goto done;
 slider_log("op13-slider: AK09970 ready; 100ms sleeping poll, two-sample debounce\n");
 unsigned candidate=0,failures=0,samples=0;
 for(;;) {
  sleep_ms(100);
  uint8_t b[7]={0};error=op13_slider_transfer(&bus,0x17,b,7,0);
  if(error) {
   candidate=0;
   if(++failures>=5){slider_log("op13-slider: bus stopped error=%d\n",error);break;}
   continue;
  }
  failures=0;
  if(!(b[0]&1)){candidate=0;continue;}
  int x=(int16_t)((b[5]<<8)|b[6]),y=(int16_t)((b[3]<<8)|b[4]),z=(int16_t)((b[1]<<8)|b[2]);
  unsigned now=op13_slider_position(calibration,x,y,z);
  if(samples++<3)slider_log("op13-slider: sample x=%d y=%d z=%d position=%u\n",x,y,z,now);
  if(now&&now==candidate&&now!=position) {
   position=now;
   slider_log("op13-slider: position=%u x=%d y=%d z=%d\n",now,x,y,z);
   redwood_touch_post(REDWOOD_SLIDER,now,0,0);
  }
  candidate=now;
 }
done:
 thread_terminate(current_thread());
}
void redwood_op13_slider_init(void)
{
 int enabled=0,powered=0;char text[128]={0};thread_t thread;
 PE_parse_boot_argn("slider",&enabled,sizeof(enabled));
 PE_parse_boot_argn("slider_power",&powered,sizeof(powered));
 if(enabled!=1||powered!=1)return;
 if(!PE_parse_boot_arg_str("slider_cal",text,sizeof(text))||
  sscanf(text,"%d,%d,%d,%d,%d,%d,%d,%d,%d",&calibration[0],&calibration[1],&calibration[2],
   &calibration[3],&calibration[4],&calibration[5],&calibration[6],&calibration[7],&calibration[8])!=9||
  !op13_slider_calibration_valid(calibration)) {
  slider_log("op13-slider: missing/invalid factory calibration; no GPIO access\n");return;
 }
 if(kernel_thread_start(slider_worker,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
}
