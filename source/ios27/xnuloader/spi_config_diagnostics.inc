/* Read-only configuration evidence for the pinned SPI descriptor constructor. */
struct spi_diag_node { uint64_t opaque[2]; };
struct spi_diag_cfg {
 uint64_t revision; void *prefix[5];
 int32_t (*path)(struct spi_diag_node *,const char *);
 void *middle[11];
 int32_t (*size)(const struct spi_diag_node *,const char *,uint32_t *);
 int32_t (*string)(const struct spi_diag_node *,const char *,char *,uint32_t);
 int32_t (*u32)(const struct spi_diag_node *,const char *,uint32_t *);
 void *gap[4];
 int32_t (*u8)(const struct spi_diag_node *,const char *,uint8_t *);
 void *gap2[2];
 int32_t (*u16)(const struct spi_diag_node *,const char *,uint16_t *);
};
_Static_assert(offsetof(struct spi_diag_cfg,path)==0x30,"path");
_Static_assert(offsetof(struct spi_diag_cfg,size)==0x90,"size");
_Static_assert(offsetof(struct spi_diag_cfg,u32)==0xa0,"u32");
_Static_assert(offsetof(struct spi_diag_cfg,u8)==0xc8,"u8");
_Static_assert(offsetof(struct spi_diag_cfg,string)==0x98,"string");
_Static_assert(offsetof(struct spi_diag_cfg,u16)==0xe0,"u16");
static void spi_config_string(struct spi_diag_cfg *p,const struct spi_diag_node *node,const char *key)
{
 uint32_t n=0;
 if(!p->size||!p->string)return;
 int32_t rc=p->size(node,key,&n);
 print("spi-config string-size %s rc %x bytes %d\n",key,(uint64_t)(int64_t)rc,(uint64_t)n);
 if(rc||n==0||n>127)return;
 struct {uint64_t pre;char buf[128];uint64_t post;} out={.pre=0x13a55a13,.post=0x13a55a13};
 rc=p->string(node,key,out.buf,n);
 int intact=out.pre==0x13a55a13&&out.post==0x13a55a13;
 out.buf[127]=0;
 print("spi-config string %s rc %x guards %d value %s\n",key,(uint64_t)(int64_t)rc,
  (uint64_t)intact,(!rc&&intact)?out.buf:"<unavailable>");
}
static void spi_configuration_diag(void)
{
 const efi_guid hw={0x9e61de7c,0x9927,0x4f28,{0x9f,0x3d,0x32,0x07,0x05,0xd8,0x18,0x01}};
 void *hwio=0;
 efi_status hw_status=BS->locate_protocol(&hw,0,&hwio);
 print("spi-config HWIO locate %x interface %x\n",hw_status,(uint64_t)(uintptr_t)hwio);
 if(!EFI_ERROR(hw_status)&&hwio){
  const volatile uint64_t *slots=hwio;
  print("spi-config HWIO raw-slot0 %x map-slot10 %x\n",slots[0],slots[2]);
 }
 const efi_guid g={0x0389b776,0x625f,0x11eb,{0x83,0xbe,0xc7,0x41,0xa9,0x13,0xde,0x34}};
 static const char *paths[]={"/soc/TOP_QUP_1","/soc/TOP_QUP_1/TOP_QUP_1_SE_4"};
 static const char *wrapper32[]={"core_base_addr","common_base_addr","se_wrapper_base_addr","core_frequency","qup_flags"};
 static const char *child8[]={"se_index","protocol_supported","interface_supported","gpi_index","i2c_hub","i2c_mm"};
 struct spi_diag_cfg *p=0;
 efi_status e=BS->locate_protocol(&g,0,(void **)&p);
 print("spi-config locate %x\n",e);
 if(EFI_ERROR(e)||!p||p->revision!=0x10003||!p->path||!p->u8||!p->u32)return;
 for(unsigned i=0;i<2;i++) {
  struct spi_diag_node node={0};
  int32_t rc=p->path(&node,paths[i]);
  print("spi-config path %d %s rc %x\n",(uint64_t)i,paths[i],(uint64_t)(int64_t)rc);
  if(rc)continue;
  print("spi-config opaque-node %d %x %x\n",(uint64_t)i,node.opaque[0],node.opaque[1]);
  spi_config_string(p,&node,"status");
  if(i==0){
   uint8_t q=0;rc=p->u8(&node,"qup_id",&q);
   print("spi-config u8 qup_id rc %x value %x\n",(uint64_t)(int64_t)rc,(uint64_t)q);
   for(unsigned j=0;j<5;j++){
    uint32_t v=0;rc=p->u32(&node,wrapper32[j],&v);
    print("spi-config u32 %s rc %x value %x\n",wrapper32[j],(uint64_t)(int64_t)rc,(uint64_t)v);
   }
  }else{
   spi_config_string(p,&node,"se_clock");
   if(p->u16){uint16_t irq=0;rc=p->u16(&node,"core_irq",&irq);
    print("spi-config u16 core_irq rc %x value %x\n",(uint64_t)(int64_t)rc,(uint64_t)irq);}
   uint32_t flags=0;rc=p->u32(&node,"se_flags",&flags);
   print("spi-config u32 se_flags rc %x value %x\n",(uint64_t)(int64_t)rc,(uint64_t)flags);
   for(unsigned j=0;j<6;j++){
    uint8_t v=0;rc=p->u8(&node,child8[j],&v);
    print("spi-config u8 %s rc %x value %x\n",child8[j],(uint64_t)(int64_t)rc,(uint64_t)v);
   }
  }
 }
}
