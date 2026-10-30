#include "op13_mhi_handoff.h"
_Static_assert(sizeof(struct mhi_host)==840,"MHI host ARM64 ABI changed");
_Static_assert(sizeof(struct op13_mhi_handoff)==936,"MHI handoff ARM64 ABI changed");
static struct op13_mhi_handoff op13_wifi_handoff_snapshot;
static int op13_wifi_handoff_export(void)
{
 struct mhi_host *h=&op13_wifi_mhi_host;
 if(!op13_wifi_fw_init_done||!op13_wifi_dma_mapped||!op13_wifi_cap_received||
    op13_wifi_radio_requested||op13_wifi_wmi_endpoint||op13_wifi_mhi_stopped||
    h->failed||!h->powered||h->ee!=MHI_HOST_EE_AMSS||h->state!=MHI_HOST_STATE_M0||op13_wifi_qrtr_ack_pending)return -1;
 struct op13_mhi_handoff *s=&op13_wifi_handoff_snapshot;memset(s,0,sizeof(*s));
 s->magic=OP13_MHI_HANDOFF_MAGIC;s->version=1;s->bytes=sizeof(*s);s->host_bytes=sizeof(*h);s->flags=OP13_MHI_SNAPSHOT;
 s->source_host_pa=(uint64_t)(uintptr_t)h;s->pool_pa=op13_wifi_dma_pool;s->pool_bytes=OP13_WIFI_DMA_BYTES;s->iova=OP13_WIFI_IOVA;
 s->bar0=0x40400000;s->bar0_bytes=0x200000;s->tables_pa=op13_wifi_dma_tables;
 s->smmu_context=op13_wifi_dma_plan.context;s->smmu_slot=op13_wifi_dma_plan.slot;s->wlfw_node=op13_wifi_wlfw_node;s->wlfw_port=op13_wifi_wlfw_port;
 __asm__ volatile("dsb sy":::"memory");s->host=*h;
 struct mhi_host relocated;int rc=op13_mhi_adopt(&relocated,s,(void *)(uintptr_t)op13_wifi_dma_pool,h->ops,h->context,1);
 print("op13-wifi-handoff: snapshot bytes=%d validated=%d transfer=0 FW_INIT_DONE=%d; no kernel launched\n",(uint64_t)sizeof(*s),(uint64_t)(!rc),(uint64_t)op13_wifi_fw_init_done);
 if(rc)return rc;
 staged_data=(const char*)s;staged_len=sizeof(*s);return 0;
}
