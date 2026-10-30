/* SPDX-License-Identifier: BSD-2-Clause
 * Adopt an explicitly transferred, reserved MHI DMA lease. No PCIe reset,
 * SMMU remap or firmware bootstrap from XNU. This first probe only verifies
 * real QRTR/WLFW target capabilities; it does not attach a network interface. */
#include <mach/vm_param.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <sys/redwood_qrtr.h>
#include <sys/redwood_sta.h>
#include <os/atomic_private.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/random.h>
#include <vm/memory_types.h>
#include "op13_mhi_handoff.h"
#include "op13_wifi_qrtr.h"
#include "op13_peach_wire.h"
#include "op13_mhi_mmio.h"
#include "op13_peach_window.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned int);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_putc(char);
static void peach_log(const char *format,...) __printflike(1,2);
static void peach_log(const char *format,...)
{
 char line[400];va_list ap;va_start(ap,format);vsnprintf(line,sizeof line,format,ap);va_end(ap);
 for(const char*p=line;*p;p++)loader_log_putc(*p);
 printf("%s",line);
}
#define printf peach_log
static LCK_GRP_DECLARE(peach_mhi_group,"op13-peach-mhi");
static LCK_MTX_DECLARE(peach_mhi_lock,&peach_mhi_group);
static struct {
 struct mhi_host host;
 struct op13_mhi_handoff lease;
 vm_offset_t bar,pool;
 unsigned attached,fault,packets,malformed,server,request,reply,ack;
 uint8_t mac[6];unsigned mac_ready;
 uint32_t ack_node,ack_port;
} peach;
static LCK_MTX_DECLARE(peach_rx_lock,&peach_mhi_group);
static struct {
 unsigned in,out,active,overflow;
 struct {size_t size;uint8_t data[8224];} packet[16];
} peach_rx;
static void peach_enqueue(const void *data,size_t n)
{
 lck_mtx_lock(&peach_rx_lock);
 if(n>8224||peach_rx.in-peach_rx.out>=16)peach_rx.overflow++;
 else {unsigned at=peach_rx.in++%16;peach_rx.packet[at].size=n;memcpy(peach_rx.packet[at].data,data,n);}
 lck_mtx_unlock(&peach_rx_lock);
}
static uint32_t peach_read(void*c,uint32_t off)
{
 (void)c;if(!peach.bar||!op13_wifi_mhi_mmio(off,0)){peach.fault=1;return ~0u;}
 __asm__ volatile("dsb sy":::"memory");uint32_t v=*(volatile uint32_t*)(peach.bar+off);
 __asm__ volatile("dsb sy":::"memory");return v;
}
static void peach_write(void*c,uint32_t off,uint32_t value)
{
 (void)c;if(peach.fault||!peach.bar||!op13_wifi_mhi_mmio(off,1)){peach.fault=1;return;}
 *(volatile uint32_t*)(peach.bar+off)=value;__asm__ volatile("dsb sy":::"memory");
}
/* Fixed resident allocation: adopted MHI rings cannot allocate/free pages. */
static void *peach_alloc(void*c,size_t n,size_t a,uint64_t*pa)
{(void)c;(void)n;(void)a;(void)pa;return NULL;}
static void peach_free(void*c,void*p,size_t n){(void)c;(void)p;(void)n;}
static uint64_t peach_time(void*c)
{(void)c;uint64_t n;absolutetime_to_nanoseconds(mach_absolute_time(),&n);return n;}
static void peach_lock(void*c){(void)c;lck_mtx_lock(&peach_mhi_lock);}
static void peach_unlock(void*c){(void)c;lck_mtx_unlock(&peach_mhi_lock);}
static void peach_pause(void*c,unsigned us){(void)c;delay_for_interval(us,NSEC_PER_USEC);}
static void peach_random(void*c,void*p,size_t n){(void)c;read_random(p,n);}
static void peach_say(void*c,const char*s){(void)c;printf("op13-wifi-mhi: %s\n",s);}
static void peach_status(void*c,enum mhi_host_status what)
{(void)c;if(what!=MHI_HOST_MISSION)peach.fault=1;printf("op13-wifi-mhi: state event %u\n",what);}
static unsigned peach_le16(const uint8_t*p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static void peach_receive(void*c,unsigned channel,const void*data,size_t n)
{
 (void)c;if(channel!=21)return;
 if(os_atomic_load(&peach_rx.active,acquire)){peach_enqueue(data,n);return;}
 struct op13_qrtr_packet p;
 if(op13_qrtr_decode(data,n,&p)){peach.malformed++;return;}
 peach.packets++;
 printf("op13-wifi: Darwin QRTR type=%u src=%u:%x dst=%u:%x bytes=%u confirm=%u\n",p.type,p.src_node,p.src_port,p.dst_node,p.dst_port,p.size,p.confirm);
 if(p.confirm){peach.ack=1;peach.ack_node=p.src_node;peach.ack_port=p.src_port;}
 if((p.type==2||p.type==4)&&p.size>=20&&op13_qrtr_u32(p.payload)==4&&
    op13_qrtr_u32(p.payload+4)==0x45&&op13_qrtr_u32(p.payload+8)==0x101&&
    op13_qrtr_u32(p.payload+12)==7&&op13_qrtr_u32(p.payload+16)==1)peach.server=1;
 if(p.type==1&&p.src_node==7&&p.src_port==1&&p.size>=7&&p.payload[0]==2&&
    peach_le16(p.payload+1)==0x5a01&&peach_le16(p.payload+3)==0x24){
  size_t bytes=peach_le16(p.payload+5),at=7;unsigned found=0,success=0;
  if(bytes!=p.size-7){peach.malformed++;return;}
  while(at<p.size){
   if(p.size-at<3){peach.malformed++;return;}
   unsigned tag=p.payload[at],len=peach_le16(p.payload+at+1);at+=3;
   if(len>p.size-at){peach.malformed++;return;}
   if(tag==2){
    if(len!=4||found++){peach.malformed++;return;}
    success=!peach_le16(p.payload+at)&&!peach_le16(p.payload+at+2);
   }
   at+=len;
  }
  if(found==1&&success)peach.reply=1;
 }
}
static const struct mhi_host_ops peach_ops={peach_read,peach_write,peach_alloc,peach_free,peach_time,
 peach_lock,peach_unlock,peach_pause,peach_random,peach_say,peach_status,peach_receive};
#include "op13_peach_qrtr.inc"
static void peach_drain(void)
{
 /* Only this poll thread dispatches callbacks. The slot remains occupied
  * through dispatch, so incoming completions cannot overwrite its bytes. */
 for(unsigned count=0;count<16;count++){
  lck_mtx_lock(&peach_rx_lock);unsigned at=peach_rx.out%16;
  if(peach_rx.in==peach_rx.out){lck_mtx_unlock(&peach_rx_lock);break;}
  size_t n=peach_rx.packet[at].size;lck_mtx_unlock(&peach_rx_lock);
  lck_mtx_lock(&qrtr_calling);qrtr_input(peach_rx.packet[at].data,(uint32_t)n);lck_mtx_unlock(&qrtr_calling);
  lck_mtx_lock(&peach_rx_lock);peach_rx.out++;lck_mtx_unlock(&peach_rx_lock);
 }
 lck_mtx_lock(&qrtr_calling);qrtr_fresh();lck_mtx_unlock(&qrtr_calling);
}
static void peach_driver(void *unused,wait_result_t result)
{
 (void)unused;(void)result;extern int redwood_lx_start(void);
 int rc=redwood_lx_start();printf("op13-wifi: full Peach ath12k start result=%d; verify scan and association separately\n",rc);
 if(!rc){extern void op13_peach_join_given(void);op13_peach_join_given();}
 if(!rc){
  /* Exercise the same serialized station/cache path as Settings, rather
   * than scanning privately inside the Linux compatibility layer. */
  struct redwood_sta_scan scan={0};scan.passive=1;
  int asked=redwood_sta_ask_scan(&scan);
  printf("op13-wifi: initial passive station scan queued=%d\n",asked);
 }
 thread_terminate(current_thread());
}
static int peach_send(unsigned type,uint32_t node,uint32_t port,const uint8_t*p,size_t n)
{
 if(n>64)return EINVAL;
 uint8_t packet[96];size_t bytes=op13_peach_qrtr_encode(packet,sizeof packet,type,0x4000,node,port,p,n);
 if(!bytes)return EINVAL;
 return mhi_host_send(&peach.host,20,packet,bytes);
}
static void peach_worker(void*p,wait_result_t result)
{
 (void)p;(void)result;
 uint8_t hello[52];op13_qrtr_hello(hello);
 int rc=mhi_host_send(&peach.host,20,hello,sizeof hello);
 printf("op13-wifi: Darwin MHI HELLO result=%d\n",rc);
 uint64_t until=peach_time(0)+UINT64_C(15000000000);
 while(!rc&&!peach.fault&&peach_time(0)<until){
  mhi_host_interrupt(&peach.host,-1);
  if(peach.ack){
   uint8_t ack[20]={0};peach.ack=0;op13_qrtr_put(ack,7);op13_qrtr_put(ack+4,1);op13_qrtr_put(ack+8,0x4000);
   rc=peach_send(7,peach.ack_node,peach.ack_port,ack,sizeof ack);
  }
  if(!rc&&!peach.request){
   const uint8_t cap[7]={0,1,0x5a,0x24,0,0,0};peach.request=1;
   rc=peach_send(1,peach.lease.wlfw_node,peach.lease.wlfw_port,cap,sizeof cap);
  }
  if(peach.reply)break;
  uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
  assert_wait_deadline((event_t)&peach,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
 }
 printf("op13-wifi: Darwin MHI probe rc=%d fault=%u EE=%u state=%u QRTR=%u malformed=%u service=%u target-cap=%u; network not attached\n",
  rc,peach.fault,peach.host.ee,peach.host.state,peach.packets,peach.malformed,peach.server,peach.reply);
 if(peach.reply&&peach.lease.pool_bytes==0x8000000){
  /* Cache only the exact service whose announcement the raw probe saw. */
  if(!peach.server){printf("op13-wifi: no verified WLFW service; full driver refused\n");thread_terminate(current_thread());return;}
  qrtr_peer=7;qrtr_servers[0].there=true;qrtr_servers[0].service=0x45;
  qrtr_servers[0].instance=0x101;qrtr_servers[0].node=7;qrtr_servers[0].port=1;
  link_thread_is=current_thread();os_atomic_store(&peach_rx.active,1,release);redwood_qmi_set_transport(&qrtr_ops);
  thread_t driver;int started=kernel_thread_start(peach_driver,0,&driver);
  if(started==KERN_SUCCESS)thread_deallocate(driver);
  else {printf("op13-wifi: no ath12k worker=%d\n",started);thread_terminate(current_thread());return;}
  printf("op13-wifi: native QRTR provider active over adopted PCIe MHI\n");
  uint64_t health=peach_time(0);
  while(!peach.fault&&!peach.host.failed){
   mhi_host_interrupt(&peach.host,-1);peach_drain();
   if(peach_time(0)-health>UINT64_C(10000000000)){
    health=peach_time(0);
    printf("op13-wifi: live MHI status=%x EE=%x cached-state=%u wakes=%u packets=%u queued-drops=%u\n",
     peach_read(0,0x48),peach_read(0,0x128),peach.host.state,peach.host.wakes,peach.packets,peach_rx.overflow);
   }
   uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
   assert_wait_deadline((event_t)&peach_rx,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
  }
  printf("op13-wifi: MHI fault; polling stopped, DMA retained until cold reset\n");
 }
 thread_terminate(current_thread());
}
/* Returns true only if a Peach lease was present, so the legacy WPSS path
 * cannot start on the same adapter, including when adoption is refused. */
int op13_peach_transport_init(void)
{
 uint64_t pa=0;
 if(!PE_parse_boot_argn("touch_lease_pa",&pa,sizeof pa)||pa<0x80000000||pa>=UINT64_C(0x1000000000)||(pa&0x3fff))return 0;
 vm_offset_t page=ml_io_map(pa,0x4000);if(!page)return 0;
 const struct op13_mhi_handoff *s=(const void*)(page+256);
 if(s->magic!=OP13_MHI_HANDOFF_MAGIC)return 0;
 if(peach.attached)return 1;
 peach.lease=*s;
 if(s->flags!=OP13_MHI_TRANSFER||s->version!=1||s->bytes!=sizeof(*s)||(s->pool_bytes!=0x4000000&&s->pool_bytes!=0x8000000)||
    s->pool_pa<0x80000000||s->pool_pa>=(UINT64_C(1)<<36)||s->pool_pa>s->pool_pa+s->pool_bytes||
    s->bar0!=0x40400000||s->bar0_bytes!=0x200000){printf("op13-wifi: transfer lease header refused; no MMIO\n");return 1;}
 peach.pool=ml_io_map_unmappable(s->pool_pa,s->pool_bytes,VM_WIMG_WCOMB);
 if(!peach.pool){printf("op13-wifi: reserved DMA mapping failed; no MMIO\n");return 1;}
 int rc=op13_mhi_adopt(&peach.host,&peach.lease,(void*)peach.pool,&peach_ops,0,0);
 if(rc){printf("op13-wifi: MHI transfer lease refused=%d; no MMIO\n",rc);return 1;}
 peach.bar=ml_io_map_unmappable(s->bar0,s->bar0_bytes,VM_WIMG_IO);
 if(!peach.bar){printf("op13-wifi: BAR mapping failed\n");return 1;}
 peach.attached=1;
 read_random(peach.mac,sizeof peach.mac);peach.mac[0]=(peach.mac[0]&0xfc)|2;peach.mac_ready=1;
 extern void redwood_lx_net_early(void);redwood_lx_net_early();
 thread_t worker;rc=kernel_thread_start(peach_worker,0,&worker);
 if(rc!=KERN_SUCCESS){printf("op13-wifi: no MHI worker=%d\n",rc);return 1;}
 thread_deallocate(worker);printf("op13-wifi: reserved Peach MHI state adopted; diagnostic QRTR probe only\n");return 1;
}
/* Imported ath12k's optional platform-calibration query: no verified
 * calibration is claimed for Peach yet. */
int lx_os_radio_calibrated(void){return 0;}

int lx_os_peach_ready(void)
{return peach.attached&&peach.reply&&peach.lease.pool_bytes==0x8000000&&!peach.fault&&!peach.host.failed;}
static LCK_MTX_DECLARE(peach_bus_lock,&peach_mhi_group);
static uint32_t peach_bar_read(unsigned off)
{__asm__ volatile("dsb sy":::"memory");uint32_t v=*(volatile uint32_t*)(peach.bar+off);__asm__ volatile("dsb sy":::"memory");return v;}
static void peach_bar_write(unsigned off,uint32_t v)
{*(volatile uint32_t*)(peach.bar+off)=v;__asm__ volatile("dsb sy":::"memory");}
#include "op13_peach_wake.h"
static uint32_t peach_wake_read(void *ctx,unsigned off){(void)ctx;return peach_bar_read(off);}
static void peach_wake_write(void *ctx,unsigned off,uint32_t value){(void)ctx;peach_bar_write(off,value);}
static void peach_wake_pause(void *ctx,unsigned ms)
{
 (void)ctx;uint64_t deadline;clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
 assert_wait_deadline((event_t)&peach.bar,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
}
int lx_os_peach_umac_wake(void)
{
 if(!lx_os_peach_ready()||!peach.host.wakes)return -1;
 static const struct op13_peach_wake_ops ops={peach_wake_read,peach_wake_write,peach_wake_pause};
 struct op13_peach_wake_result result;
 lck_mtx_lock(&peach_bus_lock);
 int rc=op13_peach_wake_umac(&ops,NULL,&result);
 lck_mtx_unlock(&peach_bus_lock);
 extern void redwood_wifi_tell(const char *,...);
 redwood_wifi_tell("op13-wifi: native UMAC wake rc=%d scratch=%x -> %x polls=%u\n",rc,result.before,result.after,result.polls);
 return rc;
}
void lx_os_peach_umac_sleep(void)
{
 if(!lx_os_peach_ready())return;
 lck_mtx_lock(&peach_bus_lock);peach_bar_write(0x3004,0);lck_mtx_unlock(&peach_bus_lock);
}
static unsigned peach_bus_access(unsigned off,unsigned value,int write)
{
 if(!lx_os_peach_ready())return ~0u;
 lck_mtx_lock(&peach_bus_lock);
 struct op13_peach_window p;unsigned saved=0,got=~0u;int changed=0;
 if(off>=0x80000)saved=peach_bar_read(0x3278);
 if(op13_peach_window_plan(off,saved,&p)){peach.fault=1;goto done;}
 if(p.remap){changed=1;peach_bar_write(0x3278,p.selected);if(peach_bar_read(0x3278)!=p.selected){peach.fault=1;goto restore;}}
 if(write){peach_bar_write(p.address,value);got=0;}else got=peach_bar_read(p.address);
restore:
 if(changed){peach_bar_write(0x3278,saved);if(peach_bar_read(0x3278)!=saved)peach.fault=1;}
done:
 lck_mtx_unlock(&peach_bus_lock);
 if(peach.fault)printf("op13-wifi: Peach HAL access refused offset=%x write=%d; cold reset required\n",off,write);
 return got;
}
unsigned lx_os_peach_read32(unsigned off){return peach_bus_access(off,0,0);}
void lx_os_peach_write32(unsigned off,unsigned v){(void)peach_bus_access(off,v,1);}
void *op13_peach_radio_memory(unsigned long size,unsigned long long *address)
{
 /* Firmware/MHI own the first64MiB. The imported LinuxKPI pool gets44MiB
  * immediately after that, inside the same private SID1401 mapping. */
 static unsigned allocated;void *memory=NULL;
 if(!lx_os_peach_ready()||size!=44ul*1024*1024||!address)return NULL;
 lck_mtx_lock(&peach_bus_lock);
 if(!allocated){allocated=1;memory=(void*)(peach.pool+0x4000000);*address=peach.lease.iova+0x4000000;memset(memory,0,size);}
 lck_mtx_unlock(&peach_bus_lock);return memory;
}
unsigned op13_peach_option(const char *name)
{
 if(!strcmp(name,"wifi_lx"))return 2;
 if(!strcmp(name,"wifi_station")||!strcmp(name,"wifi_fw_mac")||!strcmp(name,"wifi_quiet")||!strcmp(name,"wifi_passive"))return 1;
 return 0;
}

int op13_peach_radio_address(uint8_t address[6])
{if(!peach.attached||!peach.mac_ready)return 0;memcpy(address,peach.mac,6);return 1;}
