/* SPDX-License-Identifier: BSD-2-Clause
 * Versioned ARM64 ownership snapshot. No firmware code pointers are executed.
 * Snapshot exports are diagnostic only; a transfer requires a quiesced owner. */
#ifndef OP13_MHI_HANDOFF_H
#define OP13_MHI_HANDOFF_H
#include "mhi_host.h"
#define OP13_MHI_HANDOFF_MAGIC UINT64_C(0x4f50313357494649)
#define OP13_MHI_SNAPSHOT 1u
#define OP13_MHI_TRANSFER 2u
struct op13_mhi_handoff {
 uint64_t magic;
 uint32_t version,bytes,host_bytes,flags;
 uint64_t source_host_pa,pool_pa,pool_bytes,iova,bar0,bar0_bytes,tables_pa;
 uint32_t smmu_context,smmu_slot,wlfw_node,wlfw_port;
 struct mhi_host host;
};
struct op13_mhi_region {uint64_t first,size;};
struct op13_mhi_relocation {
 const struct op13_mhi_handoff *lease;
 uintptr_t mapped;
 unsigned count;
 struct op13_mhi_region regions[20];
};
static int op13_mhi_relocate(struct op13_mhi_relocation *r,uint8_t **ptr,uint64_t dma,uint64_t bytes)
{
 /* MHI allocations remain inside their original16MiB arena even when the
  * enclosing DMA mapping also reserves a separate Linux datapath pool. */
 uint64_t p=(uint64_t)(uintptr_t)*ptr,base=r->lease->pool_pa,extent=0x1000000;
 if(!p||!bytes||bytes>extent||p<base||p-base>extent-bytes||
    dma!=r->lease->iova+(p-base)||r->count>=20)return -1;
 for(unsigned i=0;i<r->count;i++){
  uint64_t a=r->regions[i].first,n=r->regions[i].size;
  if(p<a+n&&a<p+bytes)return -2;
 }
 r->regions[r->count++]=(struct op13_mhi_region){p,bytes};
 *ptr=(uint8_t *)(r->mapped+(uintptr_t)(p-base));return 0;
}
static int op13_mhi_relocate_ring(struct op13_mhi_relocation *r,struct mhi_host_ring *ring)
{
 if(!ring->elements||(ring->elements&(ring->elements-1))||ring->elements>4096||
    ring->rp>=ring->elements||ring->wp>=ring->elements)return -1;
 return op13_mhi_relocate(r,&ring->base,ring->address,(uint64_t)ring->elements*MHI_ELEMENT);
}
/* allow_snapshot is for offline relocation validation only. A kernel must pass0.
 * This function only validates/copies metadata. It performs no allocation, MMIO,
 * device reset, callback or DMA write. Destination is unchanged on failure. */
static int op13_mhi_adopt(struct mhi_host *out,const struct op13_mhi_handoff *in,
 void *mapped_pool,const struct mhi_host_ops *ops,void *context,int allow_snapshot)
{
 if(!out||!in||!mapped_pool||!ops||!ops->read||!ops->write||!ops->dma_alloc||
    !ops->dma_free||!ops->nanoseconds||!ops->lock||!ops->unlock||!ops->pause||
    !ops->random||!ops->say||!ops->status||!ops->receive)return -1;
 if(in->magic!=OP13_MHI_HANDOFF_MAGIC||in->version!=1||in->bytes!=sizeof(*in)||
    in->host_bytes!=sizeof(in->host)||
    (in->flags!=OP13_MHI_TRANSFER&&!(allow_snapshot&&in->flags==OP13_MHI_SNAPSHOT)))return -2;
 if(in->bar0!=UINT64_C(0x40400000)||in->bar0_bytes!=0x200000||
    in->iova!=0x18000000||(in->pool_bytes!=0x4000000&&in->pool_bytes!=0x8000000)||
    in->pool_pa<0x80000000||(in->pool_pa&(in->pool_bytes-1))||in->pool_pa>=(UINT64_C(1)<<36)||
    in->pool_bytes>(UINT64_C(1)<<36)-in->pool_pa||
    (uintptr_t)mapped_pool>UINTPTR_MAX-in->pool_bytes||
    in->source_host_pa<0x80000000||in->source_host_pa>=(UINT64_C(1)<<36)||in->source_host_pa>UINT64_MAX-sizeof(in->host)||
    in->smmu_context>=128||in->smmu_slot>=256||in->tables_pa<0x80000000||in->tables_pa>=(UINT64_C(1)<<36)||(in->tables_pa&4095)||
    in->wlfw_node!=7||in->wlfw_port!=1)return -3;
 uint64_t table_bytes=(2+in->pool_bytes/0x200000)*4096;
 if(table_bytes>(UINT64_C(1)<<36)-in->tables_pa||
    (in->tables_pa<in->pool_pa+in->pool_bytes&&in->pool_pa<in->tables_pa+table_bytes))return -3;
 struct mhi_host next=in->host;
 struct mhi_host_config *cfg=&next.config;
 if(!next.allocated||!next.powered||!next.mission||next.failed||
    next.ee!=MHI_HOST_EE_AMSS||next.state!=MHI_HOST_STATE_M0||
    cfg->max_channels!=72||cfg->channel_count!=2||cfg->event_count!=4||
    cfg->offload_event_mask!=(1u<<3)||cfg->buffer_length!=0x8000||
    cfg->iova_start!=0x18000000||cfg->iova_stop!=0xb0000000||
    cfg->image_offset!=0x80000||!cfg->image_length||
    cfg->image_length>next.image_size||cfg->image_offset>=cfg->image_length)return -4;
 if(next.bhi!=0x100||next.bhie!=0x224||next.channel_doorbells!=0x300||next.event_doorbells!=0x700)return -5;
 struct op13_mhi_relocation r={in,(uintptr_t)mapped_pool,0,{{0,0}}};
 if(op13_mhi_relocate(&r,&next.channel_contexts,next.channel_contexts_address,(uint64_t)cfg->max_channels*MHI_CONTEXT)||
    op13_mhi_relocate(&r,&next.event_contexts,next.event_contexts_address,(uint64_t)cfg->event_count*MHI_CONTEXT)||
    op13_mhi_relocate(&r,&next.command_context,next.command_context_address,MHI_CONTEXT)||
    op13_mhi_relocate_ring(&r,&next.commands))return -6;
 for(unsigned i=0;i<cfg->event_count;i++){
  if(next.event_busy[i]||next.event_again[i])return -7;
  if(cfg->offload_event_mask&(1u<<i)){
   if(next.events[i].base||next.events[i].address||next.events[i].elements)return -8;
  }else if(next.events[i].elements!=cfg->events[i].elements||op13_mhi_relocate_ring(&r,&next.events[i]))return -9;
 }
 for(unsigned i=0;i<cfg->channel_count;i++){
  struct mhi_host_channel *ch=&next.channels[i];
  uint64_t expected=in->source_host_pa+offsetof(struct mhi_host,config)+offsetof(struct mhi_host_config,channels)+i*sizeof(cfg->channels[0]);
  if((uint64_t)(uintptr_t)ch->config!=expected||cfg->channels[i].number!=20+i||
     cfg->channels[i].event_ring!=1||cfg->channels[i].inbound!=(i==1)||
     cfg->channels[i].elements!=64||cfg->channels[i].ee_mask!=4||
     !ch->running||ch->busy||ch->ring.elements!=64||!ch->buffer_length||ch->buffer_length>0x8000||
     op13_mhi_relocate_ring(&r,&ch->ring)||
     op13_mhi_relocate(&r,&ch->buffers,ch->buffers_address,(uint64_t)ch->buffer_length*ch->ring.elements))return -10;
  ch->config=&out->config.channels[i];
 }
 if(op13_mhi_relocate(&r,&next.image,next.image_address,next.image_size)||
    op13_mhi_relocate(&r,&next.vectors,next.vectors_address,next.vectors_size))return -11;
 next.config.image=next.image; /* never retain a pointer into the old USB staging area */
 next.ops=ops;next.context=context;
 *out=next;return 0;
}
#endif
