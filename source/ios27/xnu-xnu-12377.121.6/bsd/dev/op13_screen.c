/* AA569 screen brightness. Native DCS51 FIFO path, no framebuffer changes.
 * Requires the verified firmware configuration-AHB vote before XNU starts.
 * Root-only interface accepts a single uint32_t, 0..1000 UI brightness.
 * Zero is a visible minimum, not panel-off; never enter HBM (DBV >3515).
 */
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include "op13_dsi_brightness.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void loader_log_putc(char);
extern kern_return_t thread_terminate(thread_t);
static volatile uint32_t wanted, generation;
static volatile int failed;
static volatile uint32_t *registers;

static void screen_log(unsigned value, unsigned dbv, int result)
{
 char line[160];
 snprintf(line,sizeof(line),"op13-screen: UI=%u/1000 DBV=%u result=%d\n",value,dbv,result);
 for(const char *p=line;*p;p++)loader_log_putc(*p);
}
static uint32_t screen_read(void *context,unsigned offset)
{
 (void)context;
 uint32_t value=registers[offset/4];
 __asm__ volatile("dsb sy":::"memory");
 return value;
}
static void screen_write(void *context,unsigned offset,uint32_t value)
{
 (void)context;
 /* Keep the hardware write surface restricted to the verified FIFO path. */
 switch(offset){case 4:case 0x3c:case 0x4c:case 0x84:case 0x90:
 case 0x110:case 0x15c:case 0x17c:case 0x1ec:break;default:return;}
 registers[offset/4]=value;
 __asm__ volatile("dsb sy":::"memory");
}
static void screen_delay(void *context,unsigned us){(void)context;delay(us);}
static int screen_open(__unused dev_t dev,__unused int flags,__unused int type,__unused struct proc *p){return failed?EIO:0;}
static int screen_write_user(__unused dev_t dev,struct uio *uio,__unused int flags)
{
 uint32_t value;
 if(failed)return EIO;
 if(uio_resid(uio)!=sizeof(value))return EINVAL;
 int error=uiomove((caddr_t)&value,sizeof(value),uio);
 if(error)return error;
 if(value>1000)return EINVAL;
 __atomic_store_n(&wanted,value,__ATOMIC_RELAXED);
 __atomic_add_fetch(&generation,1,__ATOMIC_RELEASE);
 return 0;
}
static const struct cdevsw screen_cdevsw={
 .d_open=screen_open,.d_close=screen_open,.d_read=eno_rdwrt,.d_write=screen_write_user,
 .d_ioctl=eno_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_select=eno_select,
 .d_mmap=eno_mmap,.d_strategy=eno_strat,.d_type=0
};
static void screen_worker(void *unused,wait_result_t wait)
{
 (void)unused;(void)wait;
 registers=(volatile uint32_t *)ml_io_map(0x0ae94000,4096);
 if(!registers||screen_read(0,0)!=0x20090000){failed=1;screen_log(0,0,-10);goto done;}
 int major=cdevsw_add(-1,&screen_cdevsw);
 if(major<0||!devfs_make_node(makedev(major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0600,"op13-screen")){
  failed=1;screen_log(0,0,-11);goto done;
 }
 const char *marker="op13-screen: native DCS51 control v1 ready; root-only; normal-range\n";
 for(const char *p=marker;*p;p++)loader_log_putc(*p);
 struct op13_dsi_io io={0,screen_read,screen_write,screen_delay};
 uint32_t seen=0,last=~0u;
 unsigned busy=0;
 while(!failed){
  uint32_t gen=__atomic_load_n(&generation,__ATOMIC_ACQUIRE);
  uint32_t value=__atomic_load_n(&wanted,__ATOMIC_RELAXED);
  if(gen!=seen){
   if(value==last){seen=gen;continue;}
   unsigned dbv=(3515*value+500)/1000;if(dbv<8)dbv=8;
   int rc=op13_dsi_set(&io,dbv);
   if(rc==-1){if(++busy==100){screen_log(value,dbv,rc);busy=0;}}
   else{
    screen_log(value,dbv,rc);busy=0;seen=gen;
    if(rc)failed=1;else last=value;
   }
  }
  /* Sleep between updates; no renderer thread, GPU, or framebuffer work. */
  assert_wait_timeout((event_t)&generation,THREAD_UNINT,20,NSEC_PER_MSEC);
  thread_block(THREAD_CONTINUE_NULL);
 }
done:
 thread_terminate(current_thread());
}
void op13_screen_init(void)
{
 thread_t thread;
 if(kernel_thread_start(screen_worker,0,&thread)==KERN_SUCCESS)thread_deallocate(thread);
}
