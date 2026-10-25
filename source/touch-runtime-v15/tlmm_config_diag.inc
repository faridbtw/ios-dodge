/* SPDX-License-Identifier: BSD-2-Clause */
/* Pure configuration getters; NO TLMM register/protocol hardware access.
 * ABI is the same pinned config ABI already used by sm8750_config_query.c. */
static int tlmm_config_diag(void)
{
 struct tlmm_node { uint64_t opaque[2]; };
 struct tlmm_cfg_protocol {
  uint64_t revision; void *prefix[5];
  int32_t (*path)(struct tlmm_node *,const char *);
  void *middle[11];
  int32_t (*size)(const struct tlmm_node *,const char *,uint32_t *);
  void *gap;
  int32_t (*u32)(const struct tlmm_node *,const char *,uint32_t *);
  int32_t (*array)(const struct tlmm_node *,const char *,uint32_t *,uint32_t);
 };
 _Static_assert(offsetof(struct tlmm_cfg_protocol,path)==0x30,"config path ABI");
 _Static_assert(offsetof(struct tlmm_cfg_protocol,size)==0x90,"config size ABI");
 _Static_assert(offsetof(struct tlmm_cfg_protocol,array)==0xa8,"config array ABI");
 _Static_assert(offsetof(struct tlmm_cfg_protocol,u32)==0xa0,"config scalar ABI");
 const efi_guid g={0x0389b776,0x625f,0x11eb,{0x83,0xbe,0xc7,0x41,0xa9,0x13,0xde,0x34}};
 static const char *const paths[]={"/soc/pinctrl@f100000","/soc/pinctrl@f000000"};
 static const char *const props[]={"ngpios","width","version","reg"};
 struct tlmm_cfg_protocol *p=0;
 efi_status e=BS->locate_protocol(&g,0,(void **)&p);
 print("tlmm-config locate %x\n",e);
 if(EFI_ERROR(e)||!p)return 0;
 print("tlmm-config revision %x\n",p->revision);
 if(p->revision!=0x10003||!p->path||!p->u32||!p->size||!p->array)return 0;
 int verified=0;
 for(unsigned i=0;i<2;i++) {
  struct tlmm_node n={0};
  int32_t rc=p->path(&n,paths[i]);
  print("tlmm-config path %d %s rc %x\n",(uint64_t)i,paths[i],(uint64_t)(int64_t)rc);
  if(rc)continue;
  uint32_t scalars[3]={0};int scalars_ok=1;
  for(unsigned j=0;j<4;j++) {
   uint32_t v=0;
   rc=p->u32(&n,props[j],&v);
   if(j<3){scalars[j]=v;if(rc)scalars_ok=0;}
   print("tlmm-config property %d %s rc %x raw-u32 %x\n",(uint64_t)i,props[j],(uint64_t)(int64_t)rc,(uint64_t)v);
  }
  /* Pinned DALTLMM call3b44..3b84: size(node,"reg",&u32) via90,
   * array(node,"reg",buffer,count) viaa8. Bound both possible byte/count
   * conventions conservatively; dump words without inferring a physical base. */
  uint32_t count=0;
  rc=p->size(&n,"reg",&count);
  print("tlmm-config reg-size rc %x raw-count %d\n",(uint64_t)(int64_t)rc,(uint64_t)count);
  if(!rc && count>0 && count<=16) {
   struct {uint64_t before;uint32_t words[64];uint64_t after;} buf={.before=0x13a55a13a55a13ULL,.after=0x13a55a13a55a13ULL};
   rc=p->array(&n,"reg",buf.words,count);
   int intact=buf.before==0x13a55a13a55a13ULL && buf.after==0x13a55a13a55a13ULL;
   print("tlmm-config reg-array rc %x guards %d\n",(uint64_t)(int64_t)rc,(uint64_t)intact);
   if(!rc&&intact&&i==0&&count==8&&buf.words[0]==0xf100000&&buf.words[1]==0x100000&&
      scalars_ok&&scalars[0]==215&&scalars[1]==0x1000&&scalars[2]==1)verified=1;
   if(!rc&&intact)for(unsigned k=0;k<16;k++)print("tlmm-config reg-word %d %x\n",(uint64_t)k,(uint64_t)buf.words[k]);
  }
 }
 print("tlmm-config verified-gpio-bank %d\n",(uint64_t)verified);
 return verified;
}
/* Explicit stage1 snapshot: four reads only, separate from power/panel lifecycle. */
static void tlmm_gpio_snapshot(void)
{
 const uint64_t pages[]={0xf1a1000,0xf1a2000};
 for(unsigned i=0;i<2;i++) {
  uint64_t par=uefi_page_translation(pages[i]);
  print("tlmm-snapshot page %x read-PAR %x\n",pages[i],par);
  if((par&1)||(par&0x0000fffffffff000ULL)!=pages[i]||((par>>56)&0xf0))return;
  for(unsigned j=0;j<2;j++) {
   op13_trace(28,4*i+2*j);
   __asm__ volatile("dsb sy" ::: "memory");
   uint32_t v=*(volatile uint32_t *)(uintptr_t)(pages[i]+j*4);
   __asm__ volatile("dsb sy" ::: "memory");
   op13_trace(28,4*i+2*j+1);
   print("tlmm-snapshot pin %d reg %d value %x\n",(uint64_t)(161+i),(uint64_t)j*4,(uint64_t)v);
  }
 }
}
