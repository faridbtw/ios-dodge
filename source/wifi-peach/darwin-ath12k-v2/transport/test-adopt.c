#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "op13_mhi_handoff.h"
static unsigned calls;static uint64_t used;
static uint8_t *pool;
static uint32_t rd(void*c,uint32_t x){(void)c;(void)x;calls++;return 0;}
static void wr(void*c,uint32_t x,uint32_t v){(void)c;(void)x;(void)v;calls++;}
static void *alloc(void*c,size_t n,size_t align,uint64_t*a){(void)c;while(align<n)align*=2;used=(used+align-1)&~(uint64_t)(align-1);assert(n<=0x4000000-used);*a=0x18000000+used;void*p=pool+used;used+=n;return p;}
static void fre(void*c,void*p,size_t n){(void)c;(void)p;(void)n;calls++;}
static uint64_t now(void*c){(void)c;calls++;return 0;}
static void lock(void*c){(void)c;calls++;}
static void pause_us(void*c,unsigned us){(void)c;(void)us;calls++;}
static void random_bytes(void*c,void*p,size_t n){(void)c;memset(p,0,n);calls++;}
static void say(void*c,const char*s){(void)c;(void)s;calls++;}
static void status(void*c,enum mhi_host_status s){(void)c;(void)s;calls++;}
static void receive(void*c,unsigned ch,const void*p,size_t n){(void)c;(void)ch;(void)p;(void)n;calls++;}
static const struct mhi_host_ops ops={rd,wr,alloc,fre,now,lock,lock,pause_us,random_bytes,say,status,receive};
static void physical(uint8_t **p){if(*p)*p=(uint8_t*)(uintptr_t)(UINT64_C(0x100000000)+(uintptr_t)*p-(uintptr_t)pool);}
int main(int argc,char **argv){
 if(argc==2){
  struct op13_mhi_handoff captured;FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(&captured,1,sizeof captured,f)==sizeof captured&&fgetc(f)==EOF);fclose(f);
  void *mapped=malloc(0x4000000);assert(mapped);struct mhi_host moved;calls=0;
  assert(!op13_mhi_adopt(&moved,&captured,mapped,&ops,0,1));assert(!calls);
  assert(moved.ops==&ops&&moved.channels[0].config==&moved.config.channels[0]);
  assert(op13_mhi_adopt(&moved,&captured,mapped,&ops,0,0));
  free(mapped);puts("Actual phone snapshot: every MHI DMA pointer relocates, callbacks replaced, diagnostic snapshot correctly refused for transfer");return 0;
 }

 pool=calloc(1,0x4000000);assert(pool);uint8_t*newpool=malloc(0x4000000);assert(newpool);used=4096;
 struct mhi_host_config cfg={0};cfg.max_channels=72;cfg.timeout_ms=2500;cfg.buffer_length=0x8000;cfg.channel_count=2;cfg.event_count=4;cfg.offload_event_mask=8;
 cfg.channels[0]=(struct mhi_host_channel_config){20,64,1,0,4};cfg.channels[1]=(struct mhi_host_channel_config){21,64,1,1,4};
 cfg.events[0]=(struct mhi_host_event_config){32,1,0,1};cfg.events[1]=(struct mhi_host_event_config){256,2,0,0};cfg.events[2]=(struct mhi_host_event_config){32,1,0,0};cfg.events[3]=(struct mhi_host_event_config){256,2,0,0};
 uint8_t*image=calloc(1,6840384);assert(image);cfg.image=image;cfg.image_length=6840384;cfg.image_offset=cfg.sbl_length=cfg.segment_length=0x80000;cfg.iova_start=0x18000000;cfg.iova_stop=0xb0000000;
 struct op13_mhi_handoff lease={0};lease.magic=OP13_MHI_HANDOFF_MAGIC;lease.version=1;lease.bytes=sizeof lease;lease.host_bytes=sizeof lease.host;lease.flags=OP13_MHI_SNAPSHOT;lease.source_host_pa=0xc4040000;lease.pool_pa=UINT64_C(0x100000000);lease.pool_bytes=0x4000000;lease.iova=0x18000000;lease.bar0=0x40400000;lease.bar0_bytes=0x200000;lease.tables_pa=UINT64_C(0x200000000);lease.wlfw_node=7;lease.wlfw_port=1;
 assert(!mhi_host_init(&lease.host,&ops,0,&cfg));
 struct mhi_host*h=&lease.host;h->powered=h->mission=h->mission_told=1;h->ee=MHI_HOST_EE_AMSS;h->state=MHI_HOST_STATE_M0;h->bhi=0x100;h->bhie=0x224;h->channel_doorbells=0x300;h->event_doorbells=0x700;
 physical(&h->channel_contexts);physical(&h->event_contexts);physical(&h->command_context);physical(&h->commands.base);physical(&h->image);physical(&h->vectors);
 for(unsigned i=0;i<4;i++)physical(&h->events[i].base);
 for(unsigned i=0;i<2;i++){
  h->channels[i].config=(const void*)(uintptr_t)(lease.source_host_pa+offsetof(struct mhi_host,config)+offsetof(struct mhi_host_config,channels)+i*sizeof(cfg.channels[0]));
  h->channels[i].running=1;physical(&h->channels[i].ring.base);physical(&h->channels[i].buffers);
 }
 struct mhi_host out;memset(&out,0xa5,sizeof out);unsigned char before[sizeof out];memcpy(before,&out,sizeof out);calls=0;
 assert(op13_mhi_adopt(&out,&lease,newpool,&ops,(void*)123,0));assert(!memcmp(before,&out,sizeof out));
 assert(!op13_mhi_adopt(&out,&lease,newpool,&ops,(void*)123,1)&&!calls);
 assert(out.ops==&ops&&out.context==(void*)123&&out.channels[0].config==&out.config.channels[0]&&out.channels[1].config==&out.config.channels[1]);
 assert(out.config.image==out.image&&out.channels[1].ring.base==newpool+(uintptr_t)h->channels[1].ring.base-lease.pool_pa);
 lease.flags=OP13_MHI_TRANSFER;assert(!op13_mhi_adopt(&out,&lease,newpool,&ops,0,0));
 lease.pool_bytes=0x8000000;assert(!op13_mhi_adopt(&out,&lease,newpool,&ops,0,0));
 uint8_t *saved_ring=lease.host.events[0].base;uint64_t saved_address=lease.host.events[0].address;
 lease.host.events[0].base=(void*)(uintptr_t)(lease.pool_pa+0x4000000);lease.host.events[0].address=lease.iova+0x4000000;
 assert(op13_mhi_adopt(&out,&lease,newpool,&ops,0,0));
 lease.host.events[0].base=saved_ring;lease.host.events[0].address=saved_address;
 uint64_t saved_tables=lease.tables_pa;lease.tables_pa=lease.pool_pa+0x7000000;
 assert(op13_mhi_adopt(&out,&lease,newpool,&ops,0,0));lease.tables_pa=saved_tables;
 struct op13_mhi_handoff save=lease;
 for(unsigned i=0;i<16;i++){
  lease=save;switch(i){
   case 0:lease.magic^=1;break;case 1:lease.bytes--;break;case 2:lease.host_bytes++;break;case 3:lease.bar0++;break;
   case 4:lease.pool_bytes--;break;case 5:lease.pool_pa++;break;case 6:lease.host.failed=1;break;
   case 7:lease.host.state=MHI_HOST_STATE_M2;break;case 8:lease.host.channels[0].busy=1;break;case 9:lease.host.event_busy[0]=1;break;
   case 10:lease.host.commands.base=(void*)(uintptr_t)(lease.pool_pa-16);break;
   case 11:lease.host.commands.rp=lease.host.commands.elements;break;
   case 12:lease.host.channels[1].buffers=lease.host.channels[0].buffers;lease.host.channels[1].buffers_address=lease.host.channels[0].buffers_address;break;
   case 13:lease.host.events[3]=lease.host.events[0];break;
   case 14:lease.host.channels[0].config=0;break;case 15:lease.host.image_address++;break;
  }
  memset(&out,0xa5,sizeof out);assert(op13_mhi_adopt(&out,&lease,newpool,&ops,0,0));assert(!memcmp(before,&out,sizeof out));
 }
 assert(!calls);free(image);free(newpool);free(pool);
 printf("MHI relocation: actual core allocations, snapshot/transfer gate, all DMA/context/ring pointers, overlap/busy/state faults and unchanged failure output passed (host%zu handoff%zu)\n",sizeof out,sizeof lease);
}
