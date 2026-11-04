/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../loader/sm8750_power.c"
static unsigned calls,created,destroyed,voted,missing;
static efi_status create(const char *r,const char *n,uint32_t type,uint32_t v,void *p,void **out)
{
 assert(!strcmp(r,resources[created])); assert(!strcmp(n,"hackios-s3910"));
 assert(type==0x40&&!v&&!p); created++;
 *out=missing&&created==2?0:(void *)(uintptr_t)created; return 0;
}
static efi_status destroy(void *p) { assert(p); destroyed++; return 0; }
static efi_status issue(void *p,uint32_t value)
{ assert((uintptr_t)p==voted+1); assert(value==values[voted]); voted++; return 0; }
static struct npa_protocol proto={0x10003,0,create,destroy,0,issue};
static efi_status loc(const efi_guid *g,void *r,void **p)
{ (void)r; assert(g->a==npa_guid.a); calls++; *p=&proto; return 0; }
int main(void)
{
 efi_boot_services bs={0}; struct sm8750_power_session s={0}; bs.locate_protocol=loc;
 assert(sm8750_loader_power_vote(&bs,0,&s)==-3&&!calls);
 proto.revision=0x10000; assert(sm8750_loader_power_vote(&bs,1,&s)==-3&&!created);
 proto.revision=0x10003; missing=1;
 assert(sm8750_loader_power_vote(&bs,1,&s)==-1&&created==2&&destroyed==1&&!voted);
 created=destroyed=0; missing=0;
 assert(sm8750_loader_power_vote(&bs,1,&s)==0&&created==3&&voted==3&&s.stage==7);
 assert(sm8750_loader_power_vote(&bs,1,&s)==-6);
 sm8750_loader_power_release(&bs,&s); assert(destroyed==3);
 puts("PASS: NPA disabled/exact-ABI/missing resource gates, vote order, lifecycle");
}

void op13_trace(uint32_t s,uint32_t d){(void)s;(void)d;}
