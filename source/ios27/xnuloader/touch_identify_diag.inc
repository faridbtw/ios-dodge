static void bitbang_diag(void);
/* Separate explicit loader transaction; native XNU readiness is never asserted. */
#include "sm8750_first_read.h"
#include "panel-prep/sm8750_panel.h"
static struct sm8750_panel identify_panel;
static struct sm8750_first_read_session identify_session;
static struct sm8750_first_read_report identify_report;

static int identify_prepare(void *cookie)
{
 if(!tlmm_config_diag())return -3;
 const uint64_t pages[2]={0xf1a1000,0xf1a2000};
 for(unsigned i=0;i<2;i++) {
  uint64_t par=uefi_page_translation(pages[i]),write_par;
  __asm__ volatile("at s1e1w, %1\nisb\nmrs %0, par_el1" : "=r"(write_par) : "r"(pages[i]) : "memory");
  print("touch-identify TLMM page %x read-PAR %x write-PAR %x\n",pages[i],par,write_par);
  if((par&1)||(write_par&1)||(par&0x0000fffffffff000ULL)!=pages[i]||
     (write_par&0x0000fffffffff000ULL)!=pages[i]||((par>>56)&0xf0)) return -3;
 }
 const struct sm8750_panel_io io={0,sm8750_panel_raw_read,sm8750_panel_raw_write};
 return sm8750_panel_prepare(cookie,BS,&io,1);
}
static int identify_release(void *cookie) { return sm8750_panel_release(cookie); }
static void touch_identify_diag(void)
{
 if(identify_session.limit==1){op13_trace(1,0);sm8750_diag();spi_configuration_diag();if(tlmm_config_diag())tlmm_gpio_snapshot();geni_preflight_diag();op13_trace(1,1);return;}
 if(identify_session.limit>=2){bitbang_diag();return;}
 static const efi_guid li={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
 uint64_t count=0;efi_handle *handles=0;void *image=0;uint64_t image_size=0;
 #define r identify_report
 efi_status status=BS->locate_handle_buffer(ByProtocol,&li,0,&count,&handles);
 if(!EFI_ERROR(status)&&handles) for(uint64_t i=0;i<count;i++) {
  diag_loaded_image *im=0;struct sm8750_spi_topology s={0};
  if(EFI_ERROR(BS->handle_protocol(handles[i],&li,(void **)&im))||!im)continue;
  int rc=sm8750_loader_spi_inspect(BS,1,im->image_base,(size_t)im->image_size,&s);
  if(rc==0||rc==-6) {image=im->image_base;image_size=im->image_size;break;}
 }
 if(handles)BS->free_pool(handles);
 print("touch-identify start image %x size %x live %d\n",(uint64_t)(uintptr_t)image,image_size,(uint64_t)(running_live!=0));
 int rc=sm8750_first_read(BS,1,running_live!=0,image,(size_t)image_size,
  identify_prepare,identify_release,&identify_panel,&identify_session,&r);
 print("touch-identify rc %x image-status %x topology %x flags-status %x flags %x instance %d\n",
  (uint64_t)(int64_t)rc,(uint64_t)(int64_t)r.image_status,(uint64_t)(int64_t)r.topology_status,
  (uint64_t)(int64_t)r.flags_status,(uint64_t)r.se_flags,(uint64_t)r.instance);
 print("touch-identify panel %x stage %d error %x cleanup %x prepared %d pins-owned %d avdd-owned %d\n",
  (uint64_t)(int64_t)r.panel_status,(uint64_t)identify_panel.stage,(uint64_t)(int64_t)identify_panel.error,
  (uint64_t)(int64_t)identify_panel.cleanup_error,(uint64_t)identify_panel.prepared,
  (uint64_t)identify_panel.pins_owned,(uint64_t)identify_panel.avdd.owned);
 print("touch-identify NPA rev %x stage %d status %x clients %x %x %x\n",
  identify_panel.power.npa_revision,(uint64_t)identify_panel.power.stage,identify_panel.power.status,
  (uint64_t)(uintptr_t)identify_panel.power.npa_clients[0],(uint64_t)(uintptr_t)identify_panel.power.npa_clients[1],(uint64_t)(uintptr_t)identify_panel.power.npa_clients[2]);
 for(unsigned i=0;i<2;i++)print("touch-identify GPIO %d saved %x %x observed %x %x\n",(uint64_t)(161+i),
  (uint64_t)identify_panel.saved_cfg[i],(uint64_t)identify_panel.saved_io[i],
  (uint64_t)identify_panel.observed_cfg[i],(uint64_t)identify_panel.observed_io[i]);
 print("touch-identify SPI open %x read0 %x read1 %x close %x release %x header-valid %d handle %x blocked %d\n",
  (uint64_t)r.open_status,(uint64_t)r.read_status[0],(uint64_t)r.read_status[1],(uint64_t)r.close_status,
  (uint64_t)(int64_t)r.release_status,(uint64_t)r.header_valid,(uint64_t)(uintptr_t)identify_session.handle,(uint64_t)identify_session.blocked);
 print("touch-identify packet transfer-complete %d frame-valid %d identify-valid %d status %x\n",
  (uint64_t)r.raw_transfer_complete,(uint64_t)r.frame_valid,(uint64_t)r.identify_valid,
  (uint64_t)(int64_t)r.packet_status);
 for(unsigned i=0;i<4;i++)print("touch-identify header %d %x\n",(uint64_t)i,(uint64_t)r.header[i]);
 for(unsigned i=0;i<31;i++)print("touch-identify continuation %d %x\n",(uint64_t)i,(uint64_t)r.continuation[i]);
}

#undef r
