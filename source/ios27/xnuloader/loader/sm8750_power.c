#include "trace.h"
#include "trace.h"
/* SPDX-License-Identifier: BSD-2-Clause
 * Pinned NpaDxe v1.3 ABI recovered from its protocol table at RVA0x110b8.
 * These functions have NO This parameter (unlike Clock/SPMI protocols).
 * create_sync_client_ex wrapper0x1470 forwards x0..x4 and writes handle to x5.
 * destroy0x149c -> 0x391c; issue_required0x14cc ->0x33cc.
 * PmicDxe touch_screen dependencies at0x3d178 use these exact resource names,
 * client type0x40, and mode/mV/en table at0x37068 includes {7,1800,1}.
 */
#include "sm8750_power.h"
struct npa_protocol {
 uint64_t revision;
 void *init;
 efi_status (*create)(const char *,const char *,uint32_t,uint32_t,void *,void **);
 efi_status (*destroy)(void *);
 void *define_node;
 efi_status (*issue_required)(void *,uint32_t);
};
static const efi_guid npa_guid={0x79d6c870,0x725e,0x489e,{0xa0,0xa1,0x27,0xe7,0xa5,0xd0,0xcb,0x35}};
static const char *const resources[3]={"/pm/ldob4/mode","/pm/ldob4/mV","/pm/ldob4/en"};
static const uint32_t values[3]={7,1800,1};
static struct npa_protocol *locate(efi_boot_services *bs)
{
 struct npa_protocol *p=0;
 if(!bs||EFI_ERROR(bs->locate_protocol(&npa_guid,0,(void **)&p))||!p||
    p->revision!=0x10003||!p->create||!p->destroy||!p->issue_required) return 0;
 return p;
}
void sm8750_loader_power_release(efi_boot_services *bs,struct sm8750_power_session *s)
{
 struct npa_protocol *p=locate(bs);
 if(!p||!s) return;
 for(int i=2;i>=0;i--) if(s->npa_clients[i]) {
  op13_trace(33,(uint32_t)i*2);s->status=p->destroy(s->npa_clients[i]);op13_trace(33,(uint32_t)i*2+1);
  if(!EFI_ERROR(s->status)) s->npa_clients[i]=0;
 }
}
int sm8750_loader_power_vote(efi_boot_services *bs,int enabled,struct sm8750_power_session *s)
{
 struct npa_protocol *p;
 if(!s||!bs) return -5;
 if(!enabled) return -3;
 for(unsigned i=0;i<3;i++) if(s->npa_clients[i]) return -6;
 p=locate(bs); if(!p) return -3;
 s->npa_revision=p->revision;
 /* Allocate every dependency first; a missing resource must not enable VDD. */
 for(unsigned i=0;i<3;i++) {
  s->stage=1+i;
  op13_trace(10+i,0);op13_trace(31,i*2);s->status=p->create(resources[i],"hackios-s3910",0x40,0,0,&s->npa_clients[i]);op13_trace(31,i*2+1);
  op13_trace(10+i,1);if(EFI_ERROR(s->status)||!s->npa_clients[i]) goto fail;
 }
 for(unsigned i=0;i<3;i++) {
  s->stage=4+i; op13_trace(13+i,0);op13_trace(32,i*2);s->status=p->issue_required(s->npa_clients[i],values[i]);op13_trace(32,i*2+1);
  op13_trace(13+i,1);if(EFI_ERROR(s->status)) goto fail;
 }
 s->stage=7; return 0;
fail:
 sm8750_loader_power_release(bs,s); return -1;
}
