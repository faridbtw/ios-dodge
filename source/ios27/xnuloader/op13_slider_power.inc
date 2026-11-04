/* SPDX-License-Identifier: BSD-2-Clause
 * Firmware RPMh vote for the exact DT sensor supply, ldob1=1800mV.
 * ABI checked against this phone's RpmhDxe table@d108 and CmdDbDxe@6098.
 * The firmware retains and aggregates the client vote; cold reset releases it.
 */
struct op13_slider_cmd_db {
 uint64_t revision; void *query;
 efi_status (*address)(void *,const char *,uint32_t *);
};
struct op13_slider_rpmh {
 uint64_t revision; void *init;
 efi_status (*valid)(uint32_t,uint8_t *);
 efi_status (*count)(uint32_t,uint32_t *);
 void *create_internal,*create_explicit;
 efi_status (*create)(uint32_t,const char *,void **);
 void *invalidate,*callback,*destroy,*find,*churn_all,*churn_single,*finish,*finish_sw,*issue_set,*invalidate_sleep,*invalidate_wake,*read;
 efi_status (*issue)(void *,uint32_t,uint8_t,uint32_t,uint32_t,uint32_t *);
};
_Static_assert(__builtin_offsetof(struct op13_slider_rpmh,create)==0x30,"RPMh create ABI");
_Static_assert(__builtin_offsetof(struct op13_slider_rpmh,issue)==0x98,"RPMh issue ABI");
static void *op13_slider_rail_client;
static int op13_slider_rail_ready;
static int op13_slider_power(void)
{
 if(op13_slider_rail_client)return op13_slider_rail_ready?0:-14;
 const efi_guid cg={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
 const efi_guid rg={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
 struct op13_slider_cmd_db *c=0;struct op13_slider_rpmh *p=0;
 efi_status cs=BS->locate_protocol(&cg,0,(void **)&c),rs=BS->locate_protocol(&rg,0,(void **)&p);
 print("op13-slider: CmdDb=%x RPMh=%x revisions=%x %x\n",cs,rs,c?c->revision:0,p?p->revision:0);
 if(EFI_ERROR(cs)||EFI_ERROR(rs)||!c||!p||c->revision!=0x10000||p->revision!=0x10001||
  !c->address||!p->valid||!p->count||!p->create||!p->issue)return -10;
 uint32_t addr=0,count=0;uint8_t valid=0;
 cs=c->address(c,"ldob1",&addr);rs=p->valid(2,&valid);
 efi_status ns=p->count(2,&count);
 print("op13-slider: ldob1 addr=%x status=%x HLOS valid=%d status=%x TCS count=%d status=%x\n",
  (uint64_t)addr,cs,(uint64_t)valid,rs,(uint64_t)count,ns);
 if(EFI_ERROR(cs)||EFI_ERROR(rs)||EFI_ERROR(ns)||!valid||count<3||!addr||(addr&0xf)||((addr>>16)&7)!=4)return -11;
 rs=p->create(2,"ios-dodge-slider",&op13_slider_rail_client);
 print("op13-slider: RPMh client status=%x handle=%x\n",rs,(uint64_t)(uintptr_t)op13_slider_rail_client);
 if(EFI_ERROR(rs)||!op13_slider_rail_client)return -12;
 const uint32_t offsets[]={0,8,4},values[]={1800,7,1};
 for(unsigned i=0;i<3;i++) {
  uint32_t request=0;
  rs=p->issue(op13_slider_rail_client,0,0,addr+offsets[i],values[i],&request);
  print("op13-slider: RPMh active address=%x value=%d status=%x request=%d\n",
   (uint64_t)(addr+offsets[i]),(uint64_t)values[i],rs,(uint64_t)request);
  if(EFI_ERROR(rs))return -13;
 }
 BS->stall(10000);op13_slider_rail_ready=1;
 print("op13-slider: sensor supply vote retained until cold reset\n");return 0;
}
