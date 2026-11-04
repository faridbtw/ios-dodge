/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sm8750_config_query.c"
static unsigned calls,missing;
static int32_t path(struct cfg_node *n,const char *s)
{ calls++; n->opaque[0]=strstr(s,"_SE_4")?3:strstr(s,"QUP_1")?2:1; return 0; }
static int32_t u8(const struct cfg_node *n,const char *s,uint8_t *v)
{ if(!strcmp(s,"se_index")){assert(n->opaque[0]==3);*v=4;return 0;} assert(n->opaque[0]<3&&!strcmp(s,"num_se")); *v=8; return 0; }
static int32_t u32(const struct cfg_node *n,const char *s,uint32_t *v)
{ if(!strcmp(s,"se_wrapper_base_addr")){assert(n->opaque[0]==2);*v=0xa80000;return 0;} if(!strcmp(s,"common_base_addr")){assert(n->opaque[0]==2);*v=0xac0000;return 0;} assert(!strcmp(s,"core_base_addr")); if(missing&&n->opaque[0]==3) return -1;
 *v=n->opaque[0]==3?0xa90000:0xa80000; return 0; }
static struct cfg_protocol proto;
static efi_status loc(const efi_guid *g,void *r,void **p)
{ (void)g;(void)r; calls++; *p=&proto; return 0; }
int main(void)
{
 efi_boot_services bs={0}; struct sm8750_config_query_report r;
 bs.locate_protocol=loc; proto.path=path; proto.u8=u8; proto.u32=u32;
 assert(sm8750_loader_config_query(&bs,0,&r)==-3&&!calls);
 missing=1; assert(sm8750_loader_config_query(&bs,1,&r)==-6&&!r.matched_a90000);
 assert(r.derived_matched_a90000&&r.derived_base==0xa90000&&r.wrapper_common_base==0xac0000);
 missing=0; assert(sm8750_loader_config_query(&bs,1,&r)==0);
 assert(r.matched_a90000&&r.logical_instance==13&&r.core_base==0xa90000);
 puts("PASS: config getter layouts, missing-property refusal, explicit address matching");
}
