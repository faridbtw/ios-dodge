/* SPDX-License-Identifier: BSD-2-Clause
 * Single-threaded UEFI adapter for the imported MHI host. All DMA allocations
 * are restricted to our mapped NC pool; MSI vectors target its reserved first page; last page holds dump vectors.
 * Peach TME BHIE skips the512KiB BHI prefix, matching actual vendor boot.c.
 */
#include "mhi_host.h"
#include "op13_wifi_mhi_caps.h"
#include "op13_wifi_qmi_init.h"
#include "op13_wifi_files.h"
#include "op13_wifi_pmu.h"
static struct op13_pmu op13_wifi_pmu;
static struct op13_pmu_plan op13_wifi_pmu_plan;
static int op13_wifi_pmu_received,op13_wifi_pmu_valid;
static struct mhi_host op13_wifi_mhi_host;
static unsigned op13_wifi_qrtr_received,op13_wifi_qrtr_bad,op13_wifi_qrtr_services;
static uint32_t op13_wifi_wlfw_node,op13_wifi_wlfw_port;
static int op13_wifi_mhi_stopped;
static int op13_wifi_qmi_waiting,op13_wifi_files_asked,op13_wifi_fw_init_done;
static int op13_wifi_cap_asked,op13_wifi_cap_received;
static unsigned op13_wifi_qmi_expected_txn,op13_wifi_qmi_expected_id,op13_wifi_qmi_response;
static uint32_t op13_wifi_qmi_result_code,op13_wifi_qmi_error_code;
static struct op13_qmi_memory op13_wifi_mem;
static unsigned op13_wifi_mem_received,op13_wifi_init_asked,op13_wifi_mem_ready,op13_wifi_memory_asked;
static struct op13_qmi_cap op13_wifi_cap;
static int op13_wifi_qrtr_ack_pending;
static uint32_t op13_wifi_qrtr_ack_node,op13_wifi_qrtr_ack_port;
static uint64_t op13_wifi_mhi_used=4096; /* Match Android: first IOVA page reserved for MSI. */
/* Independent RX area must never overlap live TX image/rings. */
#define OP13_WIFI_HOST_BYTES 0x1000000ULL
#define OP13_WIFI_RDDM_OFFSET OP13_WIFI_HOST_BYTES
#define OP13_WIFI_RDDM_BYTES 0x780000ULL
_Static_assert(OP13_WIFI_RDDM_OFFSET+OP13_WIFI_RDDM_BYTES<=OP13_WIFI_DMA_BYTES-4096,"RDDM exceeds owned pool");
static int op13_wifi_mhi_adapter_fault,op13_wifi_rddm_ready,op13_wifi_rddm_armed;
static int op13_wifi_mhi_mmio(unsigned o,int write)
{
 if(o&3)return 0;
 if(o==0x10||o==0x38||o==0x70||o==0x74)return 1;
 if(!write&&(o==0x24||o==0x18||o==0x20||o==0x28||o==0x2c||o==0x48||o==0x128||o==0x130))return 1;
 if((o>=0x58&&o<=0x6c)||(o>=0x80&&o<=0x8c)||(o>=0x98&&o<=0xa4))return 1;
 if(o==0x108||o==0x10c||o==0x110||o==0x118||o==0x120||o==0x12c)return 1;
 if(o==0x250||o==0x254||o==0x258||o==0x260||o==0x268)return 1;
 if(o>=0x284&&o<0x29c)return 1; /* crash-dump RX vector setup */
 if(o==0x6f0||o==0x3a0||o==0x3a4||o==0x3a8||o==0x3ac||o==0x6f8||o==0x6fc)return 1;
 if(!write&&o==0x29c)return 1;
 return o>=0x700&&o<=0x71c;
}
static uint32_t op13_wifi_mhi_op_read(void *c,uint32_t o)
{
 (void)c;
 if(!op13_wifi_mhi_mmio(o,0)){op13_wifi_mhi_adapter_fault=1;print("op13-wifi-mhi: rejected read offset=%x\n",(uint64_t)o);return ~0u;}
 return *(volatile uint32_t *)(uintptr_t)(0x40400000+o);
}
static void op13_wifi_mhi_op_write(void *c,uint32_t o,uint32_t v)
{
 (void)c;
 if(op13_wifi_mhi_adapter_fault||!op13_wifi_mhi_mmio(o,1)){op13_wifi_mhi_adapter_fault=1;print("op13-wifi-mhi: rejected write offset=%x\n",(uint64_t)o);return;}
 *(volatile uint32_t *)(uintptr_t)(0x40400000+o)=v;__asm__ volatile("dsb sy":::"memory");
}
static void *op13_wifi_mhi_op_alloc(void *c,size_t size,size_t align,uint64_t *address)
{
 (void)c;
 if(!size||size>OP13_WIFI_DMA_BYTES||!align||(align&(align-1)))return 0;
 /* PCI coherent allocation guarantee: align to power-of-two >=size. */
 size_t natural=1;while(natural<size)natural<<=1;if(align<natural)align=natural;
 if(align>OP13_WIFI_DMA_BYTES)return 0;
 uint64_t at=(op13_wifi_mhi_used+align-1)&~(uint64_t)(align-1);
 if(at>OP13_WIFI_HOST_BYTES-4096||size>OP13_WIFI_HOST_BYTES-4096-at)return 0;
 *address=OP13_WIFI_IOVA+at;op13_wifi_mhi_used=at+size;
 void *memory=(void *)(uintptr_t)(op13_wifi_dma_pool+at);memset(memory,0,size);
 return memory;
}
static void op13_wifi_mhi_op_free(void *c,void *m,size_t n){(void)c;(void)m;(void)n;/* retained until reset */}
static uint64_t op13_wifi_mhi_op_time(void *c)
{(void)c;uint64_t f=READ_SYSREG(CNTFRQ_EL0),t=READ_SYSREG(CNTVCT_EL0);return t/f*1000000000ULL+(t%f)*1000000000ULL/f;}
static void op13_wifi_mhi_op_lock(void *c){(void)c;/* one synchronous caller, no IRQ handlers */}
static void op13_wifi_mhi_op_pause(void *c,unsigned us){(void)c;BS->stall(us);}
static void op13_wifi_mhi_op_random(void *c,void *bytes,size_t n)
{
 (void)c;static uint32_t sequence=1;uint32_t value=(uint32_t)READ_SYSREG(CNTVCT_EL0)^++sequence;
 uint8_t *p=bytes;for(size_t i=0;i<n;i++)p[i]=(uint8_t)(value>>((i&3)*8));
}
static void op13_wifi_mhi_op_say(void *c,const char *line){(void)c;print("op13-wifi-mhi: %s\n",line);}
static void op13_wifi_mhi_op_status(void *c,enum mhi_host_status what){(void)c;print("op13-wifi-mhi: status event=%d\n",(uint64_t)what);}
static void op13_wifi_mhi_op_receive(void *c,unsigned channel,const void *data,size_t size)
{
 (void)c;
 struct op13_qrtr_packet p;
 if(channel!=21||op13_qrtr_decode(data,size,&p)){op13_wifi_qrtr_bad++;return;}
 op13_wifi_qrtr_received++;
 print("op13-wifi-qrtr: type=%d src=%d:%x dst=%d:%x size=%d confirm=%d\n",
  (uint64_t)p.type,(uint64_t)p.src_node,(uint64_t)p.src_port,(uint64_t)p.dst_node,(uint64_t)p.dst_port,(uint64_t)p.size,(uint64_t)p.confirm);
 if(p.type==1&&p.dst_node==1&&p.dst_port==0x4000&&p.src_node==op13_wifi_wlfw_node&&p.src_port==op13_wifi_wlfw_port){
  struct op13_qmi_message message;
  if(!op13_qmi_message_decode(p.payload,p.size,&message)){
   /* Validated TLVs only. No serials, strings, firmware bytes or MACs logged. */
   if(message.flags==2&&(message.id==0x24||message.id==0x25||message.id==0x20)){
    for(size_t at=0;at<message.size;){
     unsigned tag=message.tlvs[at];size_t bytes=op13_qmi_u16(message.tlvs+at+1);at+=3;
     const uint8_t *v=message.tlvs+at;
     if(message.id==0x24&&message.transaction==1&&tag==0x22){
      int rc=op13_wifi_pmu_received?-1:op13_pmu_decode(v,bytes,&op13_wifi_pmu);
      op13_wifi_pmu_received=1;op13_wifi_pmu_valid=0;
      if(!rc){
       for(unsigned i=0;i<op13_wifi_pmu.count;i++)print("op13-wifi-pmu: pin=%s wake-valid=%d mV=%d sleep-valid=%d mV=%d\n",
        op13_pmu_pins[op13_wifi_pmu.item[i].pin],(uint64_t)op13_wifi_pmu.item[i].wake_valid,(uint64_t)op13_wifi_pmu.item[i].wake,
        (uint64_t)op13_wifi_pmu.item[i].sleep_valid,(uint64_t)op13_wifi_pmu.item[i].sleep);
       rc=op13_pmu_plan(&op13_wifi_pmu,&op13_wifi_pmu_plan);
       if(!rc)op13_wifi_pmu_valid=1;
      }
      print("op13-wifi-pmu: native plan result=%d; no power settings sent yet\n",(uint64_t)(int64_t)rc);
     }
     if(message.id==0x24)print("op13-wifi-qmi: CAP TLV=%x bytes=%d\n",(uint64_t)tag,(uint64_t)bytes);
     if(bytes==8&&((message.id==0x24&&tag==0x1a)||((message.id==0x25||message.id==0x20)&&tag==0x10)))
      print("op13-wifi-qmi: metadata id=%x TLV=%x value=%x\n",(uint64_t)message.id,(uint64_t)tag,
       (uint64_t)op13_qrtr_u32(v)|((uint64_t)op13_qrtr_u32(v+4)<<32));
     if(message.id==0x24&&tag==0x20&&bytes==4)
      print("op13-wifi-qmi: BDF download method=%d\n",(uint64_t)op13_qrtr_u32(v));
     at+=bytes;
    }
   }
   if(message.flags==2&&message.transaction==op13_wifi_qmi_expected_txn&&message.id==op13_wifi_qmi_expected_id&&
      !op13_qmi_result(&message,&op13_wifi_qmi_result_code,&op13_wifi_qmi_error_code)){
    op13_wifi_qmi_response=1;
    print("op13-wifi-qmi: response txn=%d id=%x result=%d error=%d\n",(uint64_t)message.transaction,(uint64_t)message.id,(uint64_t)op13_wifi_qmi_result_code,(uint64_t)op13_wifi_qmi_error_code);
   }
   if(message.flags==4){
    if(message.id==0x37)op13_wifi_mem_ready=1;
    if(message.id==0x38)op13_wifi_fw_init_done=1;
    print("op13-wifi-qmi: indication id=%x bytes=%d\n",(uint64_t)message.id,(uint64_t)message.size);
    if(message.id==0x35&&!op13_qmi_mem_decode(&message,&op13_wifi_mem)){
     op13_wifi_mem_received=1;
     print("op13-wifi-qmi: requested memory segments=%d; no addresses supplied yet\n",(uint64_t)op13_wifi_mem.count);
     for(unsigned i=0;i<op13_wifi_mem.count;i++){
      print("op13-wifi-qmi: mem segment=%d type=%d bytes=%x configs=%d\n",(uint64_t)i,(uint64_t)op13_wifi_mem.seg[i].type,(uint64_t)op13_wifi_mem.seg[i].size,(uint64_t)op13_wifi_mem.seg[i].cfg_count);
      for(unsigned j=0;j<op13_wifi_mem.seg[i].cfg_count;j++)print("op13-wifi-qmi: mem cfg offset=%x bytes=%x secure=%d\n",op13_wifi_mem.seg[i].cfg[j].offset,(uint64_t)op13_wifi_mem.seg[i].cfg[j].size,(uint64_t)op13_wifi_mem.seg[i].cfg[j].secure);
     }
    }
   }
  }
  if(op13_wifi_cap_asked&&!op13_qmi_cap_decode(p.payload,p.size,&op13_wifi_cap)&&op13_wifi_cap.transaction==1){
   op13_wifi_cap_received=1;
   print("op13-wifi-qmi: CAP response result=%d error=%d chip-valid=%d chip=%x family=%x board-valid=%d board=%x soc-valid=%d soc=%x\n",
    (uint64_t)op13_wifi_cap.result,(uint64_t)op13_wifi_cap.error,(uint64_t)op13_wifi_cap.has_chip,(uint64_t)op13_wifi_cap.chip,
    (uint64_t)op13_wifi_cap.family,(uint64_t)op13_wifi_cap.has_board,(uint64_t)op13_wifi_cap.board,(uint64_t)op13_wifi_cap.has_soc,(uint64_t)op13_wifi_cap.soc);
  }
  if(p.confirm){op13_wifi_qrtr_ack_node=p.src_node;op13_wifi_qrtr_ack_port=p.src_port;op13_wifi_qrtr_ack_pending=1;}
 }

 if(p.type==4&&p.size>=20&&op13_qrtr_u32(p.payload)==4){
  uint32_t svc=op13_qrtr_u32(p.payload+4),inst=op13_qrtr_u32(p.payload+8);
  uint32_t node=op13_qrtr_u32(p.payload+12),port=op13_qrtr_u32(p.payload+16);
  op13_wifi_qrtr_services++;
  print("op13-wifi-qrtr: service=%x instance=%x node=%d port=%x\n",(uint64_t)svc,(uint64_t)inst,(uint64_t)node,(uint64_t)port);
  if(svc==0x45){op13_wifi_wlfw_node=node;op13_wifi_wlfw_port=port;}
 }
}
static uint32_t op13_wifi_caps_read(void *c,uint32_t o)
{
 (void)c;
 if(o!=0x24&&!op13_wifi_mhi_cap_offset(o)&&!(o>=4&&op13_wifi_mhi_cap_offset(o-4))){
  op13_wifi_mhi_adapter_fault=1;return ~0u;
 }
 return *(volatile uint32_t *)(uintptr_t)(0x40400000+o);
}
static void op13_wifi_caps_write(void *c,uint32_t o,uint32_t v)
{
 (void)c;
 if(op13_wifi_mhi_adapter_fault||o<4||!op13_wifi_mhi_cap_offset(o-4)||(v!=0xfd100000&&v!=0x8000007c)){
  op13_wifi_mhi_adapter_fault=1;return;
 }
 print("op13-wifi-mhi: capability config offset=%x before=%x value=%x\n",(uint64_t)o,(uint64_t)op13_wifi_caps_read(0,o),(uint64_t)v);
 *(volatile uint32_t *)(uintptr_t)(0x40400000+o)=v;__asm__ volatile("dsb sy":::"memory");
 print("op13-wifi-mhi: capability config after=%x\n",(uint64_t)op13_wifi_caps_read(0,o));
}
int op13_mhi_uefi_caps(struct mhi_host *h)
{
 (void)h;
 print("op13-wifi-mhi: misc capability head=%x\n",(uint64_t)op13_wifi_caps_read(0,0x24));
 struct op13_wifi_mhi_caps_io io={0,op13_wifi_caps_read,op13_wifi_caps_write};uint32_t bw=0,notify=0;
 int rc=op13_wifi_mhi_caps_setup(&io,&bw,&notify);
 print("op13-wifi-mhi: misc capability setup=%d BW=%x notify=%x\n",(uint64_t)(int64_t)rc,(uint64_t)bw,(uint64_t)notify);
 return op13_wifi_mhi_adapter_fault?-1:rc;
}
static int op13_wifi_rddm_stage(void)
{
 if(!op13_wifi_rddm_ready)return -1;
 staged_data=(const char *)(uintptr_t)(op13_wifi_dma_pool+OP13_WIFI_RDDM_OFFSET);staged_len=OP13_WIFI_RDDM_BYTES;
 print("op13-wifi-mhi: private RDDM buffer staged, do not upload publicly\n");return 0;
}
int op13_mhi_rddm_prepare(struct mhi_host *h)
{
 if(!op13_wifi_dma_mapped||op13_wifi_mhi_adapter_fault||op13_wifi_rddm_armed||h->bhie+MHI_BHIE_RXVECADDR_LOW!=0x284)return -1;
 memset((void *)(uintptr_t)(op13_wifi_dma_pool+OP13_WIFI_RDDM_OFFSET),0,OP13_WIFI_RDDM_BYTES);
 uint64_t vector_address=OP13_WIFI_IOVA+OP13_WIFI_DMA_BYTES-4096+0x100;
 uint64_t *vectors=(uint64_t *)(uintptr_t)(op13_wifi_dma_pool+OP13_WIFI_DMA_BYTES-4096+0x100);
 for(unsigned i=0;i<15;i++){
  vectors[2*i]=OP13_WIFI_IOVA+OP13_WIFI_RDDM_OFFSET+i*0x80000ULL;
  vectors[2*i+1]=0x80000;
 }
 __asm__ volatile("dsb sy":::"memory");
 op13_wifi_mhi_op_write(0,0x288,(uint32_t)(vector_address>>32));
 op13_wifi_mhi_op_write(0,0x284,(uint32_t)vector_address);
 op13_wifi_mhi_op_write(0,0x28c,15*16);
 op13_wifi_mhi_op_write(0,0x294,1);
 op13_wifi_rddm_armed=!op13_wifi_mhi_adapter_fault;
 print("op13-wifi-mhi: RDDM prearmed=%d independent RX=%x bytes=%x before BHI\n",
       (uint64_t)op13_wifi_rddm_armed,OP13_WIFI_IOVA+OP13_WIFI_RDDM_OFFSET,OP13_WIFI_RDDM_BYTES);
 return op13_wifi_rddm_armed?0:-1;
}
static void op13_wifi_rddm_capture(void)
{
 /* Already armed before BHI: do not overwrite a completed dump or TX image. */
 if(op13_wifi_mhi_host.ee!=MHI_HOST_EE_RDDM||op13_wifi_mhi_adapter_fault||!op13_wifi_rddm_armed)return;
 fb_reply("INFO","Collecting prearmed firmware crash into owned RAM; no partition writes");
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return;
 uint32_t status=0;
 for(unsigned i=0;i<5000;i++){
  status=op13_wifi_mhi_op_read(0,0x29c);
  if(status!=~0u&&((status>>30)==2||(status>>30)==3))break;
  BS->stall(1000);
 }
 __asm__ volatile("dsb sy":::"memory");
 op13_wifi_rddm_ready=status!=~0u&&(status>>30)==2;
 print("op13-wifi-mhi: RDDM status=%x ready=%d bytes=780000\n",(uint64_t)status,(uint64_t)op13_wifi_rddm_ready);
 BS->set_watchdog_timer(0,0,0,0);
}
static void op13_wifi_mhi_stop_failed(void)
{
 if(op13_wifi_mhi_stopped)return;
 op13_wifi_rddm_capture();
 op13_wifi_link_update(0xf166004,2,0);BS->stall(1000);op13_wifi_link_write(0x40000004,2);
 op13_wifi_mhi_stopped=1;
 print("op13-wifi-mhi: endpoint stopped after failure; buffers/context retained until cold reset\n");
}
static int op13_wifi_mhi_poll(void)
{
 struct mhi_host *h=&op13_wifi_mhi_host;
 if(!h->allocated||!h->powered||op13_wifi_mhi_stopped)return -1;
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -2;
 uint64_t begin=op13_wifi_mhi_op_time(0);
 while(op13_wifi_mhi_op_time(0)-begin<10000000000ULL){
  mhi_host_interrupt(h,-1);
  if(h->failed||h->ee==MHI_HOST_EE_RDDM||h->state==MHI_HOST_STATE_SYS_ERR||op13_wifi_mhi_adapter_fault)break;
  if(op13_wifi_qrtr_ack_pending){
   uint8_t ack[52]={0};op13_wifi_qrtr_ack_pending=0;
   op13_qrtr_put(ack,1);op13_qrtr_put(ack+4,7);op13_qrtr_put(ack+8,1);op13_qrtr_put(ack+12,0x4000);
   op13_qrtr_put(ack+20,20);op13_qrtr_put(ack+24,op13_wifi_qrtr_ack_node);op13_qrtr_put(ack+28,op13_wifi_qrtr_ack_port);
   op13_qrtr_put(ack+32,7);op13_qrtr_put(ack+36,1);op13_qrtr_put(ack+40,0x4000);
   if(mhi_host_send(h,20,ack,sizeof ack)){op13_wifi_mhi_adapter_fault=1;break;}
  }
  if(op13_wifi_qmi_waiting&&op13_wifi_qmi_response)break;
  BS->stall(200);
 }
 int failed=h->failed||h->ee!=MHI_HOST_EE_AMSS||h->state==MHI_HOST_STATE_SYS_ERR||op13_wifi_mhi_adapter_fault;
 print("op13-wifi-qrtr: poll ms=%d failed=%d EE=%d state=%d events=%d packets=%d malformed=%d services=%d WLFW=%d:%x\n",
  (op13_wifi_mhi_op_time(0)-begin)/1000000,(uint64_t)failed,(uint64_t)h->ee,(uint64_t)h->state,h->serviced,
  (uint64_t)op13_wifi_qrtr_received,(uint64_t)op13_wifi_qrtr_bad,(uint64_t)op13_wifi_qrtr_services,(uint64_t)op13_wifi_wlfw_node,(uint64_t)op13_wifi_wlfw_port);
 BS->set_watchdog_timer(0,0,0,0);
 /* Read only our owned SMMU context while the endpoint remains healthy. */
 if(!failed)print("op13-wifi-dma: owned-context FSR=%x FAR=%x:%x FSYNR=%x\n",
  (uint64_t)op13_wifi_dma_cb_read(0x58),(uint64_t)op13_wifi_dma_cb_read(0x64),
  (uint64_t)op13_wifi_dma_cb_read(0x60),(uint64_t)op13_wifi_dma_cb_read(0x68));
 if(failed)op13_wifi_mhi_stop_failed();
 return failed?-3:0;
}
static int op13_wifi_mhi_runtime(void)
{
 if(op13_wifi_mhi_prepare())return -1;
 static const struct mhi_host_ops ops={op13_wifi_mhi_op_read,op13_wifi_mhi_op_write,op13_wifi_mhi_op_alloc,
  op13_wifi_mhi_op_free,op13_wifi_mhi_op_time,op13_wifi_mhi_op_lock,op13_wifi_mhi_op_lock,
  op13_wifi_mhi_op_pause,op13_wifi_mhi_op_random,op13_wifi_mhi_op_say,op13_wifi_mhi_op_status,op13_wifi_mhi_op_receive};
 struct mhi_host_config cfg={0};
 cfg.max_channels=72;cfg.timeout_ms=2500;cfg.buffer_length=0x8000;cfg.channel_count=2;cfg.event_count=4;cfg.offload_event_mask=1u<<3;
 cfg.channels[0]=(struct mhi_host_channel_config){20,64,1,0,4};
 cfg.channels[1]=(struct mhi_host_channel_config){21,64,1,1,4};
 cfg.events[0]=(struct mhi_host_event_config){32,1,0,1};
 cfg.events[1]=(struct mhi_host_event_config){256,2,0,0};
 /* Native CNSS satellite event3 is offloaded: zero context, no host doorbells. */
 cfg.events[2]=(struct mhi_host_event_config){32,1,0,0};
 cfg.events[3]=(struct mhi_host_event_config){256,2,0,0};
 cfg.image=(const uint8_t *)(uintptr_t)PAYLOAD_BASE;cfg.image_length=6840384;
 cfg.image_offset=cfg.sbl_length=cfg.segment_length=0x80000;
 /* Exact vendor cnss_pci_parse_iommu_resv_regions(): gaps between DT
  * reserved [0,18000000) and [b0000000,100000000). Only MHI limits
  * change; private SMMU tables still expose only our64MiB buffer. */
 cfg.iova_start=0x18000000ULL;cfg.iova_stop=0xb0000000ULL;
 print("op13-wifi-mhi: DMA pool translation PAR=%x MHI window=%x..%x\n",uefi_page_translation(op13_wifi_dma_pool),cfg.iova_start,cfg.iova_stop);
 int rc=mhi_host_init(&op13_wifi_mhi_host,&ops,0,&cfg);
 print("op13-wifi-mhi: host init=%d DMA used=%x\n",(uint64_t)(int64_t)rc,op13_wifi_mhi_used);
 if(rc)return rc;
 fb_reply("INFO","MHI runtime firmware boot next; all DMA buffers privately owned");
 efi_status s=BS->set_watchdog_timer(30,0x10000,0,0);if(EFI_ERROR(s))return -2;
 rc=mhi_host_power_up(&op13_wifi_mhi_host);
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-mhi: runtime result=%d EE=%x state=%x events=%d adapter-fault=%d\n",(uint64_t)(int64_t)rc,
  (uint64_t)op13_wifi_mhi_host.ee,(uint64_t)op13_wifi_mhi_host.state,op13_wifi_mhi_host.serviced,(uint64_t)op13_wifi_mhi_adapter_fault);
 if(rc||op13_wifi_mhi_adapter_fault){op13_wifi_mhi_stop_failed();return -3;}
 uint8_t hello[52];op13_qrtr_hello(hello);
 s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -4;
 rc=mhi_host_send(&op13_wifi_mhi_host,20,hello,sizeof hello);
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-qrtr: HELLO queued result=%d; checking firmware service announcements\n",(uint64_t)(int64_t)rc);
 if(rc){op13_wifi_mhi_stop_failed();return -5;}
 return op13_wifi_mhi_poll();
}

static int op13_wifi_qmi_cap(void)
{
 if(!op13_wifi_wlfw_node||!op13_wifi_wlfw_port||op13_wifi_mhi_stopped||op13_wifi_cap_asked)return -1;
 uint8_t packet[40];op13_qmi_cap_request(packet,op13_wifi_wlfw_node,op13_wifi_wlfw_port);
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -2;
 op13_wifi_cap_asked=1;
 int rc=mhi_host_send(&op13_wifi_mhi_host,20,packet,sizeof packet);
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-qmi: read-only CAP request queued result=%d\n",(uint64_t)(int64_t)rc);
 if(rc){op13_wifi_mhi_stop_failed();return -3;}
 rc=op13_wifi_mhi_poll();
 print("op13-wifi-qmi: capability query received=%d result=%d; no radio mode or calibration writes\n",(uint64_t)op13_wifi_cap_received,(uint64_t)op13_wifi_cap.result);
 return rc?rc:(!op13_wifi_cap_received||op13_wifi_cap.result)?-4:0;
}

static int op13_wifi_qmi_transaction(unsigned txn,unsigned id,const uint8_t *tlvs,size_t size)
{
 static uint8_t packet[6300];size_t length=op13_qmi_request(packet,sizeof packet,op13_wifi_wlfw_node,op13_wifi_wlfw_port,txn,id,tlvs,size);
 if(!length||op13_wifi_mhi_stopped)return -1;
 op13_wifi_qmi_expected_txn=txn;op13_wifi_qmi_expected_id=id;op13_wifi_qmi_response=0;
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -2;
 int rc=mhi_host_send(&op13_wifi_mhi_host,20,packet,length);
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-qmi: request txn=%d id=%x queued=%d\n",(uint64_t)txn,(uint64_t)id,(uint64_t)(int64_t)rc);
 if(rc){op13_wifi_mhi_stop_failed();return -3;}
 op13_wifi_qmi_waiting=1;rc=op13_wifi_mhi_poll();op13_wifi_qmi_waiting=0;
 return rc?rc:(!op13_wifi_qmi_response||op13_wifi_qmi_result_code)?-4:0;
}
 static const uint8_t op13_wifi_qmi_indications[]={0x15,4,0,0x4c,0x45,0x4e,0x4b,
  0x16,1,0,1,0x17,1,0,1,0x18,1,0,1};
static int op13_wifi_qmi_init(void)
{
 if(!op13_wifi_wlfw_node||!op13_wifi_wlfw_port||op13_wifi_mhi_stopped||op13_wifi_init_asked)return -1;
 op13_wifi_init_asked=1;
 /* Native client KNEL. Subscribe only to indications consumed by this probe. */

 int rc=op13_wifi_qmi_transaction(2,0x20,op13_wifi_qmi_indications,sizeof op13_wifi_qmi_indications);if(rc)return rc;
 /* Native one-client BDF/M3 model. These are startup declarations only:
  * no radio mode, calibration command, or memory addresses sent by this probe. */
 static const uint8_t host[]={0x10,4,0,1,0,0,0,
  0x14,1,0,1,0x16,1,0,1,0x17,1,0,1,0x1a,1,0,0,
  /* Native SMMU address window, including the64KiB IPA reservation.
   * QMI describes the platform aperture; only our64MiB remains mapped.
   * Three static {u64 start,u64 size} entries, unused entries zero. */
  0x1f,48,0,
  0,0,0,0x18,0,0,0,0, 0,0,1,0x98,0,0,0,0,
  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
  /* Actual Android cnss2.ko sets host_build_type_valid=1 and type=2.
   * Native sun_gki_defconfig has no CONFIG_CNSS2_DEBUG; qmi.h selects SECONDARY. */
  0x20,4,0, 2,0,0,0,
  /* Native feature bit6: AUX_UC_SUPPORT (main.c, PEACH). */
  0x27,8,0, 0x40,0,0,0,0,0,0,0,
  /* Native PEACH two-link/single-chip MLO topology (CNSS qmi.c).
   * TLV26 is a static array of three six-byte structs, without a count byte. */
  0x21,1,0,1, 0x22,2,0,0,0, 0x23,1,0,0,
  0x24,2,0,32,0, 0x25,1,0,1,
  0x26,18,0, 0,2,0,1,1,1, 0,0,0,0,0,0, 0,0,0,0,0,0};
 rc=op13_wifi_qmi_transaction(3,0x34,host,sizeof host);
 if(!rc&&!op13_wifi_mem_received)rc=op13_wifi_mhi_poll();
 print("op13-wifi-qmi: init result=%d memory-request-received=%d; radio not configured\n",(uint64_t)(int64_t)rc,(uint64_t)op13_wifi_mem_received);
 return rc;
}

static int op13_wifi_qmi_memory(void)
{
 uint64_t offsets[52]={0};
 if(!op13_wifi_mem_received||op13_wifi_memory_asked||op13_wifi_mhi_stopped||
    op13_qmi_memory_plan(&op13_wifi_mem,0x4000000ULL,offsets))return -1;
 op13_wifi_memory_asked=1;
 uint8_t tlvs[4+3*17]={0};size_t size=4+17*op13_wifi_mem.count;
 tlvs[0]=1;tlvs[1]=1+17*op13_wifi_mem.count;tlvs[3]=op13_wifi_mem.count;
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -2;
 for(unsigned i=0;i<op13_wifi_mem.count;i++){
  uint64_t address=OP13_WIFI_IOVA+offsets[i];uint8_t *v=tlvs+4+17*i;
  memset((void *)(uintptr_t)(op13_wifi_dma_pool+offsets[i]),0,op13_wifi_mem.seg[i].size);
  op13_qrtr_put(v,(uint32_t)address);op13_qrtr_put(v+4,(uint32_t)(address>>32));
  op13_qrtr_put(v+8,op13_wifi_mem.seg[i].size);op13_qrtr_put(v+12,op13_wifi_mem.seg[i].type);
  print("op13-wifi-qmi: owned memory segment=%d IOVA=%x bytes=%x type=%d\n",(uint64_t)i,address,(uint64_t)op13_wifi_mem.seg[i].size,(uint64_t)op13_wifi_mem.seg[i].type);
 }
 __asm__ volatile("dsb sy":::"memory");
 BS->set_watchdog_timer(0,0,0,0);
 int rc=op13_wifi_qmi_transaction(4,0x36,tlvs,size);
 if(!rc&&!op13_wifi_mem_ready)rc=op13_wifi_mhi_poll();
 print("op13-wifi-qmi: memory setup result=%d FW_MEM_READY=%d\n",(uint64_t)(int64_t)rc,(uint64_t)op13_wifi_mem_ready);
 return rc?rc:op13_wifi_mem_ready?0:-4;
}

static unsigned op13_wifi_files_txn=10;
static int op13_wifi_qmi_dma_file(unsigned file,unsigned id,uint64_t offset)
{
 const struct op13_wifi_file *f=&op13_wifi_files[file];
 if(offset<0x3000000ULL||offset>OP13_WIFI_DMA_BYTES-4096||f->size>OP13_WIFI_DMA_BYTES-4096-offset)return -1;
 uint64_t address=OP13_WIFI_IOVA+offset;
 memcpy((void *)(uintptr_t)(op13_wifi_dma_pool+offset),(const void *)(uintptr_t)(PAYLOAD_BASE+f->offset),f->size);
 __asm__ volatile("dsb sy":::"memory");
 uint8_t tlvs[25]={0};size_t at=0;unsigned addr_type=1;
 if(id==0x5b){tlvs[0]=1;tlvs[1]=4;at=7;addr_type=2;} /* TME patch type0 */
 tlvs[at++]=addr_type;tlvs[at++]=8;tlvs[at++]=0;
 op13_qrtr_put(tlvs+at,(uint32_t)address);op13_qrtr_put(tlvs+at+4,(uint32_t)(address>>32));at+=8;
 tlvs[at++]=addr_type+1;tlvs[at++]=4;tlvs[at++]=0;op13_qrtr_put(tlvs+at,f->size);at+=4;
 print("op13-wifi-qmi: owned file=%d bytes=%x IOVA=%x message=%x\n",(uint64_t)file,(uint64_t)f->size,address,(uint64_t)id);
 return op13_wifi_qmi_transaction(op13_wifi_files_txn++,id,tlvs,at);
}
static int op13_wifi_qmi_bdf(unsigned file,unsigned type)
{
 const struct op13_wifi_file *f=&op13_wifi_files[file];
 const uint8_t *data=(const uint8_t *)(uintptr_t)(PAYLOAD_BASE+f->offset);
 static uint8_t tlvs[6200];unsigned segment=0;uint32_t remaining=f->size;
 unsigned board=op13_wifi_cap.has_board?op13_wifi_cap.board:0xff;
 while(remaining){
  size_t bytes=remaining>6144?6144:remaining;
  size_t size=op13_qmi_bdf_chunk(tlvs,sizeof tlvs,board,type,segment,remaining,data,bytes);
  if(!size)return -1;
  int rc=op13_wifi_qmi_transaction(op13_wifi_files_txn++,0x25,tlvs,size);if(rc)return rc;
  data+=bytes;remaining-=bytes;segment++;
 }
 print("op13-wifi-qmi: board file=%d type=%d accepted chunks=%d bytes=%d\n",(uint64_t)file,(uint64_t)type,(uint64_t)segment,(uint64_t)f->size);
 return 0;
}
static int op13_wifi_qmi_qdss(void)
{
 const struct op13_wifi_file *f=&op13_wifi_files[5];
 const uint8_t *data=(const uint8_t *)(uintptr_t)(PAYLOAD_BASE+f->offset);
 static uint8_t tlvs[6200];uint32_t remaining=f->size;unsigned segment=0;
 while(remaining){
  size_t bytes=remaining>6144?6144:remaining;
  size_t n=op13_qmi_config_chunk(tlvs,sizeof tlvs,segment,remaining,data,bytes);if(!n)return -1;
  int rc=op13_wifi_qmi_transaction(op13_wifi_files_txn++,0x44,tlvs,n);if(rc)return rc;
  data+=bytes;remaining-=bytes;segment++;
 }
 print("op13-wifi-qmi: native QDSS config accepted chunks=%d bytes=%d\n",(uint64_t)segment,(uint64_t)f->size);
 return 0;
}
static int op13_wifi_pmu_apply(void)
{
 if(!op13_wifi_pmu_received||!op13_wifi_pmu_valid||op13_wifi_mhi_stopped||op13_wifi_aop_fault)return -1;
 /* All messages prebuilt and validated before touching the existing mailbox. */
 char messages[8][64];unsigned count=0;
 for(unsigned r=0;r<4;r++)for(unsigned sleep=0;sleep<2;sleep++){
  unsigned mv=op13_wifi_pmu_plan.mv[r][sleep];if(!mv)continue;
  if(op13_pmu_message(messages[count],r,sleep,mv))return -2;count++;
 }
 if(!count)return -3;
 struct op13_wifi_qmp_io io={0,op13_wifi_aop_read,op13_wifi_aop_write,op13_wifi_aop_ring,op13_wifi_aop_delay};
 efi_status status=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(status))return -4;
 uint32_t box=0;int rc=op13_wifi_qmp_box(&io,&box);
 if(!rc&&box!=op13_wifi_aop_box)rc=-1;
 for(unsigned i=0;!rc&&i<count;i++){
  rc=op13_wifi_qmp_send(&io,box,messages[i]);
  print("op13-wifi-pmu: native AOP %s result=%d\n",messages[i],(uint64_t)(int64_t)rc);
  if(op13_wifi_aop_fault)rc=-1;
 }
 BS->set_watchdog_timer(0,0,0,0);
 if(rc){op13_wifi_mhi_stop_failed();return -5;}
 return 0;
}
static int op13_wifi_qmi_files(void)
{
 if(!op13_wifi_mem_ready||!op13_wifi_cap_received||op13_wifi_cap.result||op13_wifi_files_asked||op13_wifi_mhi_stopped)return -1;
 /* Verify every extra file before any file is offered to the device. */
 for(unsigned i=0;i<6;i++){
  const struct op13_wifi_file *f=&op13_wifi_files[i];
  if(f->offset<0x800000||f->offset>0x1000000||!f->size||f->size>0x1000000-f->offset||
     ufs_fnv(0xcbf29ce484222325ULL,(const uint8_t *)(uintptr_t)(PAYLOAD_BASE+f->offset),f->size)!=f->fnv)return -2;
 }
 op13_wifi_files_asked=1;
 int pmu_rc=op13_wifi_pmu_apply();if(pmu_rc)return pmu_rc;
 fb_reply("INFO","Loading verified native radio files into owned RAM");
 int rc=op13_wifi_qmi_dma_file(0,0x5b,0x3000000);
 if(rc){
  /* Exact same result1/error83 is recorded in Android's native boot log;
   * cnss_fw_mem_ready_hdlr deliberately does not gate startup on this patch. */
  if(op13_wifi_mhi_stopped||!op13_wifi_qmi_response||op13_wifi_qmi_result_code!=1||op13_wifi_qmi_error_code!=83)return rc;
  print("op13-wifi-qmi: optional TME patch error83 matches native Android; continuing normal startup\n");
 }
 rc=op13_wifi_qmi_bdf(1,4);if(rc)return rc;
 rc=op13_wifi_qmi_bdf(2,1);if(rc)return rc;
 rc=op13_wifi_qmi_dma_file(3,0x3c,0x3100000);if(rc)return rc;
 rc=op13_wifi_qmi_dma_file(4,0x5a,0x3200000);if(rc)return rc;
 rc=op13_wifi_qmi_qdss();if(rc)return rc;
 rc=op13_wifi_mhi_poll();if(rc)return rc;
 /* Re-register the identical client/subscriptions to read firmware's state
  * bitmap; no host-cap reset, radio mode, or new resources. */
 rc=op13_wifi_qmi_transaction(op13_wifi_files_txn++,0x20,op13_wifi_qmi_indications,sizeof op13_wifi_qmi_indications);
 print("op13-wifi-qmi: native file sequence result=%d FW_INIT_DONE=%d; scan/data path not configured\n",(uint64_t)(int64_t)rc,(uint64_t)op13_wifi_fw_init_done);
 return rc;
}
