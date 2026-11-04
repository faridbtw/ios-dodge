/* Independent, explicit diagnostics. No resource-ready handoff is produced. */
#include "sm8750_prepare.h"
#include "sm8750_spi_inspect.h"
#include "sm8750_config_query.h"
typedef struct {
 uint32_t revision, pad;
 void *parent,*system_table,*device,*file_path,*reserved;
 uint32_t options_size,pad2;
 void *options,*image_base;
 uint64_t image_size;
 uint32_t code_type,data_type;
 void *unload;
} diag_loaded_image;
_Static_assert(offsetof(diag_loaded_image,image_base)==0x40,"LoadedImage ABI");
static void diag_range(uint64_t addr,uint64_t size)
{
 static uint8_t map[65536];
 uint64_t n=sizeof(map),key=0,ds=0; uint32_t ver=0;
 efi_status st=BS->get_memory_map(&n,map,&key,&ds,&ver);
 print("diag range %x size %x map-status %x\n",addr,size,st);
 if(EFI_ERROR(st)||ds<sizeof(efi_memory_descriptor)||n>sizeof(map)||size>UINT64_MAX-addr) return;
 for(uint64_t off=0;off+ds<=n;off+=ds) {
  efi_memory_descriptor *d=(void *)(map+off);
  if(d->pages>(UINT64_MAX-d->phys_start)/4096) continue;
  uint64_t end=d->phys_start+d->pages*4096;
  if(addr<end && d->phys_start<addr+size)
   print("diag mem type %d phys %x end %x virt %x attr %x\n",(uint64_t)d->type,d->phys_start,end,d->virt_start,d->attribute);
 }
}
static void sm8750_diag(void)
{
 struct sm8750_prepare_report p={0};
 int rc=sm8750_loader_prepare(BS,1,0,&p);
 print("diag prepare rc %x clock-locate %x rev %x spmi-locate %x rev %x\n",(uint64_t)(int64_t)rc,p.clock_locate_status,p.clock_revision,p.spmi_locate_status,p.spmi_revision);
 for(unsigned i=0;i<3;i++) print("diag clock %d status %x id %x hz %d on %d\n",(uint64_t)i,p.clock_status[i],p.clock_id[i],(uint64_t)p.frequency[i],(uint64_t)p.clock_on[i]);
 for(unsigned i=0;i<3;i++) print("diag clock-v2 %d get-id %x get-frequency %x is-on %x\n",(uint64_t)i,p.clock_id_status[i],p.clock_frequency_status[i],p.clock_on_status[i]);
 print("diag spmi result %x bytes %d type %x subtype %x votes %d\n",(uint64_t)p.spmi_result,(uint64_t)p.spmi_bytes,(uint64_t)p.avdd_type_subtype[0],(uint64_t)p.avdd_type_subtype[1],(uint64_t)p.clock_votes);
 for(unsigned i=0;i<8;i++) print("diag GPIO3 %x = %x\n",(uint64_t)(0x8a40+i),(uint64_t)p.avdd_control[i]);
 static const efi_guid li={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
 uint64_t count=0; efi_handle *handles=0; unsigned matches=0;
 efi_status st=BS->locate_handle_buffer(ByProtocol,&li,0,&count,&handles);
 print("diag LoadedImage status %x count %d\n",st,count);
 if(!EFI_ERROR(st)&&handles) for(uint64_t i=0;i<count;i++) {
  diag_loaded_image *im=0;
  if(EFI_ERROR(BS->handle_protocol(handles[i],&li,(void **)&im))||!im) continue;
  struct sm8750_spi_topology s={0};
  rc=sm8750_loader_spi_inspect(BS,1,im->image_base,(size_t)im->image_size,&s);
  if(rc!=0&&rc!=-6) continue;
  matches++;
  print("diag SPI image %x size %x rc %x rev %x init %d logical %d physical-verified %d\n",(uint64_t)(uintptr_t)im->image_base,im->image_size,(uint64_t)(int64_t)rc,s.revision,(uint64_t)s.initialized,(uint64_t)s.logical_instance,(uint64_t)s.physical_address_verified);
  for(unsigned j=0;j<4;j++) print("diag SPI wrapper %d count %d\n",(uint64_t)j,(uint64_t)s.wrapper_counts[j]);
  diag_range((uintptr_t)im->image_base,im->image_size);
 }
 if(handles) BS->free_pool(handles);
 print("diag SPI validated-image-count %d\n",(uint64_t)matches);
 struct sm8750_config_protocol_report c={0};
 rc=sm8750_loader_config_inspect(BS,1,&c);
 print("diag config passive rc %x locate %x interface %x\n",(uint64_t)(int64_t)rc,c.locate_status,c.interface_address);
 if(!rc) {
  static const unsigned slots[]={0,6,20,25,28};
  diag_range(c.interface_address,328);
  for(unsigned i=0;i<5;i++) { unsigned j=slots[i];
   print("diag config slot %x value %x\n",(uint64_t)j*8,c.slots[j]);
   if(c.slots[j]) diag_range(c.slots[j],1);
  }
 }
}
static void sm8750_config_diag(void)
{
 struct sm8750_config_query_report r={0};
 int rc=sm8750_loader_config_query(BS,1,&r);
 print("diag config-query rc %x locate %x core %x common %x counts %d %d logical %d matched-a90000 %d\n",(uint64_t)(int64_t)rc,r.locate_status,r.core_base,r.common_base,(uint64_t)r.count0,(uint64_t)r.count1,(uint64_t)r.logical_instance,(uint64_t)r.matched_a90000);
 print("diag config-v2 se-wrapper %x se-index %d derived %x matched-a90000 %d actual-common %x\n",r.se_wrapper_base,(uint64_t)r.se_index,r.derived_base,(uint64_t)r.derived_matched_a90000,r.wrapper_common_base);
 for(unsigned i=0;i<3;i++) print("diag config-v2 property %d status %x\n",(uint64_t)i,(uint64_t)(int64_t)r.extended_status[i]);
 for(unsigned i=0;i<3;i++) print("diag config path %d status %x\n",(uint64_t)i,(uint64_t)(int64_t)r.path_status[i]);
 for(unsigned i=0;i<4;i++) print("diag config value %d status %x\n",(uint64_t)i,(uint64_t)(int64_t)r.value_status[i]);
}
