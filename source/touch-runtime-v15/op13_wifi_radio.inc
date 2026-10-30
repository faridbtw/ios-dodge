#include "op13_wifi_radio.h"
#include "op13_wifi_rx_queue.h"
#include "op13_wifi_htt.h"
static unsigned op13_wifi_htt_requested,op13_wifi_htt_version_seen,op13_wifi_htt_major,op13_wifi_htt_minor;
#include "op13_wifi_wmi_command.h"
static unsigned op13_wifi_wmi_credits,op13_wifi_wmi_credit_size;
static uint32_t op13_wifi_wmi_abi[6];
static unsigned op13_wifi_wmi_abi_valid,op13_wifi_wmi_init_sent,op13_wifi_wmi_ready;
static unsigned op13_wifi_wmi_endpoint,op13_wifi_htt_endpoint,op13_wifi_service_done;
static unsigned op13_wifi_service_expected,op13_wifi_service_status,op13_wifi_wmi_service_ready;
static unsigned op13_wifi_wmi_connect_requested,op13_wifi_htc_sequence;
static uint32_t op13_wifi_ce_tx_hp[3];
static uint8_t op13_wifi_wmi_capture[65536];static unsigned op13_wifi_wmi_capture_len;
static unsigned op13_wifi_radio_requested,op13_wifi_htc_ready,op13_wifi_ce_rx_count;
static uint32_t op13_wifi_rx_tp[2];
static struct op13_rx_queue op13_wifi_rx_queue[2];
static unsigned op13_wifi_rx_queue_ready,op13_wifi_rx_deferred;
static uint8_t op13_wifi_htc_error[3528];
static unsigned op13_wifi_htc_error_len;
static int op13_wifi_ce_receive(void)
{
 const struct op13_ce_io io={0,op13_wifi_ce_init_read,op13_wifi_ce_init_write};
 uint32_t saved,value,selected;int rc=-1;
 if(!op13_wifi_rx_queue_ready){for(unsigned i=0;i<2;i++)op13_rx_queue_init(&op13_wifi_rx_queue[i]);op13_wifi_rx_queue_ready=1;}
 if(io.read(0,OP13_CE_WINDOW,&saved)||saved==~0u)return -1;
 selected=(saved&~0x3fu)|0x40000037u;
 if(io.write(0,OP13_CE_WINDOW,selected)||io.read(0,OP13_CE_WINDOW,&value)||value!=selected)goto restore;
 rc=0;
 for(unsigned which=0;!rc&&which<2;which++){
  const struct op13_ce_ring *buffers=&op13_ce_rings[1+2*which],*status=&op13_ce_rings[2+2*which];
  uint32_t hp=0,r2=op13_ce_r2(status),size=status->entries*status->words;
  if(io.read(0,r2,&hp)||hp>=size||hp%status->words){rc=-2;break;}
  __asm__ volatile("dsb sy":::"memory");
  while(op13_wifi_rx_tp[which]!=hp){
   unsigned index=0,tp=op13_wifi_rx_tp[which];
   if(op13_rx_queue_buffer(&op13_wifi_rx_queue[which],&index)||tp>=size||tp%status->words){rc=-3;break;}
   const volatile uint32_t *desc=(const volatile uint32_t *)(uintptr_t)
      (op13_wifi_dma_pool+status->ring_offset+(uint64_t)tp*4);
   unsigned bytes=desc[0]>>16;
   /* Native ce_completed_recv_next_nolock_srng: DRRI can precede descriptor DMA.
    * Zero length is not completed: leave TP and ownership untouched and retry.
    * A bounded outer polling deadline prevents an infinite wait. */
   if(!bytes){op13_wifi_rx_deferred++;break;}
   if(bytes<8||bytes>buffers->max_bytes){print("op13-wifi-ce: invalid RX size=%d CE%d tp=%d hp=%d\n",(uint64_t)bytes,(uint64_t)buffers->ce,(uint64_t)tp,(uint64_t)hp);rc=-4;break;}
   const uint8_t *packet=(const uint8_t *)(uintptr_t)
      (op13_wifi_dma_pool+buffers->buffer_offset+(uint64_t)index*buffers->stride);
   struct op13_htc_frame frame;
   if(op13_htc_decode(packet,bytes,&frame)){memcpy(op13_wifi_htc_error,packet,bytes);op13_wifi_htc_error_len=bytes;rc=-5;break;}
   if(frame.flags&2){
    unsigned credits=0;
    int cr=op13_htc_credit_delta(packet+8+frame.data_bytes,frame.payload_bytes-frame.data_bytes,
                            op13_wifi_wmi_endpoint,&credits);
    if(cr||op13_wifi_wmi_credits>2||credits>2-op13_wifi_wmi_credits){
     print("op13-wifi-htc: trailer rejected parse=%d delta=%d available=%d trailer=%d\n",(uint64_t)(int64_t)cr,(uint64_t)credits,(uint64_t)op13_wifi_wmi_credits,(uint64_t)(frame.payload_bytes-frame.data_bytes));
     memcpy(op13_wifi_htc_error,packet,bytes);op13_wifi_htc_error_len=bytes;rc=-9;break;
    }
    op13_wifi_wmi_credits+=credits;
    if(credits)print("op13-wifi-htc: WMI credits +%d available=%d\n",(uint64_t)credits,(uint64_t)op13_wifi_wmi_credits);
   }
   op13_wifi_ce_rx_count++;
   print("op13-wifi-htc: CE%d RX bytes=%d endpoint=%d flags=%x payload=%d message=%d\n",
    (uint64_t)buffers->ce,(uint64_t)bytes,(uint64_t)frame.endpoint,(uint64_t)frame.flags,
    (uint64_t)frame.payload_bytes,(uint64_t)frame.message);
   if(!frame.endpoint&&frame.message==1){
    op13_wifi_htc_ready=1;op13_wifi_wmi_credit_size=frame.credit_size;
    print("op13-wifi-htc: READY credits=%d size=%d endpoints=%d\n",
     (uint64_t)frame.credits,(uint64_t)frame.credit_size,(uint64_t)frame.max_endpoints);
   }
   if(!frame.endpoint&&frame.message==3&&frame.service==op13_wifi_service_expected){
    op13_wifi_service_done=1;op13_wifi_service_status=frame.status;
    if(!frame.status){
     if(frame.service==0x100){op13_wifi_wmi_endpoint=frame.assigned;op13_wifi_wmi_credits=2;}
     if(frame.service==0x300)op13_wifi_htt_endpoint=frame.assigned;
    }
    print("op13-wifi-htc: service=%x status=%d endpoint=%d max-message=%d\n",
     (uint64_t)frame.service,(uint64_t)frame.status,(uint64_t)frame.assigned,(uint64_t)frame.max_message);
   }
   if(frame.endpoint&&frame.endpoint==op13_wifi_htt_endpoint&&frame.data_bytes){
    unsigned major=0,minor=0;
    int hr=op13_htt_version_response(packet+8,frame.data_bytes,&major,&minor);
    print("op13-wifi-htt: RX bytes=%d type=%d version-decode=%d\n",(uint64_t)frame.data_bytes,(uint64_t)(packet[8]&255),(uint64_t)(int64_t)hr);
    if(!hr){op13_wifi_htt_version_seen=1;op13_wifi_htt_major=major;op13_wifi_htt_minor=minor;}
   }
   if(frame.endpoint&&frame.endpoint==op13_wifi_wmi_endpoint){
    unsigned payload=frame.data_bytes;
    if(payload>=4){
     unsigned event=op13_qrtr_u32(packet+8)&0xffffffu;
     print("op13-wifi-wmi: event=%x bytes=%d endpoint=%d\n",(uint64_t)event,(uint64_t)payload,(uint64_t)frame.endpoint);
     if(event==1)op13_wifi_wmi_service_ready=1;
     for(size_t at=4;at+4<=payload;){
      uint32_t h=op13_qrtr_u32(packet+8+at);unsigned tag=h>>16,n=h&65535;
      if(n>payload-at-4){rc=-10;break;}
      const uint8_t *v=packet+8+at+4;
      if(event==1&&tag==32&&n==128){
       for(unsigned i=0;i<6;i++)op13_wifi_wmi_abi[i]=op13_qrtr_u32(v+4+i*4);
       op13_wifi_wmi_abi_valid=op13_qrtr_u32(v+72)==0;
      }
      if(event==2&&tag==35&&n>=36){
       uint32_t status=op13_qrtr_u32(v+32);op13_wifi_wmi_ready=status==0;
       print("op13-wifi-wmi: READY status=%d successful=%d\n",(uint64_t)status,(uint64_t)op13_wifi_wmi_ready);
      }
      at+=4+n;
     }
     if(rc)break;
     if(payload+8<=sizeof op13_wifi_wmi_capture-op13_wifi_wmi_capture_len){
      uint8_t *at=op13_wifi_wmi_capture+op13_wifi_wmi_capture_len;
      op13_qrtr_put(at,event);op13_qrtr_put(at+4,payload);memcpy(at+8,packet+8,payload);
      op13_wifi_wmi_capture_len+=payload+8;
     }
    }
   }
   if(op13_rx_queue_complete(&op13_wifi_rx_queue[which])){rc=-11;break;}
   /* Clear completion length before returning this status slot to hardware. */
   ((volatile uint32_t *)desc)[0]&=0xffffu;
   __asm__ volatile("dsb sy":::"memory");
   op13_wifi_rx_tp[which]=(tp+status->words)&(size-1);
   if(io.write(0,r2+4,op13_wifi_rx_tp[which])){rc=-6;break;}
  }
  while(!rc&&op13_wifi_rx_queue[which].count){
   uint32_t hardware_tp;unsigned slot,id;
   if(io.read(0,op13_ce_r2(buffers)+4,&hardware_tp)){rc=-12;break;}
   int available=op13_rx_queue_refill(&op13_wifi_rx_queue[which],hardware_tp,&slot,&id);
   if(available<0){rc=-13;break;}if(!available)break;
   uint64_t address=OP13_WIFI_IOVA+buffers->buffer_offset+(uint64_t)id*buffers->stride;
   volatile uint32_t *d=(volatile uint32_t *)(uintptr_t)(op13_wifi_dma_pool+buffers->ring_offset+(uint64_t)slot*8);
   d[0]=(uint32_t)address;d[1]=(uint32_t)(address>>32);
   __asm__ volatile("dsb sy":::"memory");
   uint32_t next=((slot+1)&63)*2;
   if(io.write(0,op13_ce_r2(buffers),next)||io.read(0,op13_ce_r2(buffers),&value)||value!=next){rc=-14;break;}
   if(op13_rx_queue_post(&op13_wifi_rx_queue[which])){rc=-15;break;}
  }
 }
restore:
 if(io.write(0,OP13_CE_WINDOW,saved)||io.read(0,OP13_CE_WINDOW,&value)||value!=saved)return -7;
 return rc;
}
static int op13_wifi_radio_start(void)
{
 struct mhi_host *h=&op13_wifi_mhi_host;
 if(!op13_wifi_fw_init_done||!op13_wifi_ce_rings_ready||op13_wifi_ce_init_fault||
    op13_wifi_radio_requested||op13_wifi_mhi_stopped||!h->powered||h->failed||h->ee!=MHI_HOST_EE_AMSS||h->state!=MHI_HOST_STATE_M0)return -1;
 op13_wifi_radio_requested=1;
 uint8_t config[400];size_t bytes=op13_qmi_wlan_config(config,sizeof config);if(!bytes)return -2;
 int rc=op13_wifi_qmi_transaction(op13_wifi_files_txn++,0x23,config,bytes);
 print("op13-wifi-radio: native WLAN config result=%d, seven owned host rings ready\n",(uint64_t)(int64_t)rc);
 if(rc)return rc;
 static const uint8_t mode[]={1,4,0,0,0,0,0,0x10,1,0,0}; /* Native mission mode, hw_debug0. */
 rc=op13_wifi_qmi_transaction(op13_wifi_files_txn++,0x22,mode,sizeof mode);
 print("op13-wifi-radio: mission mode result=%d; awaiting HTC in owned RX buffers\n",(uint64_t)(int64_t)rc);
 if(rc)return rc;
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -3;
 uint64_t begin=op13_wifi_mhi_op_time(0);
 while(op13_wifi_mhi_op_time(0)-begin<10000000000ULL){
  mhi_host_interrupt(h,-1);
  if(h->failed||h->ee!=MHI_HOST_EE_AMSS||h->state==MHI_HOST_STATE_SYS_ERR||op13_wifi_mhi_adapter_fault){rc=-8;break;}
  if(op13_wifi_qrtr_ack_pending){
   uint8_t ack[52]={0};op13_wifi_qrtr_ack_pending=0;
   op13_qrtr_put(ack,1);op13_qrtr_put(ack+4,7);op13_qrtr_put(ack+8,1);op13_qrtr_put(ack+12,0x4000);
   op13_qrtr_put(ack+20,20);op13_qrtr_put(ack+24,op13_wifi_qrtr_ack_node);op13_qrtr_put(ack+28,op13_wifi_qrtr_ack_port);
   op13_qrtr_put(ack+32,7);op13_qrtr_put(ack+36,1);op13_qrtr_put(ack+40,0x4000);
   if(mhi_host_send(h,20,ack,sizeof ack)){rc=-9;break;}
  }
  rc=op13_wifi_ce_receive();if(rc||op13_wifi_htc_ready)break;
  BS->stall(200);
 }
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-radio: HTC ready=%d RX=%d result=%d EE=%d state=%d; no scan/join issued\n",
  (uint64_t)op13_wifi_htc_ready,(uint64_t)op13_wifi_ce_rx_count,(uint64_t)(int64_t)rc,
  (uint64_t)h->ee,(uint64_t)h->state);
 if(rc){op13_wifi_ce_init_fault=1;op13_wifi_mhi_stop_failed();return rc;}
 return op13_wifi_htc_ready?0:-10;
}
static int op13_wifi_htc_send(unsigned ce,unsigned endpoint,const uint8_t *payload,size_t bytes)
{
 if(ce!=0&&ce!=3&&ce!=4)return -1;
 unsigned which=ce==4?2:ce==3;const struct op13_ce_ring *r=&op13_ce_rings[which==2?6:which==1?5:0];
 unsigned hp=op13_wifi_ce_tx_hp[which],size=r->entries*r->words,next=(hp+r->words)&(size-1);
 if(hp>=size||hp%r->words||bytes+8>r->max_bytes||endpoint>=32)return -2;
 const struct op13_ce_io io={0,op13_wifi_ce_init_read,op13_wifi_ce_init_write};
 uint32_t saved,selected,value;int rc=-3;
 if(io.read(0,OP13_CE_WINDOW,&saved)||saved==~0u)return -3;
 selected=(saved&~0x3fu)|0x40000037u;
 if(io.write(0,OP13_CE_WINDOW,selected)||io.read(0,OP13_CE_WINDOW,&value)||value!=selected)goto restore;
 if(io.read(0,op13_ce_r2(r)+4,&value)||value>=size||value%r->words||next==value)goto restore;
 unsigned slot=hp/r->words;
 uint64_t offset=r->buffer_offset+(uint64_t)slot*r->stride,address=OP13_WIFI_IOVA+offset;
 uint8_t *packet=(uint8_t *)(uintptr_t)(op13_wifi_dma_pool+offset);
 size_t n=op13_htc_encode(packet,r->stride,endpoint,op13_wifi_htc_sequence++,payload,bytes);
 if(n&&ce==3&&endpoint==op13_wifi_wmi_endpoint)packet[1]|=1; /* Request credit return. */
 uint32_t descriptor[4];
 if(!n||op13_ce_tx_desc(descriptor,address,n,OP13_WIFI_IOVA+r->buffer_offset,
                       OP13_WIFI_IOVA+r->buffer_offset+(uint64_t)r->entries*r->stride))goto restore;
 memcpy((void *)(uintptr_t)(op13_wifi_dma_pool+r->ring_offset+(uint64_t)hp*4),descriptor,sizeof descriptor);
 __asm__ volatile("dsb sy":::"memory");
 if(io.write(0,op13_ce_r2(r),next)||io.read(0,op13_ce_r2(r),&value)||value!=next)goto restore;
 op13_wifi_ce_tx_hp[which]=next;rc=0;
 print("op13-wifi-htc: CE%d TX bytes=%d endpoint=%d hp=%d\n",(uint64_t)ce,(uint64_t)n,(uint64_t)endpoint,(uint64_t)next);
restore:
 if(io.write(0,OP13_CE_WINDOW,saved)||io.read(0,OP13_CE_WINDOW,&value)||value!=saved)return -4;
 return rc;
}
static int op13_wifi_wmi_wait(unsigned phase)
{
 struct mhi_host *h=&op13_wifi_mhi_host;int rc=0;
 efi_status s=BS->set_watchdog_timer(20,0x10000,0,0);if(EFI_ERROR(s))return -1;
 uint64_t begin=op13_wifi_mhi_op_time(0);
 while(op13_wifi_mhi_op_time(0)-begin<5000000000ULL){
  mhi_host_interrupt(h,-1);
  if(h->failed||h->ee!=MHI_HOST_EE_AMSS||h->state==MHI_HOST_STATE_SYS_ERR||op13_wifi_mhi_adapter_fault){rc=-2;break;}
  if(op13_wifi_qrtr_ack_pending){
   uint8_t ack[52]={0};op13_wifi_qrtr_ack_pending=0;
   op13_qrtr_put(ack,1);op13_qrtr_put(ack+4,7);op13_qrtr_put(ack+8,1);op13_qrtr_put(ack+12,0x4000);
   op13_qrtr_put(ack+20,20);op13_qrtr_put(ack+24,op13_wifi_qrtr_ack_node);op13_qrtr_put(ack+28,op13_wifi_qrtr_ack_port);
   op13_qrtr_put(ack+32,7);op13_qrtr_put(ack+36,1);op13_qrtr_put(ack+40,0x4000);
   if(mhi_host_send(h,20,ack,sizeof ack)){rc=-3;break;}
  }
  rc=op13_wifi_ce_receive();if(rc)break;
  if(phase==1&&op13_wifi_service_done)break;
  if(phase==3&&op13_wifi_wmi_ready)break;
  if(phase==2&&op13_wifi_wmi_service_ready&&op13_wifi_mhi_op_time(0)-begin>1000000000ULL)break;
  BS->stall(200);
 }
 BS->set_watchdog_timer(0,0,0,0);
 print("op13-wifi-wmi: wait phase=%d rc=%d failed=%d EE=%d state=%d adapter-fault=%d ready=%d credits=%d deferred=%d\n",(uint64_t)phase,(uint64_t)(int64_t)rc,(uint64_t)h->failed,(uint64_t)h->ee,(uint64_t)h->state,(uint64_t)op13_wifi_mhi_adapter_fault,(uint64_t)op13_wifi_wmi_ready,(uint64_t)op13_wifi_wmi_credits,(uint64_t)op13_wifi_rx_deferred);
 if(rc){op13_wifi_ce_init_fault=1;op13_wifi_mhi_stop_failed();return rc;}
 return phase==4?0:(phase==3?op13_wifi_wmi_ready:phase==1?op13_wifi_service_done&&!op13_wifi_service_status:op13_wifi_wmi_service_ready)?0:-4;
}
static int op13_wifi_connect_service(unsigned service,unsigned flags)
{
 uint8_t payload[8]={2,0,0,0,0,0,0,0};payload[2]=service;payload[3]=service>>8;payload[4]=flags;payload[5]=flags>>8;
 op13_wifi_service_expected=service;op13_wifi_service_done=0;op13_wifi_service_status=~0u;
 int rc=op13_wifi_htc_send(0,0,payload,sizeof payload);if(rc){op13_wifi_ce_init_fault=1;op13_wifi_mhi_stop_failed();return rc;}
 return op13_wifi_wmi_wait(1);
}
static int op13_wifi_wmi_connect(void)
{
 if(!op13_wifi_htc_ready||!op13_wifi_ce_rings_ready||op13_wifi_ce_init_fault||op13_wifi_mhi_stopped||op13_wifi_wmi_connect_requested)return -1;
 op13_wifi_wmi_connect_requested=1;
 int rc=op13_wifi_connect_service(0x100,2u<<8); /* Assign both advertised credits to WMI. */
 if(!rc)rc=op13_wifi_connect_service(0x300,8); /* Native HTT disables credit flow. */
 if(!rc){
  static const uint8_t setup[12]={5,0,0,0,0,0,0,0,0,0,0,0};
  rc=op13_wifi_htc_send(0,0,setup,sizeof setup);
  if(rc){op13_wifi_ce_init_fault=1;op13_wifi_mhi_stop_failed();}
 }
 if(!rc)rc=op13_wifi_wmi_wait(2);
 print("op13-wifi-wmi: connect result=%d WMI-endpoint=%d HTT-endpoint=%d service-ready=%d captured=%d; no scan/join\n",
  (uint64_t)(int64_t)rc,(uint64_t)op13_wifi_wmi_endpoint,(uint64_t)op13_wifi_htt_endpoint,
  (uint64_t)op13_wifi_wmi_service_ready,(uint64_t)op13_wifi_wmi_capture_len);
 return rc;
}

static int op13_wifi_wmi_poll(void)
{
 if(!op13_wifi_wmi_endpoint||op13_wifi_mhi_stopped||op13_wifi_ce_init_fault)return -1;
 int rc=op13_wifi_wmi_wait(4);
 print("op13-wifi-wmi: poll result=%d init-sent=%d ready=%d credits=%d captured=%d\n",
 (uint64_t)(int64_t)rc,(uint64_t)op13_wifi_wmi_init_sent,(uint64_t)op13_wifi_wmi_ready,
 (uint64_t)op13_wifi_wmi_credits,(uint64_t)op13_wifi_wmi_capture_len);return rc;
}
static int op13_wifi_wmi_send_mailbox(void)
{
 unsigned command=0,bytes=op13_wifi_wmi_mailbox_size;
 if(!op13_wifi_wmi_abi_valid||!op13_wifi_wmi_endpoint||!op13_wifi_wmi_credit_size||op13_wifi_mhi_stopped||op13_wifi_ce_init_fault)return -1;
 int rc=op13_wmi_validate(op13_wifi_wmi_mailbox,bytes,op13_wifi_wmi_abi,op13_wifi_wmi_init_sent,op13_wifi_wmi_ready,&command);
 if(rc){print("op13-wifi-wmi: command validation result=%d\n",(uint64_t)(int64_t)rc);return rc;}
 unsigned credits=(bytes+8+op13_wifi_wmi_credit_size-1)/op13_wifi_wmi_credit_size;
 if(credits>op13_wifi_wmi_credits){print("op13-wifi-wmi: insufficient credits need=%d have=%d\n",(uint64_t)credits,(uint64_t)op13_wifi_wmi_credits);return -2;}
 rc=op13_wifi_htc_send(3,op13_wifi_wmi_endpoint,op13_wifi_wmi_mailbox,bytes);
 op13_wifi_wmi_mailbox_size=0;
 if(rc){op13_wifi_ce_init_fault=1;op13_wifi_mhi_stop_failed();return rc;}
 op13_wifi_wmi_credits-=credits;if(command==1)op13_wifi_wmi_init_sent=1;
 print("op13-wifi-wmi: command=%x queued bytes=%d credits=%d\n",(uint64_t)command,(uint64_t)bytes,(uint64_t)op13_wifi_wmi_credits);
 return op13_wifi_wmi_wait(command==1?3:4);
}

static int op13_wifi_htt_probe(void)
{
 if(!op13_wifi_wmi_ready||!op13_wifi_htt_endpoint||op13_wifi_htt_requested||op13_wifi_mhi_stopped||op13_wifi_ce_init_fault)return -1;
 uint8_t command[8];if(op13_htt_version_request(command,sizeof command)!=sizeof command)return -2;
 op13_wifi_htt_requested=1;
 int rc=op13_wifi_htc_send(4,op13_wifi_htt_endpoint,command,sizeof command);
 if(!rc)rc=op13_wifi_wmi_wait(4);
 print("op13-wifi-htt: requested=%d version-seen=%d version=%d.%d rc=%d; no data rings configured\n",(uint64_t)op13_wifi_htt_requested,(uint64_t)op13_wifi_htt_version_seen,(uint64_t)op13_wifi_htt_major,(uint64_t)op13_wifi_htt_minor,(uint64_t)(int64_t)rc);
 return rc?rc:op13_wifi_htt_version_seen?0:-3;
}
