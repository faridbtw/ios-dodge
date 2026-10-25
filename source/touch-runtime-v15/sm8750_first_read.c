/* SPDX-License-Identifier: BSD-2-Clause */
#include "sm8750_first_read.h"
#include "trace.h"
#include "sm8750_config_query.h"
#include "sm8750_spi_inspect.h"
#include "startup_packet.h"
/* Public SpiDeviceInfoType layout; independently matched at SPI RVA27c0..2864.
 * Enum values below: normal clock0, idle-low0, input-first0, CS-low0,
 * CS-deassert0, master1, loopback0. No legacy instance enum is used. */
struct first_config {
 uint32_t clock_mode,polarity,phase,deassert_ns,min_hz,max_hz,cs_polarity,cs_mode;
 uint8_t hs,pad[3];
 uint32_t slave,core_mode,bits,loopback;
};
_Static_assert(sizeof(struct first_config)==52,"SPI config size");
_Static_assert(offsetof(struct first_config,max_hz)==0x14,"frequency ABI");
_Static_assert(offsetof(struct first_config,slave)==0x24,"CS ABI");
_Static_assert(offsetof(struct first_config,bits)==0x2c,"bits ABI");
struct first_spi {uint64_t revision;uint32_t (*open)(uint32_t,void **);
 uint32_t (*transfer)(void *,const struct first_config *,const uint8_t *,uint32_t,uint8_t *,uint32_t);
 uint32_t (*close)(void *);};
struct flag_node {uint64_t token[2];};
struct flag_proto {void *prefix[6];int32_t (*path)(struct flag_node *,const char *);
 void *middle[13];int32_t (*u32)(const struct flag_node *,const char *,uint32_t *);};
int sm8750_first_read(efi_boot_services *bs,int enabled,int live,const void *image,
 size_t size,int (*prepare)(void *),int (*release)(void *),void *panel,
 struct sm8750_first_read_session *session,struct sm8750_first_read_report *r)
{
 static const efi_guid guid={0x4c7ffd28,0x6a06,0x4425,{0x9e,0xe2,0x67,0x6e,0xbc,0x08,0x96,0x83}};
 struct first_spi *spi=0;struct sm8750_config_query_report cfg={0};
 struct sm8750_spi_topology topo={0};void *handle=0;uint8_t tx[31];
 const struct first_config conf={.max_hz=1000000,.core_mode=1,.bits=8};
 if(!bs||!r||!session)return -5;
 if(session->blocked||session->handle||session->panel_owned)return -6;
 *r=(struct sm8750_first_read_report){0};
 r->flags_status=r->image_status=r->topology_status=r->panel_status=r->release_status=INT32_MIN;
 r->packet_status=INT32_MIN;
 r->open_status=r->read_status[0]=r->read_status[1]=r->close_status=UINT32_MAX;
 if(session->limit<1||session->limit>5)return -5;
 op13_trace(1,session->limit);
 if(!enabled||live||!prepare||!release)return -3;
 op13_trace(30,0);
 r->image_status=sm8750_loader_spi_inspect(bs,1,image,size,&topo);
 if(r->image_status!=0&&r->image_status!=-6)return -3;
 op13_trace(30,1);
 r->topology_status=sm8750_loader_config_query(bs,1,&cfg);
 /* Exact live-observed topology; missing optional child core_base is allowed
  * only because the independently proven wrapper/index computation matches. */
 if(cfg.path_status[0]||cfg.path_status[1]||cfg.path_status[2]||
    cfg.value_status[0]||cfg.value_status[1]||cfg.extended_status[0]||
    cfg.extended_status[1]||cfg.extended_status[2]||cfg.count0!=10||cfg.count1!=8||
    cfg.se_wrapper_base!=0xa80000||cfg.se_index!=4||
    cfg.derived_base!=0xa90000||!cfg.derived_matched_a90000||
    cfg.wrapper_common_base!=0xac0000)return -3;
 /* SPI5534 reads child se_flags u32. Bit3 selects synchronous polling at30f4.
  * Reuse only the independently proven config ABI, no private SPI calls. */
 op13_trace(30,2);
 struct flag_node node={0};struct flag_proto *fp=0;
 static const efi_guid fg={0x0389b776,0x625f,0x11eb,{0x83,0xbe,0xc7,0x41,0xa9,0x13,0xde,0x34}};
 if(EFI_ERROR(bs->locate_protocol(&fg,0,(void **)&fp))||!fp||!fp->path||!fp->u32)return -3;
 op13_trace(30,3);
 r->flags_status=fp->path(&node,"/soc/TOP_QUP_1/TOP_QUP_1_SE_4");
 if(r->flags_status)return -3;
 op13_trace(30,4);
 r->flags_status=fp->u32(&node,"se_flags",&r->se_flags);
 if(r->flags_status||!(r->se_flags&8))return -3;
 op13_trace(30,5);
 r->instance=(uint32_t)cfg.count0+5;
 if(EFI_ERROR(bs->locate_protocol(&guid,0,(void **)&spi))||!spi||spi->revision!=0x10000||
    !spi->open||!spi->transfer||!spi->close)return -3;
 if(session->limit==1)return 0;
 op13_trace(2,0);
 session->panel_owned=1;
 r->panel_status=prepare(panel);
 if(r->panel_status){r->release_status=release(panel);if(!r->release_status)session->panel_owned=0;else session->blocked=1;return -1;}
 int rc=-1;
 if(session->limit==2){rc=0;goto done;}
 op13_trace(3,0);
 r->open_status=spi->open(r->instance,&handle);
 session->handle=handle;session->close_owned=spi->close;
 op13_trace(3,1);
 if(r->open_status||!handle)goto done;
 if(session->limit==3){rc=0;goto done;}
 op13_trace(4,0);
 for(unsigned i=0;i<sizeof(tx);i++)tx[i]=0xff;
 r->read_status[0]=spi->transfer(handle,&conf,tx,4,r->header,4);
 op13_trace(4,1);
 if(r->read_status[0])goto done;
 if(session->limit==4){rc=0;goto done;}
 if(r->header[0]!=0xa5||r->header[1]!=0x10||r->header[2]!=24||r->header[3]){rc=-6;goto done;}
 r->header_valid=1;
 op13_trace(5,0);
 r->read_status[1]=spi->transfer(handle,&conf,tx,31,r->continuation,31);
 op13_trace(5,1);
 if(!r->read_status[1]){
  unsigned frame=0;
  r->raw_transfer_complete=1;
  r->packet_status=startup_packet_validate(r->header,sizeof(r->header),
    r->continuation,sizeof(r->continuation),&frame);
  r->frame_valid=frame;
  r->identify_valid=r->packet_status==0;
  rc=r->packet_status;
 }
 done:
 /* open may return an error AND a context; preserve/close any returned handle.
  * Close failure prevents a claimed clean result and is explicitly reported. */
 if(handle){op13_trace(6,0);r->close_status=spi->close(handle);op13_trace(6,1);if(r->close_status){session->blocked=1;return -1;}session->handle=0;}
 op13_trace(7,0);
 r->release_status=release(panel);op13_trace(7,1);if(r->release_status){session->blocked=1;rc=-1;}else session->panel_owned=0;
 return rc;
}

int sm8750_first_read_cleanup(efi_boot_services *bs,int (*release)(void *),void *panel,
 struct sm8750_first_read_session *s,struct sm8750_first_read_report *r)
{
 static const efi_guid g={0x4c7ffd28,0x6a06,0x4425,{0x9e,0xe2,0x67,0x6e,0xbc,0x08,0x96,0x83}};
 struct first_spi *p=0;
 if(!bs||!release||!s||!r)return -5;
 if(s->handle){
  if(EFI_ERROR(bs->locate_protocol(&g,0,(void **)&p))||!p||p->revision!=0x10000||!p->close)return -3;
  if(!s->close_owned||p->close!=s->close_owned)return -3;
  r->close_status=s->close_owned(s->handle);if(r->close_status)return -1;s->handle=0;
 }
 if(s->panel_owned){r->release_status=release(panel);if(r->release_status)return -1;s->panel_owned=0;}
 s->blocked=0;return 0;
}
