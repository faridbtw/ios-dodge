#include "op13_wifi_rails.h"
static void *op13_wifi_rail_client;
static unsigned op13_wifi_rails_ready;
static int op13_wifi_rails_vote(void)
{
 if(op13_wifi_rail_client)return op13_wifi_rails_ready?0:-1;
 const efi_guid cg={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
 const efi_guid rg={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
 struct op13_slider_cmd_db *c=0;struct op13_slider_rpmh *p=0;
 efi_status cs=BS->locate_protocol(&cg,0,(void **)&c),rs=BS->locate_protocol(&rg,0,(void **)&p);
 if(EFI_ERROR(cs)||EFI_ERROR(rs)||!c||!p||c->revision!=0x10000||p->revision!=0x10001||!c->address||!p->valid||!p->count||!p->create||!p->issue)return -2;
 uint32_t address[6]={0},voltage[6]={0},count=0;uint8_t valid=0;
 if(EFI_ERROR(p->valid(2,&valid))||!valid||EFI_ERROR(p->count(2,&count))||count<3)return -3;
 for(unsigned i=0;i<6;i++){
  cs=c->address(c,op13_wifi_rails[i].name,&address[i]);
  print("op13-wifi-rails: %s address=%x query=%x\n",op13_wifi_rails[i].name,(uint64_t)address[i],cs);
  if(EFI_ERROR(cs))return -4;
 }
 if(op13_wifi_rail_plan(address,voltage))return -5;
 rs=p->create(2,"ios-dodge-wifi-rails",&op13_wifi_rail_client);
 if(EFI_ERROR(rs)||!op13_wifi_rail_client)return -6;
 /* Native Linux power.c sets voltage then enables each regulator. No mode
  * writes here. Independent client votes aggregate with existing clients. */
 for(unsigned i=0;i<6;i++)for(unsigned step=0;step<2;step++){
  uint32_t request=0,value=step?1:voltage[i];
  rs=p->issue(op13_wifi_rail_client,0,0,address[i]+(step?4:0),value,&request);
  print("op13-wifi-rails: %s %s value=%d status=%x request=%d\n",op13_wifi_rails[i].name,step?"enable":"mV",(uint64_t)value,rs,(uint64_t)request);
  if(EFI_ERROR(rs))return -7;
 }
 BS->stall(10000);op13_wifi_rails_ready=1;
 print("op13-wifi-rails: six native supply votes retained until cold reset; no storage writes\n");return 0;
}
